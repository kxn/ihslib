/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <errno.h>

#include "ihslib/session.h"
#include "ihslib/common.h"
#include "base.h"
#include "packet.h"
#include "crypto.h"

#include "session_pri.h"
#include "frame_stats.h"

#include "session/channels/channel.h"
#include "session/channels/ch_discovery.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_stats.h"
#include "session/channels/video/ch_data_video.h"
#include "protobuf/remoteplay.pb-c.h"

#include "hid/manager.h"

/* Long enough to ride out one lost packet and its 10 ms retransmit, short enough
 * that a host which never answers doesn't hold the UI. */
#define STOP_ACK_TIMEOUT_MS 250
#define STOP_ACK_POLL_MS 5
#define SESSION_RECV_TIMEOUT_US 10000

typedef struct IHS_QueueItem {
    IHS_SessionPacket packet;
    bool reliable;
} QueuedPacket;

static void SessionRecvCallback(IHS_Base *base, const IHS_SocketAddress *address, IHS_Buffer *data);

static void SessionInitialized(IHS_Base *base, void *context);

static void SessionFinalized(IHS_Base *base, void *context);

static void SessionSendWorker(void *context);

static QueuedPacket *QueuedPacketCreate(IHS_Session *session, IHS_SessionPacket *packet);

static void QueuedPacketDestroy(QueuedPacket *queued, void *unused);

static const IHS_BaseRunCallbacks SessionRunCallbacks = {
        .initialized = SessionInitialized,
        .finalized = SessionFinalized,
};

IHS_Session *IHS_SessionCreate(const IHS_ClientConfig *clientConfig, const IHS_SessionInfo *sessionInfo) {
    IHS_Session *session = calloc(1, sizeof(IHS_Session));
    IHS_BaseInit(&session->base, clientConfig, SessionRecvCallback, false);
    IHS_BaseSetRunCallbacks(&session->base, &SessionRunCallbacks, NULL);
    session->info = *sessionInfo;
    session->sendQueueMutex = IHS_MutexCreate();
    session->sendQueueCond = IHS_CondCreate();
    session->sendQueue = IHS_QueueCreate(sizeof(QueuedPacket));
    session->timers = IHS_TimerCreate();
    IHS_RetransmissionInit(&session->retransmission, session);
    session->hidManager = IHS_HIDManagerCreate();
    session->frameStats = IHS_FrameStatsAggregatorCreate();
    session->stopPacketId = -1; /* calloc's 0 is a valid packet id */

    // Default the negotiated-streaming flags to true; OnSetClientConfig will reflect the
    // server's actual answer once the SetStreamingClientConfig control message arrives.
    session->state.streamingInput = true;
    session->state.streamingAudio = true;
    session->state.streamingVideo = true;

    session->numChannels = 3;

    session->channels[IHS_SessionChannelIdDiscovery] = IHS_SessionChannelDiscoveryCreate(session);
    session->channels[IHS_SessionChannelIdControl] = IHS_SessionChannelControlCreate(session);
    session->channels[IHS_SessionChannelIdStats] = IHS_SessionChannelStatsCreate(session);
    char *ipStr = IHS_IPAddressToString(&sessionInfo->address.ip);
    IHS_SessionLog(session, IHS_LogLevelInfo, "Session", "Session created. IP address: %s", ipStr);
    free(ipStr);

    session->hidManager->session = session;
    return session;
}


bool IHS_SessionConnect(IHS_Session *session) {
    IHS_SessionLog(session, IHS_LogLevelInfo, "Session", "Starting session thread");
    // After worker ready, send connect packet
    return IHS_BaseStartWorker(&session->base, "IHSSession");
}

/* Poll for the host's ACK of our StopRequest, then drop the transport. This runs
 * on the timer thread on purpose: half of IHS_SessionDisconnect's callers are error
 * paths inside the receive thread, and that is the very thread that must process
 * the ACK. Blocking any of them would guarantee the timeout it is waiting on. */
static uint64_t StopAckTimerRun(int runCount, void *context) {
    IHS_Session *session = context;
    if (session->stopAcked) {
        return 0;
    }
    if ((runCount + 1) * STOP_ACK_POLL_MS >= STOP_ACK_TIMEOUT_MS) {
        IHS_SessionLog(session, IHS_LogLevelWarn, "Session",
                       "Host did not acknowledge StopRequest within %d ms", STOP_ACK_TIMEOUT_MS);
        return 0;
    }
    return STOP_ACK_POLL_MS;
}

static void StopAckTimerEnd(void *context) {
    IHS_Session *session = context;
    session->stopPacketId = -1;
    IHS_SessionChannelDiscoveryDisconnect(IHS_SessionChannelFor(session, IHS_SessionChannelIdDiscovery));
}

void IHS_SessionDisconnect(IHS_Session *session) {
    /* Tell the host we are done before dropping the transport. Without the
     * StopRequest it only sees a client that stopped answering, and leaves the
     * streaming session up until its own timeout: Steam stays in streaming mode,
     * the game keeps running, and the next connection lands mid-session. */
    if (session->stopPacketId >= 0) {
        return; /* already stopping; the error paths call this repeatedly */
    }
    IHS_SessionChannel *control = IHS_SessionChannelFor(session, IHS_SessionChannelIdControl);
    if (control == NULL) {
        IHS_SessionChannelDiscoveryDisconnect(IHS_SessionChannelFor(session, IHS_SessionChannelIdDiscovery));
        return;
    }
    /* A reset snapshot may be coalesced behind an older input packet. Commit it
     * now so reliable channel order is old input -> neutral input -> StopRequest. */
    if (!IHS_SessionChannelControlFlushPendingHID(control)) {
        IHS_SessionLog(session, IHS_LogLevelWarn, "Session",
                       "Failed to flush final HID snapshot before StopRequest");
    }
    session->stopPacketId = control->nextPacketId;
    session->stopAcked = false;
    CStopRequest stop = CSTOP_REQUEST__INIT;
    IHS_SessionChannelControlSend(control, k_EStreamControlStopRequest, (const ProtobufCMessage *) &stop,
                                  IHS_PACKET_ID_NEXT);
    /* Closing the socket now would strand the StopRequest in the send queue, or
     * lose it to the one packet drop this link is entitled to. Disconnect once the
     * host has acknowledged it, or once we stop waiting. */
    IHS_TimerTaskStart(session->timers, StopAckTimerRun, StopAckTimerEnd, 0, session);
}

void IHS_SessionThreadedJoin(IHS_Session *session) {
    IHS_BaseWaitWorker(&session->base);
}

void IHS_SessionDestroy(IHS_Session *session) {
    for (int i = 0; i < session->numChannels; ++i) {
        IHS_SessionChannelDestroy(session->channels[i]);
    }
    IHS_HIDManagerDestroy(session->hidManager);
    IHS_FrameStatsAggregatorDestroy(session->frameStats);
    IHS_TimerDestroy(session->timers);
    IHS_RetransmissionDeinit(&session->retransmission);
    IHS_CondDestroy(session->sendQueueCond);
    IHS_MutexDestroy(session->sendQueueMutex);
    IHS_QueueDestroy(session->sendQueue, QueuedPacketDestroy, NULL);
    IHS_SessionLog(session, IHS_LogLevelInfo, "Session", "Destroying session, bye!");
    IHS_BaseDestroy(&session->base);
    free(session);
}

/*
 * Private functions
 */

void IHS_SessionInterrupt(IHS_Session *session) {
    IHS_BaseInterruptWorker(&session->base);
    IHS_MutexLock(session->sendQueueMutex);
    IHS_CondSignal(session->sendQueueCond);
    IHS_MutexUnlock(session->sendQueueMutex);
    for (int i = session->numChannels - 1; i >= 0; --i) {
        IHS_SessionChannelStop(session->channels[i]);
    }
}

bool IHS_SessionSendPacket(IHS_Session *session, IHS_SessionPacket *packet) {
    const IHS_SessionInfo *config = &session->info;
    // Write header and CRC to the buffer
    IHS_SessionPacketPopulateBuffer(packet);
    // Shallow copied buffer - offset & suffix changes will be temporary
    IHS_Buffer serialized = packet->body;
    IHS_BufferExtendSize(&serialized);

    if (packet->header.retransmitCount > 0) {
        IHS_SessionLog(session, IHS_LogLevelVerbose, "Retransmission",
                       "Send Packet(channelId=%u, packetId=%u, fragmentId=%u), retransmitCount=%u",
                       packet->header.channelId, packet->header.packetId, packet->header.fragmentId,
                       packet->header.retransmitCount);
    }
    return IHS_BaseSend(&session->base, config->address, &serialized);
}

bool IHS_SessionQueuePacket(IHS_Session *session, IHS_SessionPacket *packet, bool retransmit) {
    assert(packet->body.offset == IHS_PACKET_HEADER_SIZE);
    // If the packet has CRC, require 4 bytes extra space at the end of body
    assert(!packet->header.hasCrc || packet->body.suffix == 4);
    /* Register before exposing the initial send to the worker. Otherwise a fast
     * ACK can arrive between sendto() and registration and be lost forever. */
    if (retransmit && !IHS_RetransmissionTrack(&session->retransmission, packet,
                                               IHS_TimerNow())) {
        IHS_SessionLog(session, IHS_LogLevelError, "Retransmission",
                       "Failed to track reliable Packet(channelId=%u, packetId=%u, fragmentId=%d)",
                       packet->header.channelId, packet->header.packetId,
                       packet->header.fragmentId);
        return false;
    }
    IHS_MutexLock(session->sendQueueMutex);
    // Move buffer ownership from packet to QueuedPacket
    QueuedPacket *item = QueuedPacketCreate(session, packet);
    item->reliable = retransmit;

    IHS_QueueAppend(session->sendQueue, item);

    IHS_CondSignal(session->sendQueueCond);
    IHS_MutexUnlock(session->sendQueueMutex);
    return true;
}

bool IHS_SessionSendControlMessage(IHS_Session *session, EStreamControlMessage type, const ProtobufCMessage *message) {
    IHS_SessionChannel *channel = IHS_SessionChannelFor(session, IHS_SessionChannelIdControl);
    return IHS_SessionChannelControlSend(channel, type, message, IHS_PACKET_ID_NEXT);
}

void IHS_SessionHIDAddProvider(IHS_Session *session, IHS_HIDProvider *provider) {
    IHS_BaseLock(&session->base);
    IHS_HIDManagerAddProvider(session->hidManager, provider);
    IHS_BaseUnlock(&session->base);
}

const IHS_SessionInfo *IHS_SessionGetInfo(const IHS_Session *session) {
    return &session->info;
}

void IHS_SessionGetReliabilityStats(IHS_Session *session,
                                    IHS_SessionReliabilityStats *stats) {
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->hidOldestInFlightPacketId = -1;
    if (session == NULL) {
        return;
    }
    IHS_RetransmissionStats reliable;
    IHS_RetransmissionGetStats(&session->retransmission, &reliable, IHS_TimerNow());
    stats->reliableTracked = reliable.tracked;
    stats->reliableAcknowledged = reliable.acknowledged;
    stats->reliableSuperseded = reliable.superseded;
    stats->reliableGiveUps = reliable.giveUps;
    stats->reliableNacks = reliable.nacks;
    stats->reliableRetries = reliable.retries;
    stats->reliableSendFailures = reliable.sendFailures;
    stats->reliableOutstanding = reliable.outstanding;
    stats->reliableOldestOutstandingMs = reliable.oldestOutstandingMs;
    stats->reliableOldestChannelId = reliable.oldestChannelId;
    stats->reliableOldestPacketId = reliable.oldestPacketId;
    stats->reliableOldestFragmentId = reliable.oldestFragmentId;
    stats->reliableOldestRetryCount = reliable.oldestRetryCount;
    stats->reliableMaxAckLatencyMs = reliable.maxAckLatencyMs;

    IHS_SessionChannel *channel = IHS_SessionChannelFor(session, IHS_SessionChannelIdControl);
    if (channel != NULL) {
        IHS_SessionChannelControl *control = (IHS_SessionChannelControl *) channel;
        IHS_MutexLock(control->sendLock);
        stats->hidSubmitted = control->hidSubmitted;
        stats->hidCoalesced = control->hidCoalesced;
        stats->hidSent = control->hidSent;
        stats->hidAcknowledged = control->hidAcknowledged;
        stats->hidSuperseded = control->hidSuperseded;
        stats->hidPending = 0U;
        stats->hidInFlight = 0U;
        stats->hidOldestInFlightPacketId = -1;
        IHS_MutexUnlock(control->sendLock);
    }
}

static void SessionRecvCallback(IHS_Base *base, const IHS_SocketAddress *address, IHS_Buffer *data) {
    (void) address;
    IHS_Session *session = (IHS_Session *) base;
    IHS_SessionPacket packet;
    IHS_SessionPacketReturn ret = IHS_SessionPacketParse(&packet, data);
    if (ret != IHS_SessionPacketResultOK) {
        IHS_SessionLog(session, IHS_LogLevelDebug, "Session", "Discarding packet. Reason: %u", ret);
        return;
    }

    IHS_SessionChannelId channelId = packet.header.channelId;
    IHS_SessionPacketType packetType = packet.header.type;
    if (packetType == IHS_SessionPacketTypeACK) {
        IHS_RetransmissionAcknowledge(&session->retransmission, channelId,
                                     packet.header.packetId, packet.header.fragmentId,
                                     IHS_TimerNow());
        if (channelId == IHS_SessionChannelIdControl &&
            (int32_t) packet.header.packetId == session->stopPacketId) {
            session->stopAcked = true; /* releases IHS_SessionDisconnect */
        }
    } else if (packetType == IHS_SessionPacketTypeNACK) {
        IHS_RetransmissionNack(&session->retransmission, channelId,
                               packet.header.packetId, packet.header.fragmentId,
                               IHS_TimerNow());
    }
    IHS_SessionChannel *channel = IHS_SessionChannelFor(session, channelId);
    if (channel == NULL && session->negotiatedVideoCodec != 0 /* not None */ &&
        packetType != IHS_SessionPacketTypeACK && packetType != IHS_SessionPacketTypeNACK &&
        IHS_SessionChannelForType(session, IHS_SessionChannelTypeDataVideo) == NULL) {
        /* Some hosts (e.g. desktop streaming) start sending video on its data
         * channel without ever emitting k_EStreamControlStartVideoData. Create
         * the video channel on demand from the negotiated codec/capture size so
         * the stream can actually be decoded. */
        CStartVideoDataMsg msg = CSTART_VIDEO_DATA_MSG__INIT;
        msg.channel = channelId;
        msg.has_codec = true;
        msg.codec = session->negotiatedVideoCodec;
        msg.has_width = true;
        msg.width = session->captureWidth ? session->captureWidth : 1920;
        msg.has_height = true;
        msg.height = session->captureHeight ? session->captureHeight : 1080;
        channel = IHS_SessionChannelDataVideoCreate(session, &msg);
        IHS_SessionChannelAdd(session, channel);
        IHS_SessionLog(session, IHS_LogLevelInfo, "Session",
                       "Created video channel %u on demand (codec=%d, %ux%u)",
                       channelId, msg.codec, msg.width, msg.height);
    }
    if (channel != NULL) {
        IHS_SessionChannelReceivedPacket(channel, &packet);
    } else {
        IHS_SessionLog(session, IHS_LogLevelDebug, "Session", "Unknown channel for packet(type=%u, ch=%u)", packetType,
                       channelId);
    }
    IHS_SessionPacketClear(&packet, true);
}

static void SessionInitialized(IHS_Base *base, void *context) {
    (void) context;
    IHS_Session *session = (IHS_Session *) base;
    /* Interrupting a worker only changes base.interrupted; it cannot wake a
     * blocking recvfrom(). Once StopRequest makes the host go quiet, join must
     * still observe the interrupt without waiting for another datagram. */
    if (!IHS_UDPSocketSetRecvTimeout(base->socket, SESSION_RECV_TIMEOUT_US)) {
        IHS_SessionLog(session, IHS_LogLevelWarn, "Session",
                       "Failed to set session receive timeout");
    }
    session->sendThread = IHS_ThreadCreate(SessionSendWorker, "IHSSessSend", session);

    if (session->callbacks.session && session->callbacks.session->initialized) {
        session->callbacks.session->initialized(session, session->callbackContexts.session);
    }

    if (session->callbacks.session && session->callbacks.session->connecting) {
        session->callbacks.session->connecting(session, session->callbackContexts.session);
    }

    IHS_BaseLock(&session->base);
    session->state.connectionId = IHS_CryptoRandomUInt32();
    session->state.connectionState = IHS_SessionConnectionStateConnecting;
    IHS_BaseUnlock(&session->base);

    /* crc32c(b'Connect') */
    const static uint8_t body[4] = {0xc7, 0x3d, 0x8f, 0x3c};

    IHS_SessionChannel *discovery = IHS_SessionChannelFor(session, IHS_SessionChannelIdDiscovery);
    IHS_SessionPacket packet;
    IHS_SessionChannelInitializePacket(discovery, &packet, IHS_SessionPacketTypeConnect, true, 0);
    IHS_BufferAppendMem(&packet.body, body, sizeof(body));
    IHS_SessionQueuePacket(session, &packet, true);
    IHS_SessionPacketClear(&packet, true);
}

static void SessionFinalized(IHS_Base *base, void *context) {
    (void) context;
    IHS_Session *session = (IHS_Session *) base;
    IHS_ThreadJoin(session->sendThread);
    session->sendThread = NULL;
    if (session->callbacks.session && session->callbacks.session->finalized) {
        session->callbacks.session->finalized(session, session->callbackContexts.session);
    }
}

static void SessionSendWorker(void *context) {
    IHS_Session *session = (IHS_Session *) context;
    while (!session->base.interrupted) {
        IHS_MutexLock(session->sendQueueMutex);
        QueuedPacket *queued;
        // Poll the first item in the queue
        while ((queued = IHS_QueuePoll(session->sendQueue)) == NULL) {
            // Wait till someone add item into the queue
            IHS_CondWait(session->sendQueueCond, session->sendQueueMutex);
            if (session->base.interrupted) {
                IHS_MutexUnlock(session->sendQueueMutex);
                return;
            }
        }
        IHS_MutexUnlock(session->sendQueueMutex);

        bool sent = IHS_SessionSendPacket(session, &queued->packet);
        if (!sent) {
            IHS_SessionLog(session, IHS_LogLevelWarn, "Session",
                           "Failed to send Packet(channelId=%u, packetId=%u, fragmentId=%u): %s",
                           queued->packet.header.channelId, queued->packet.header.packetId,
                           queued->packet.header.fragmentId, strerror(errno));
        }

        if (queued->reliable) {
            IHS_RetransmissionNoteInitialSend(&session->retransmission,
                                              &queued->packet.header, sent,
                                              IHS_TimerNow());
        }
        QueuedPacketDestroy(queued, NULL);
        IHS_QueueItemFree(queued);
    }
}

static QueuedPacket *QueuedPacketCreate(IHS_Session *session, IHS_SessionPacket *packet) {
    QueuedPacket *item = IHS_QueueItemObtain(session->sendQueue);
    item->packet.header = packet->header;
    item->packet.crc = packet->crc;
    IHS_BufferTransferOwnership(&packet->body, &item->packet.body);
    return item;
}

static void QueuedPacketDestroy(QueuedPacket *queued, void *unused) {
    (void) unused;
    IHS_SessionPacketClear(&queued->packet, true);
}

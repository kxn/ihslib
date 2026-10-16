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

#include <errno.h>
#include <memory.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "crypto.h"
#include "ihslib/common.h"
#include "ihslib/session.h"
#include "packet.h"

#include "frame_stats.h"
#include "frame_tracker.h"
#include "session_pri.h"

#include "protobuf/remoteplay.pb-c.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_discovery.h"
#include "session/channels/ch_stats.h"
#include "session/channels/channel.h"
#include "session/channels/video/ch_data_video.h"

#include "hid/manager.h"

/* Bounded receive wait lets the existing worker advance transport deadlines. */
#define SESSION_RECV_TIMEOUT_US 10000

typedef struct IHS_QueueItem {
    IHS_SessionPacket packet;
    bool reliable;
    uint32_t queuedAt;
} QueuedPacket;

static void SessionRecvCallback(IHS_Base *base, const IHS_SocketAddress *address, IHS_Buffer *data);

static void SessionInitialized(IHS_Base *base, void *context);

static void SessionFinalized(IHS_Base *base, void *context);
static void SessionUpdate(IHS_Base *base, void *context);

static void SessionSendWorker(void *context);

static QueuedPacket *QueuedPacketCreate(IHS_Session *session, IHS_SessionPacket *packet);

static void QueuedPacketDestroy(QueuedPacket *queued, void *unused);

static const IHS_BaseRunCallbacks SessionRunCallbacks = {
    .initialized = SessionInitialized,
    .finalized = SessionFinalized,
    .update = SessionUpdate,
};

IHS_Session *IHS_SessionCreate(const IHS_ClientConfig *clientConfig,
                               const IHS_SessionInfo *sessionInfo) {
    IHS_Session *session = calloc(1, sizeof(IHS_Session));
    IHS_BaseInit(&session->base, clientConfig, SessionRecvCallback, false);
    IHS_BaseSetRunCallbacks(&session->base, &SessionRunCallbacks, NULL);
    session->info = *sessionInfo;
    atomic_init(&session->state.connectionState, IHS_SessionConnectionStateUnconnected);
    atomic_init(&session->state.mtu, 1024);
    atomic_init(&session->state.connectionId, 0);
    atomic_init(&session->state.hostConnectionId, 0);
    atomic_init(&session->state.crcRequired, false);
    atomic_init(&session->transport.lifecycleGeneration, 0);
    session->sendQueueMutex = IHS_MutexCreate();
    session->sendQueueCond = IHS_CondCreate();
    session->sendQueue = IHS_QueueCreate(sizeof(QueuedPacket));
    session->pendingData = IHS_QueueCreate(sizeof(QueuedPacket));
    session->timers = IHS_TimerCreate();
    IHS_StreamClockInit(&session->clock);
    IHS_OutgoingInit(session, IHS_SessionPacketTimestamp());
    IHS_RetransmissionInit(&session->retransmission, session);
    session->hidManager = IHS_HIDManagerCreate();
    session->frameStats = IHS_FrameStatsAggregatorCreate();
    atomic_init(&session->hostRequestedStop, false);
    atomic_init(&session->hostVideoStopped, false);

    // Protocol defaults until replaced by a complete config.
    atomic_init(&session->state.streamingInput, true);
    atomic_init(&session->state.streamingAudio, true);
    atomic_init(&session->state.streamingVideo, true);

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

/* Transport disconnect preserves the host game. StopRequest(129) is the
 * separate official "stop game" action; never send it during generic cleanup. */
void IHS_SessionDisconnect(IHS_Session *session) {
    IHS_SessionChannelDiscoveryDisconnect(
        IHS_SessionChannelFor(session, IHS_SessionChannelIdDiscovery));
}

bool IHS_SessionStopGame(IHS_Session *session) {
    if (!session)
        return false;
    IHS_BaseLock(&session->base);
    bool connected = session->state.connectionState == IHS_SessionConnectionStateConnected;
    bool sent = false;
    if (connected) {
        CStopRequest request = CSTOP_REQUEST__INIT;
        sent = IHS_SessionSendControlMessage(session, k_EStreamControlStopRequest,
                                             (const ProtobufCMessage *)&request);
    }
    IHS_BaseUnlock(&session->base);
    return sent;
}

void IHS_SessionThreadedJoin(IHS_Session *session) {
    IHS_BaseWaitWorker(&session->base);
}

void IHS_SessionDestroy(IHS_Session *session) {
    session->destroying = true;
    if (session->hidManager->pollTimer) {
        IHS_TimerTaskStopImmediate(session->hidManager->pollTimer);
        session->hidManager->pollTimer = NULL;
    }
    IHS_SessionInterrupt(session);
    if (session->base.worker)
        IHS_SessionThreadedJoin(session);
    IHS_HIDManagerCloseAll(session->hidManager);
    /* Data workers may still call control/stats on stop: destroy them first. */
    for (int i = session->numChannels - 1; i >= 3; --i) {
        IHS_SessionChannelDestroy(session->channels[i]);
        session->channels[i] = NULL;
    }
    session->numChannels = 3;
    /* Discovery shutdown closes HID devices using the still-live control channel. */
    for (int i = 0; i < 3; i++)
        IHS_SessionChannelDestroy(session->channels[i]);
    IHS_HIDManagerDestroy(session->hidManager);
    if (session->frameTracker)
        IHS_FrameTrackerClose(session->frameTracker);
    IHS_FrameStatsAggregatorDestroy(session->frameStats);
    IHS_TimerDestroy(session->timers);
    IHS_RetransmissionDeinit(&session->retransmission);
    IHS_OutgoingDeinit(session);
    IHS_StreamClockDeinit(&session->clock);
    if (session->clientConfig)
        cstreaming_client_config__free_unpacked(session->clientConfig, NULL);
    IHS_CondDestroy(session->sendQueueCond);
    IHS_MutexDestroy(session->sendQueueMutex);
    IHS_QueueDestroy(session->sendQueue, QueuedPacketDestroy, NULL);
    IHS_QueueDestroy(session->pendingData, QueuedPacketDestroy, NULL);
    IHS_SessionLog(session, IHS_LogLevelInfo, "Session", "Destroying session, bye!");
    free((void *)session->clientCapabilities.systemInfo);
    free((void *)session->clientCapabilities.decoderInfo);
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
    return IHS_SessionSendPacketResult(session, packet) == 0;
}
int IHS_SessionSendPacketResult(IHS_Session *session, IHS_SessionPacket *packet) {
    IHS_BaseLock(&session->base);
    const IHS_SessionInfo *config = &session->info;
    /* Transport clock is stamped at the actual send attempt, including retries. */
    packet->header.sendTimestamp = IHS_SessionPacketTimestamp();
    if (!IHS_OutgoingCanSend(session, packet->header.type, packet->header.sendTimestamp)) {
        IHS_BaseUnlock(&session->base);
        return 1;
    }
    uint32_t charged = IHS_PACKET_HEADER_SIZE + packet->body.size;
    if (session->state.crcRequired && !packet->header.hasCrc) {
        packet->header.hasCrc = true;
        IHS_BufferSetSuffixLength(&packet->body, 4);
    }
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
    bool sent = IHS_BaseSend(&session->base, config->address, &serialized);
    int result = sent ? 0 : errno == EAGAIN || errno == EWOULDBLOCK ? 1 : 2;
    if (sent) IHS_OutgoingSent(session, packet->header.type, packet->header.sendTimestamp, charged);
    IHS_BaseUnlock(&session->base);
    return result;
}

bool IHS_SessionQueuePacket(IHS_Session *session, IHS_SessionPacket *packet, bool retransmit) {
    assert(packet->body.offset == IHS_PACKET_HEADER_SIZE);
    // If the packet has CRC, require 4 bytes extra space at the end of body
    assert(!packet->header.hasCrc || packet->body.suffix == 4);
    IHS_MutexLock(session->sendQueueMutex);
    if (packet->header.autoPacketId) {
        bool reliable = packet->header.type == IHS_SessionPacketTypeReliable ||
                        packet->header.type == IHS_SessionPacketTypeReliableFrag;
        packet->header.packetId = IHS_OutgoingNextId(session, packet->header.channelId, reliable);
        packet->header.autoPacketId = false;
    }
    /* Register before exposing the initial send to the worker. Otherwise a fast
     * ACK can arrive between sendto() and registration and be lost forever. */
    if (retransmit && !IHS_RetransmissionTrack(&session->retransmission, packet, IHS_TimerNow())) {
        IHS_MutexUnlock(session->sendQueueMutex);
        IHS_SessionLog(session, IHS_LogLevelError, "Retransmission",
                       "Failed to track reliable Packet(channelId=%u, packetId=%u, fragmentId=%d)",
                       packet->header.channelId, packet->header.packetId,
                       packet->header.fragmentId);
        return false;
    }
    // Move buffer ownership from packet to QueuedPacket
    QueuedPacket *item = QueuedPacketCreate(session, packet);
    item->reliable = retransmit;
    item->queuedAt = IHS_SessionPacketTimestamp();

    IHS_QueueAppend(session->sendQueue, item);

    IHS_CondSignal(session->sendQueueCond);
    IHS_MutexUnlock(session->sendQueueMutex);
    return true;
}

bool IHS_SessionSendControlMessage(IHS_Session *session, EStreamControlMessage type,
                                   const ProtobufCMessage *message) {
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

void IHS_SessionGetReliabilityStats(IHS_Session *session, IHS_SessionReliabilityStats *stats) {
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
        IHS_SessionChannelControl *control = (IHS_SessionChannelControl *)channel;
        IHS_MutexLock(control->sendLock);
        stats->hidSubmitted = control->hidSubmitted;
        stats->hidCoalesced = control->hidCoalesced;
        stats->hidSent = control->hidSent;
        stats->hidAcknowledged = reliable.hidAcknowledged;
        stats->hidSuperseded = 0;
        stats->hidPending = reliable.hidPending;
        stats->hidInFlight = reliable.hidInFlight;
        stats->hidOldestInFlightPacketId = reliable.hidOldestInFlightPacketId;
        IHS_MutexUnlock(control->sendLock);
    }
}

bool IHS_SessionHostVideoStopped(IHS_Session *session) {
    return session && atomic_load(&session->hostVideoStopped);
}

bool IHS_SessionHostRequestedStop(IHS_Session *session) {
    return session && atomic_load(&session->hostRequestedStop);
}

void IHS_SessionHostStopped(IHS_Session *session) {
    atomic_store(&session->hostRequestedStop, true);
    IHS_SessionLog(session, IHS_LogLevelWarn, "Session", "Host stopped the session");
    /* Best-effort goodbye: bounded, never blocks on the dead peer. */
    IHS_SessionDisconnect(session);
    IHS_BaseLock(&session->base);
    bool notify =
        session->callbacks.session != NULL && session->callbacks.session->disconnected != NULL;
    IHS_BaseUnlock(&session->base);
    if (notify) {
        session->callbacks.session->disconnected(session, session->callbackContexts.session);
    }
    /* Belt and braces: the worker must exit even if the timer path stalls. */
    IHS_SessionInterrupt(session);
}

static void SessionRecvCallback(IHS_Base *base, const IHS_SocketAddress *address,
                                IHS_Buffer *data) {
    IHS_Session *session = (IHS_Session *)base;
    IHS_SessionPacket packet;
    IHS_SessionPacketReturn ret = IHS_SessionPacketParse(&packet, data);
    if (ret != IHS_SessionPacketResultOK) {
        IHS_SessionLog(session, IHS_LogLevelDebug, "Session", "Discarding packet. Reason: %u", ret);
        return;
    }

    /* Socket HandleMessage 0x7ff660..0x7ff6f0 dispatches Unconnected discovery
     * before connection-ID checks: MTU probes have zero IDs after Connected.
     * HandlePacket 0x7faff0..0x7fb028 checks IDs only for connected traffic. */
    if (IHS_IPAddressCompare(&address->ip, &session->info.address.ip) != 0 ||
        address->port != session->info.address.port ||
        (packet.header.type != IHS_SessionPacketTypeUnconnected &&
         packet.header.dstConnectionId != session->state.connectionId) ||
        (packet.header.type == IHS_SessionPacketTypeUnconnected &&
         packet.header.channelId != IHS_SessionChannelIdDiscovery) ||
        (packet.header.type != IHS_SessionPacketTypeUnconnected &&
         session->state.connectionState >= IHS_SessionConnectionStateHandshaking &&
         packet.header.type != IHS_SessionPacketTypeConnectACK &&
         packet.header.srcConnectionId != session->state.hostConnectionId)) {
        IHS_SessionPacketClear(&packet, true);
        return;
    }
    packet.header.receiveTimestamp = IHS_SessionPacketTimestamp();
    IHS_SessionChannelId channelId = packet.header.channelId;
    IHS_SessionPacketType packetType = packet.header.type;
    bool special = packetType == IHS_SessionPacketTypeUnconnected ||
                   packetType == IHS_SessionPacketTypeConnect ||
                   packetType == IHS_SessionPacketTypeConnectACK ||
                   packetType == IHS_SessionPacketTypeDisconnect;
    if ((!special && (channelId >= 32 ||
                      (session->state.crcRequired && !packet.header.hasCrc))) ||
        session->transport.timedOut) {
        IHS_SessionPacketClear(&packet, true);
        return;
    }
    /* Transport acceptance precedes Connected's body/state guard and a
     * channel's payload validation. Discovery probes do not keep it alive. */
    if (packetType != IHS_SessionPacketTypeUnconnected)
        session->transport.lastReceive = packet.header.receiveTimestamp;
    /* Connection::HandlePacket calls GetChannel after CRC/ID/admission and
     * before the channel ignores unknown types (0x7fb164..c0). */
    if (!special && session->state.connectionState >= IHS_SessionConnectionStateHandshaking)
        IHS_OutgoingRegister(session, channelId);
    if (packetType >= IHS_SessionPacketTypeMax) {
        IHS_SessionPacketClear(&packet, true);
        return;
    }
    if (packetType == IHS_SessionPacketTypeConnectACK) {
        IHS_SessionChannelReceivedPacket(session->channels[IHS_SessionChannelIdDiscovery], &packet);
        IHS_SessionPacketClear(&packet, true);
        return;
    }
    if (packetType == IHS_SessionPacketTypeUnreliableNack) {
        if (session->state.connectionState >= IHS_SessionConnectionStateHandshaking)
            IHS_OutgoingNack(session, &packet);
        IHS_SessionPacketClear(&packet, true);
        return;
    }
    if (packetType == IHS_SessionPacketTypeACK) {
        if (packet.body.size < 4) {
            IHS_SessionPacketClear(&packet, true);
            return;
        }
        const uint8_t *b = IHS_BufferPointer(&packet.body);
        uint32_t echo =
            (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        IHS_StreamClockFeedback(&session->clock, echo, packet.header.sendTimestamp,
                                IHS_SessionPacketTimestamp());
    }
    if (packetType == IHS_SessionPacketTypeACK) {
        /* Official ACK semantics: the value is the contiguous delivery point —
         * it confirms every packet at/below it (libmain 0x7f94b8 advances its
         * send window through min(ack+1-head, ...)). Exact-match ACK handling
         * retains packets already covered by a later cumulative ACK, causing
         * needless retries. A causal link to the historical 20 s input stalls
         * cannot be established from this client-side branch alone. */
        IHS_RetransmissionAcknowledgeThrough(&session->retransmission, channelId,
                                             (uint16_t)(packet.header.packetId + 1u),
                                             IHS_TimerNow());

    } else if (packetType == IHS_SessionPacketTypeDisconnect) {
        /* Host quit the game / tore the session down. Without this the client
         * sat in a dead session forever: no video, dead inputs, no exit.
         * (Hardware freeze 2026-09-04.) */
        if (session->state.connectionState >= IHS_SessionConnectionStateHandshaking)
            IHS_SessionHostStopped(session);
        IHS_SessionPacketClear(&packet, true);
        return;
    }
    IHS_SessionChannel *channel = IHS_SessionChannelFor(session, channelId);
    if (channel == NULL && channelId >= IHS_SessionChannelIdDataStart &&
        (packetType == IHS_SessionPacketTypeUnreliable ||
         packetType == IHS_SessionPacketTypeUnreliableFrag ||
         packetType == IHS_SessionPacketTypeUnreliableFEC)) {
        /* OnDataPacket queues early data; StartVideoData supplies codec/channel
         * identity before HandlePendingDataPackets (0x7ae1f4). Do not guess. */
        if (session->pendingDataCount == 320) {
            QueuedPacket *old = (void *)IHS_QueuePoll(session->pendingData);
            IHS_SessionPacketClear(&old->packet, true);
            IHS_QueueItemFree((void *)old);
            session->pendingDataCount--;
        }
        QueuedPacket *queued = (void *)IHS_QueueItemObtain(session->pendingData);
        queued->packet = packet;
        memset(&packet.body, 0, sizeof(packet.body));
        IHS_QueueAppend(session->pendingData, (void *)queued);
        session->pendingDataCount++;
    }
    if (channel != NULL) {
        IHS_SessionChannelReceivedPacket(channel, &packet);
    } else {
        IHS_SessionLog(session, IHS_LogLevelDebug, "Session",
                       "Unknown channel for packet(type=%u, ch=%u)", packetType, channelId);
    }
    IHS_SessionPacketClear(&packet, true);
}

static bool PendingDataMatches(IHS_QueueItem *item, void *context) {
    return ((QueuedPacket *)item)->packet.header.channelId == ((IHS_SessionChannel *)context)->id;
}
void IHS_SessionDrainPendingData(IHS_Session *session, IHS_SessionChannel *channel) {
    QueuedPacket *queued;
    while ((queued = (void *)IHS_QueuePollBy(session->pendingData, PendingDataMatches, channel))) {
        session->pendingDataCount--;
        IHS_SessionChannelReceivedPacket(channel, &queued->packet);
        IHS_SessionPacketClear(&queued->packet, true);
        IHS_QueueItemFree((void *)queued);
    }
}

static void SessionInitialized(IHS_Base *base, void *context) {
    (void)context;
    IHS_Session *session = (IHS_Session *)base;
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
    session->state.hostConnectionId = 0;
    session->state.crcRequired = false;
    IHS_SessionTransportBegin(session, IHS_SessionPacketTimestamp());
    IHS_SessionTransportSendConnect(session);
    IHS_BaseUnlock(&session->base);
}

static void SessionFinalized(IHS_Base *base, void *context) {
    (void)context;
    IHS_Session *session = (IHS_Session *)base;
    /* A fatal receive error may end BaseWorker without a requested stop.
     * Wake the send worker before joining it in that path too. */
    IHS_SessionInterrupt(session);
    IHS_ThreadJoin(session->sendThread);
    session->sendThread = NULL;
    if (session->callbacks.session && session->callbacks.session->finalized) {
        session->callbacks.session->finalized(session, session->callbackContexts.session);
    }
}

static void SessionUpdate(IHS_Base *base, void *context) {
    (void)context;
    IHS_SessionTransportUpdate((IHS_Session *)base, IHS_SessionPacketTimestamp());
}

void IHS_SessionWakeSend(IHS_Session *session) {
    IHS_MutexLock(session->sendQueueMutex);
    IHS_CondSignal(session->sendQueueCond);
    IHS_MutexUnlock(session->sendQueueMutex);
}
static void SessionSendWorker(void *context) {
    IHS_Session *session = context;
    uint32_t lastUpdate = IHS_SessionPacketTimestamp();
    while (!session->base.interrupted) {
        IHS_MutexLock(session->sendQueueMutex);
        QueuedPacket *queued = (void *)IHS_QueuePoll(session->sendQueue);
        if (!queued && !session->base.interrupted) {
            IHS_CondWaitTimeout(session->sendQueueCond, session->sendQueueMutex, 1);
            queued = (void *)IHS_QueuePoll(session->sendQueue);
        }
        IHS_MutexUnlock(session->sendQueueMutex);
        if (session->base.interrupted) {
            if (queued) { QueuedPacketDestroy(queued, NULL); IHS_QueueItemFree((void *)queued); }
            break;
        }
        bool hadQueued = queued != NULL;
        if (queued) {
            unsigned type = queued->packet.header.type;
            if (type == IHS_SessionPacketTypeUnreliable || type == IHS_SessionPacketTypeUnreliableFrag) {
                uint8_t ch = queued->packet.header.channelId;
                if (IHS_OutgoingRetain(session, &queued->packet, queued->queuedAt))
                    IHS_OutgoingSendUnreliable(session, ch, false);
                else
                    IHS_SessionLog(session, IHS_LogLevelError, "Send", "Cannot retain outgoing channel %u packet", ch);
            } else {
                bool sent = IHS_SessionSendPacket(session, &queued->packet);
                if (queued->reliable)
                    IHS_RetransmissionNoteInitialSend(&session->retransmission, &queued->packet.header,
                                                      sent, IHS_TimerNow());
            }
            QueuedPacketDestroy(queued, NULL);
            IHS_QueueItemFree((void *)queued);
        }
        uint32_t now = IHS_SessionPacketTimestamp();
        if (!hadQueued || (uint32_t)(now - lastUpdate) >= 65) {
            if (session->state.connectionState >= IHS_SessionConnectionStateHandshaking)
                IHS_OutgoingUpdate(session, now);
            lastUpdate = now;
        }
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
    (void)unused;
    IHS_SessionPacketClear(&queued->packet, true);
}

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

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "ch_data_video.h"
#include "partial_frames.h"
#include "session/channels/ch_data.h"

#include "ihs_timer.h"

#include "crypto.h"
#include "endianness.h"
#include "protobuf/pb_utils.h"
#include "session/channels/ch_stats.h"
#include "session/frame_stats.h"
#include "session/frame_tracker.h"
#include "session/packet.h"
#include "session/session_pri.h"

#include "frame_h264.h"
#include "frame_hevc.h"

typedef struct IHS_SessionChannelVideo {
    IHS_SessionChannelData base;
    IHS_StreamVideoConfig config;
    struct {
        uint16_t expectedSequence;
        uint16_t lastFrameId;
        uint32_t lastRecoveryRequest;
        bool waitingKeyFrame, syncingBacklog;
        int32_t overflowFrameSpan;
        bool frameStarted;
        bool frameFinished;
        /* Same-object Reset preserves the mode; a new decoder starts unseen. */
        bool contentModeSeen, contentMode;
    } states;
    struct {
        uint16_t expectedSubFrameStart;
        IHS_VideoPartialFrames partial;
        IHS_Buffer buffer;
        IHS_StreamVideoFrameFlag flags;
        bool contentMode;
        /* Pending-reset complete frames retain their ownership until the actor
         * or a key mark purges them. Capacity is a local tracker bound. */
        IHS_FrameTicket *deferred[IHS_FRAME_TRACKER_CAPACITY];
        size_t deferredCount;
    } frame;
    bool appStarted;
    bool prepared, backendReady;
    atomic_bool resetRequested;
    IHS_VideoEpochInfo epoch;
    IHS_FrameTicket *incoming, *assembly;
    uint16_t assemblyId;
    bool tracked, contributed;
    IHS_Mutex *stateMutex;
} IHS_SessionChannelVideo;

static void ChannelVideoInit(IHS_SessionChannel *channel, const void *config);

static void ChannelVideoDeinit(IHS_SessionChannel *channel);

static bool DataStart(IHS_SessionChannel *channel);

static void DataReceived(IHS_SessionChannel *channel, const IHS_SessionDataFrameHeader *header,
                         IHS_Buffer *body);

static void DataStop(IHS_SessionChannel *channel);

static size_t VideoFrameHeaderParse(IHS_VideoFrameHeader *header, const uint8_t *data);

/**
 * Assemble one frame in the partial frames list
 * @param channel Channel instance
 * @return true if the frame is ready
 */
static bool AssembleFrame(IHS_SessionChannel *channel);

static bool AppendToFrameBuffer(IHS_SessionChannelVideo *channel, const IHS_Buffer *data,
                                const IHS_VideoFrameHeader *header);

static IHS_StreamVideoSubmitResult SubmitFrame(IHS_SessionChannel *channel, uint16_t frameId,
                                               IHS_Buffer *data, IHS_StreamVideoFrameFlag flags);

/**
 * Append one data frame into partial video frames list
 * @param channel Channel instance
 * @param data Data frame body
 * @param header Data frame header
 */
static void AddPartialFrame(IHS_SessionChannelVideo *channel, uint16_t frameId, uint32_t timestamp,
                            const IHS_VideoFrameHeader *header, IHS_Buffer *data);

/**
 * Report pending frame IDs; >150ms requests synchronization but still accepts
 * sequence-continuous data (Android ARM64 CheckOverflow 0x7b2854).
 * @param channel Channel instance
 */
static void CheckPartialOverflow(IHS_SessionChannel *channel);

/**
 * Clear partial video frames and not yet assembled frame data
 * @param channel
 */
static void DiscardPending(IHS_SessionChannelVideo *channel);

static const IHS_SessionChannelDataClass ChannelClass = {
    {.init = ChannelVideoInit,
     .deinit = ChannelVideoDeinit,
     .received = IHS_SessionChannelDataReceived,
     .stopped = IHS_SessionChannelDataStopped,
     .instanceSize = sizeof(IHS_SessionChannelVideo)},
    .start = DataStart,
    .dataFrame = DataReceived,
    .stop = DataStop,
    .startOnCaller = true,
};

static const uint8_t EmptyIV[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

IHS_SessionChannel *IHS_SessionChannelDataVideoCreate(IHS_Session *session,
                                                      const CStartVideoDataMsg *message) {
    return IHS_SessionChannelDataCreate(&ChannelClass, session, IHS_SessionChannelTypeDataVideo,
                                        message->channel, (void *)message);
}

static void ChannelVideoInit(IHS_SessionChannel *channel, const void *config) {
    IHS_SessionChannelVideo *videoCh = (IHS_SessionChannelVideo *)channel;
    const CStartVideoDataMsg *message = config;
    videoCh->config.width = message->width;
    videoCh->config.height = message->height;
    videoCh->config.codec = (IHS_StreamVideoCodec)message->codec;
    atomic_init(&videoCh->resetRequested, false);
    if (message->has_codec_data && message->codec_data.len) {
        if (message->codec_data.len > 2u * 1024u * 1024u || !message->codec_data.data) {
            channel->initFailed = true;
            return;
        }
        videoCh->config.codecDataLen = message->codec_data.len;
        videoCh->config.codecData = malloc(videoCh->config.codecDataLen);
        if (!videoCh->config.codecData) {
            channel->initFailed = true;
            return;
        }
        memcpy(videoCh->config.codecData, message->codec_data.data, message->codec_data.len);
    }
    videoCh->stateMutex = IHS_MutexCreate();
    IHS_BufferInit(&videoCh->frame.buffer, 128 * 1024 /*128KB*/, 2048 * 1024 /*2MB*/);
    videoCh->frame.buffer.data = malloc(videoCh->frame.buffer.maxCapacity);
    if (videoCh->frame.buffer.data)
        videoCh->frame.buffer.capacity = videoCh->frame.buffer.maxCapacity;
    if (!videoCh->stateMutex || !videoCh->frame.buffer.data) {
        channel->initFailed = true;
        return;
    }
    IHS_VideoPartialFramesInit(&videoCh->frame.partial);
    videoCh->tracked =
        channel->session->callbacks.video && channel->session->callbacks.video->startTracked;
    videoCh->epoch.session_id = channel->session->videoTrackingId;
    if (videoCh->tracked && channel->session->nextVideoEpoch != UINT64_MAX)
        videoCh->epoch.video_epoch = ++channel->session->nextVideoEpoch;
    IHS_SessionChannelDataInit(channel, 2048);
}

static void ChannelVideoDeinit(IHS_SessionChannel *channel) {
    IHS_SessionChannelDataDeinit(channel);
    /* The worker is joined; shared rollback is now safe and idempotent. */
    DataStop(channel);
    IHS_SessionChannelVideo *videoCh = (IHS_SessionChannelVideo *)channel;
    IHS_MutexDestroy(videoCh->stateMutex);
    if (videoCh->config.codecData) {
        free(videoCh->config.codecData);
    }
    IHS_BufferClear(&videoCh->frame.buffer, true);
    IHS_VideoPartialFramesClear(&videoCh->frame.partial);
}

static bool DataStart(IHS_SessionChannel *channel) {
    IHS_SessionChannelVideo *videoCh = (IHS_SessionChannelVideo *)channel;
    IHS_Session *session = channel->session;
    const IHS_StreamVideoCallbacks *callbacks = session->callbacks.video;
    if (session->invalidVideoCallbacks) {
        IHS_SessionDisconnect(session); /* Invalid local callback/ownership contract. */
        return false;
    }
    if (!callbacks)
        return true;
    if (!videoCh->prepared) {
        if (callbacks->prepare && callbacks->prepare(session, &videoCh->config,
                                                     session->callbackContexts.video) != 0)
            return false;
        videoCh->prepared = true;
    }
    if (videoCh->tracked &&
        (!session->frameTracker || !videoCh->epoch.video_epoch ||
         !IHS_FrameTrackerOpenEpoch(session->frameTracker, videoCh->epoch.video_epoch))) {
        IHS_SessionDisconnect(session);
        return false;
    }
    if (!videoCh->tracked && !callbacks->start) {
        videoCh->backendReady = true; /* Preserve legacy submit-only callbacks. */
        return true;
    }
    int started =
        videoCh->tracked
            ? callbacks->startTracked(session, &videoCh->epoch, &videoCh->config,
                                      session->callbackContexts.video)
            : callbacks->start(session, &videoCh->config, session->callbackContexts.video);
    bool unavailable = callbacks->prepare && started == 1;
    if (started != 0 && !unavailable) {
        if (videoCh->tracked)
            IHS_FrameTrackerCloseEpoch(session->frameTracker, videoCh->epoch.video_epoch);
        IHS_SessionDisconnect(session); /* Local guard, or legacy combined start failure. */
        return false;
    }
    videoCh->appStarted = true;
    videoCh->backendReady = !unavailable;
    if (unavailable) return true;
    CVideoDecoderInfoMsg message = CVIDEO_DECODER_INFO_MSG__INIT;
    message.info = (char *)session->clientCapabilities.decoderInfo;
    if (session->clientCapabilities.decoderThreads)
        PROTOBUF_C_SET_VALUE(message, threads, session->clientCapabilities.decoderThreads);

    /* Standalone StartDecoding does not stop its actor on Info send failure. */
    IHS_SessionSendControlMessage(session, k_EStreamControlVideoDecoderInfo,
                                  (const ProtobufCMessage *)&message);
    return true;
}

static void DataReceived(IHS_SessionChannel *channel, const IHS_SessionDataFrameHeader *header,
                         IHS_Buffer *body) {
    if (!header)
        return;
    IHS_SessionChannelVideo *videoCh = (IHS_SessionChannelVideo *)channel;
    uint32_t receivedPayloadSize = (uint32_t)body->size;
    int32_t offset = header->timestampsLocal ? 0 : IHS_StreamClockOffset(&channel->session->clock);
    /* Production sampling occurs on RX before the FIFO. Direct decoder probes
     * can supply raw peer-clock headers and retain their own receive boundary. */
    if (!header->receivedRecorded) {
        IHS_SessionRecordNetworkVideo(channel->session, header->sendTimestamp - offset,
            header->receiveBegin, header->receiveTimestamp, header->id, receivedPayloadSize);
    }
    if (channel->session->frameStats != NULL) {
        if (videoCh->tracked)
            IHS_FrameStatsCollectTracked(channel->session->frameStats, channel->session->frameTracker);
        if (!header->receivedRecorded) {
            IHS_FrameStatsRecordReceivedInput(channel->session->frameStats, header->id,
                header->timestamp - offset, header->sendTimestamp - offset,
                header->receiveTimestamp, receivedPayloadSize, header->inputMark,
                header->inputRecvTimestamp ? header->inputRecvTimestamp - offset : 0);
        }
    }

    if (body->size < 7)
        return;
    IHS_VideoFrameHeader vhead;
    IHS_BufferOffsetBy(body, (int)VideoFrameHeaderParse(&vhead, IHS_BufferPointer(body)));

    IHS_MutexLock(videoCh->stateMutex);
    videoCh->contributed = false;
    if (videoCh->tracked) {
        uint64_t now = IHS_TimerNow() * 1000;
        IHS_FrameReceive received = {.firstReceiveUs = now,
                                     .lastReceiveUs = now,
                                     .senderFrameTimestamp = header->timestamp - offset,
                                     .senderSendTimestamp = header->sendTimestamp - offset,
                                     .receiveTimestamp = header->receiveTimestamp,
                                     .inputMark = header->inputMark,
                                     .inputReceived = header->inputRecvTimestamp ? header->inputRecvTimestamp - offset : 0,
                                     .frameSize = receivedPayloadSize};
        IHS_FrameBeginResult result =
            IHS_FrameTrackerBegin(channel->session->frameTracker, videoCh->epoch.video_epoch,
                                  header->id, &received, &videoCh->incoming);
        if (result != IHS_FrameBeginOK) {
            if (result != IHS_FrameBeginStale)
                IHS_SessionDisconnect(channel->session);
            goto unlock;
        }
    }

    if (vhead.flags & VideoFrameFlagKeyFrame) {
        DiscardPending(videoCh);
        const IHS_StreamVideoCallbacks *callbacks = channel->session->callbacks.video;
        if (videoCh->tracked && callbacks->flushTracked) {
            callbacks->flushTracked(channel->session, &videoCh->epoch,
                                     IHS_StreamVideoFlushDecoder,
                                     channel->session->callbackContexts.video);
            videoCh->frame.flags |= IHS_StreamVideoFrameFlushHandled;
        }
        videoCh->states.waitingKeyFrame = false;
        videoCh->states.syncingBacklog = false;
        videoCh->states.overflowFrameSpan = 0;
        videoCh->states.expectedSequence = vhead.sequence;
        IHS_SessionLog(channel->session, IHS_LogLevelDebug, "Video", "Coming keyframe");
    }
    uint32_t now = IHS_SessionPacketTimestamp();
    if ((videoCh->states.waitingKeyFrame || videoCh->states.syncingBacklog) &&
        (int32_t)(now - videoCh->states.lastRecoveryRequest) > 6553) {
        IHS_SessionChannelDataLost(channel);
        videoCh->states.lastRecoveryRequest = now;
    }
    if (!videoCh->states.waitingKeyFrame && vhead.sequence != videoCh->states.expectedSequence) {
        if (!videoCh->states.syncingBacklog) {
            IHS_SessionChannelDataLost(channel);
            videoCh->states.lastRecoveryRequest = now;
        }
        videoCh->states.waitingKeyFrame = true;
    }
    videoCh->states.expectedSequence = vhead.sequence + 1;
    videoCh->states.lastFrameId = header->id;
    if (videoCh->states.waitingKeyFrame) {
        goto unlock;
    }
    if (vhead.flags & VideoFrameFlagEncrypted) {
        const IHS_SessionInfo *config = &channel->session->info;
        IHS_Buffer plain;
        IHS_BufferInit(&plain, 0, 0);
        IHS_BufferEnsureMaxSizeExact(&plain, body->size);
        size_t outLen = body->size;
        int decryptRet = IHS_CryptoSymmetricDecryptWithIV(
            IHS_BufferPointer(body), body->size, EmptyIV, sizeof(EmptyIV), config->sessionKey,
            config->sessionKeyLen, IHS_BufferPointer(&plain), &outLen);
        if (decryptRet != 0) {
            IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Video",
                           "Failed to decrypt video frame: %d, request keyframe", decryptRet);
            IHS_BufferClear(&plain, true);
            IHS_SessionChannelDataLost(channel);
            videoCh->states.waitingKeyFrame = true;
            videoCh->states.lastRecoveryRequest = IHS_SessionPacketTimestamp();
            goto unlock;
        }
        plain.size = outLen;
        AddPartialFrame(videoCh, header->id, header->timestamp, &vhead, &plain);
        IHS_BufferClear(&plain, true);
    } else {
        AddPartialFrame(videoCh, header->id, header->timestamp, &vhead, body);
    }

    while (!videoCh->base.interrupted && AssembleFrame(channel)) {
        /* Official OnThink 7b2648..26f8: only the finishing fragment decides
         * mode. A transition requests an asynchronous reset. Every pending
         * request, including one from a previous backend submit, gates FinalDecode. */
        if (videoCh->states.contentModeSeen &&
            videoCh->states.contentMode != videoCh->frame.contentMode)
            atomic_store(&videoCh->resetRequested, true);
        videoCh->states.contentModeSeen = true;
        videoCh->states.contentMode = videoCh->frame.contentMode;
        if (atomic_load(&videoCh->resetRequested)) {
            if (videoCh->assembly) {
                if (videoCh->frame.deferredCount == IHS_FRAME_TRACKER_CAPACITY) {
                    IHS_SessionDisconnect(channel->session); /* Local ownership bound. */
                    break;
                }
                videoCh->frame.deferred[videoCh->frame.deferredCount++] = videoCh->assembly;
                videoCh->assembly = NULL;
            }
            /* The original keeps encoded buffers/packet references but accepts
             * following frames. Do not submit or clear the accumulated bytes. */
            videoCh->states.frameStarted = videoCh->states.frameFinished = false;
            videoCh->frame.flags = 0;
            continue;
        }
        IHS_StreamVideoSubmitResult result =
            SubmitFrame(channel, videoCh->assemblyId, &videoCh->frame.buffer, videoCh->frame.flags);
        if (videoCh->assembly) {
            IHS_FrameTicketRelease(videoCh->assembly);
            videoCh->assembly = NULL;
        }
        IHS_BufferClear(&videoCh->frame.buffer, false);
        videoCh->frame.flags = 0;
        videoCh->states.frameFinished = false;
        videoCh->states.frameStarted = false;
        if (result == IHS_StreamVideoSubmitError)
            break;
    }
    CheckPartialOverflow(channel);
unlock:
    if (videoCh->incoming) {
        if (!videoCh->contributed) {
            IHS_FrameOutcome outcome = {.result = IHS_VideoFrameResultDroppedNetworkLost,
                                        .completionUs = IHS_TimerNow() * 1000};
            IHS_FrameTicketComplete(videoCh->incoming, &outcome);
        }
        IHS_FrameTicketRelease(videoCh->incoming);
        videoCh->incoming = NULL;
    }
    IHS_MutexUnlock(videoCh->stateMutex);
}

static void DataStop(IHS_SessionChannel *channel) {
    IHS_Session *session = channel->session;
    IHS_SessionChannelVideo *videoCh = (IHS_SessionChannelVideo *)channel;
    const IHS_StreamVideoCallbacks *callbacks = session->callbacks.video;
    if (!videoCh->appStarted)
        return;
    videoCh->appStarted = false;
    if (videoCh->tracked) {
        if (session->frameStats)
            IHS_FrameStatsCollectTracked(session->frameStats, session->frameTracker);
        IHS_FrameTrackerCloseEpoch(session->frameTracker, videoCh->epoch.video_epoch);
        if (callbacks->flushTracked)
            callbacks->flushTracked(session, &videoCh->epoch, IHS_StreamVideoFlushPlayer,
                                     session->callbackContexts.video);
    }
    DiscardPending(videoCh);
    if (videoCh->tracked && callbacks->flushTracked)
        callbacks->flushTracked(session, &videoCh->epoch, IHS_StreamVideoFlushDecoder,
                                 session->callbackContexts.video);
    /* Base Stop 7b1fe8..f0 records recovery after flush, before the standalone
     * Stop retires its backend. Destruction/next initialization count as time
     * already spent waiting for recovery, including uint32 clock wrap. */
    videoCh->states.waitingKeyFrame = true;
    videoCh->states.syncingBacklog = false;
    videoCh->states.lastRecoveryRequest = IHS_SessionPacketTimestamp();
    if (videoCh->tracked) {
        callbacks->stopTracked(session, &videoCh->epoch, session->callbackContexts.video);
    } else if (callbacks && callbacks->stop)
        callbacks->stop(session, session->callbackContexts.video);
    videoCh->backendReady = false;
}

void IHS_SessionVideoHandlePendingReset(IHS_Session *session) {
    if (session->base.interrupted || session->transport.detached)
        return;
    IHS_SessionChannel *ch = IHS_SessionChannelForType(session, IHS_SessionChannelTypeDataVideo);
    /* A generic data-channel fixture/consumer may use the video channel type
     * without owning this private decoder state. Type alone is not its ABI. */
    if (!ch || ch->cls != &ChannelClass.base) return;
    IHS_SessionChannelVideo *video = (void *)ch;
    if (!atomic_load(&video->resetRequested) || !video->appStarted) return;
    /* No Base/state mutex across join or callbacks. RX is this actor, so channel
     * replacement cannot race this section. New complete data is excluded. */
    IHS_SessionChannelDataJoinAndPurge(&video->base);
    DataStop(ch);
    atomic_store(&video->resetRequested, false); /* Coalesce old-worker requests. */
    if (session->base.interrupted || session->transport.detached) return;
    if (video->tracked) {
        if (session->nextVideoEpoch == UINT64_MAX) {
            IHS_SessionDisconnect(session);
            return;
        }
        video->epoch.video_epoch = ++session->nextVideoEpoch;
    }
    if (!DataStart(ch) || session->base.interrupted || session->transport.detached) return;
    IHS_SessionChannelDataRestartWorker(&video->base);
    IHS_SessionChannelDataLost(ch);
}

static size_t VideoFrameHeaderParse(IHS_VideoFrameHeader *header, const uint8_t *data) {
    size_t offset = 0;
    offset += IHS_ReadUInt16LE(&data[offset], &header->sequence);
    header->flags = data[offset++];
    offset += IHS_ReadUInt16LE(&data[offset], &header->subFrameStart);
    offset += IHS_ReadUInt16LE(&data[offset], &header->subFrameEnd);
    return offset;
}

static bool AssembleFrame(IHS_SessionChannel *channel) {
    IHS_SessionChannelVideo *videoCh = (IHS_SessionChannelVideo *)channel;

    IHS_VideoPartialFrame *partial = videoCh->frame.partial.head;
    while (partial != NULL && !videoCh->states.frameFinished) {
        IHS_VideoPartialFrame *next = partial->next;
        if (partial->header.subFrameEnd != 0) {
            if (partial->header.subFrameStart != videoCh->frame.expectedSubFrameStart) {
                break;
            }
            if (partial->header.flags & VideoFrameFlagSubFrameAdvance) {
                if (partial->header.flags & VideoFrameFlagFrameFinish) {
                    videoCh->frame.expectedSubFrameStart = 0;
                } else {
                    videoCh->frame.expectedSubFrameStart = partial->header.subFrameEnd + 1;
                }
            }
        }
        if (!videoCh->states.frameStarted) {
            videoCh->states.frameStarted = true;
            videoCh->assemblyId = partial->frameId;
            if (partial->ticket) {
                if (!IHS_FrameTicketRetain(partial->ticket))
                    return false;
                videoCh->assembly = partial->ticket;
            }
        } else if (partial->frameId != videoCh->assemblyId ||
                   (videoCh->tracked && partial->ticket != videoCh->assembly)) {
            DiscardPending(videoCh);
            videoCh->states.waitingKeyFrame = true;
            videoCh->states.lastRecoveryRequest = IHS_SessionPacketTimestamp();
            IHS_SessionChannelDataLost(channel);
            return false;
        }
        // append buffer
        if (!AppendToFrameBuffer(videoCh, &partial->data, &partial->header)) {
            DiscardPending(videoCh);
            IHS_SessionDisconnect(channel->session);
            return false;
        }
        if (partial->header.flags & VideoFrameFlagFrameFinish) {
            videoCh->frame.contentMode = (partial->header.flags & VideoFrameFlagContentMode) != 0;
            videoCh->states.frameFinished = true;
        }
        IHS_BufferClear(&partial->data, true);
        IHS_VideoPartialFramesRemove(&videoCh->frame.partial, partial);
        partial = next;
    }
    return videoCh->states.frameFinished;
}

static void AddPartialFrame(IHS_SessionChannelVideo *channel, uint16_t frameId, uint32_t timestamp,
                            const IHS_VideoFrameHeader *header, IHS_Buffer *data) {
    /* Soft synchronization may last arbitrarily long if the host never sends
     * a keyframe. Bound pending storage independently of host cooperation. */
    size_t bytes = channel->frame.buffer.size;
    unsigned count = 0;
    for (IHS_VideoPartialFrame *part = channel->frame.partial.head; part; part = part->next) {
        bytes += part->data.size;
        count++;
    }
    if (count >= 256 || data->size > channel->frame.buffer.maxCapacity ||
        bytes > channel->frame.buffer.maxCapacity - data->size) {
        DiscardPending(channel);
        channel->states.waitingKeyFrame = true;
        channel->states.lastRecoveryRequest = IHS_SessionPacketTimestamp();
        IHS_SessionChannelDataLost(&channel->base.base);
        return;
    }
    IHS_VideoPartialFrame *cur = NULL;
    IHS_VideoPartialFramesForEach(cur, &channel->frame.partial) {
        if (frameId == cur->frameId && header->subFrameEnd < cur->header.subFrameStart) {
            break;
        }
    }
    IHS_VideoPartialFrame *inserted;
    if (cur != NULL) {
        inserted =
            IHS_VideoPartialFramesInsertBefore(&channel->frame.partial, cur, frameId, header, data);
    } else {
        inserted = IHS_VideoPartialFramesAppend(&channel->frame.partial, frameId, header, data);
    }
    if (!inserted) {
        IHS_SessionDisconnect(channel->base.base.session);
        return;
    }
    inserted->timestamp = timestamp;
    if (channel->incoming) {
        if (!IHS_FrameTicketRetain(channel->incoming)) {
            IHS_SessionDisconnect(channel->base.base.session);
            return;
        }
        inserted->ticket = channel->incoming;
        channel->contributed = true;
    }
}

static void CheckPartialOverflow(IHS_SessionChannel *channel) {
    IHS_SessionChannelVideo *videoCh = (IHS_SessionChannelVideo *)channel;
    if (videoCh->states.waitingKeyFrame || videoCh->states.syncingBacklog)
        return;
    IHS_VideoPartialFrame *head = videoCh->frame.partial.head;
    IHS_VideoPartialFrame *tail = videoCh->frame.partial.tail;
    if (!head || head == tail)
        return;
    int32_t span = (int32_t)(tail->timestamp - head->timestamp);
    if (span > 9830) {
        IHS_SessionChannelDataLost(channel);
        videoCh->states.syncingBacklog = true;
        videoCh->states.lastRecoveryRequest = IHS_SessionPacketTimestamp();
    }
    /* CheckOverflow 0x7b2988..b8 compares an inclusive frame-ID range.
     * +412 is the member offset, not a timestamp increment. Keep the signed
     * 32-bit subtraction used by the reference, including at ID wrap. */
    int32_t frameSpan = (int32_t)tail->frameId - (int32_t)head->frameId + 1;
    if (frameSpan >= videoCh->states.overflowFrameSpan) {
        CStreamVideoOverflowMsg message = CSTREAM_VIDEO_OVERFLOW_MSG__INIT;
        PROTOBUF_C_SET_VALUE(message, first_frame_id, head->frameId);
        PROTOBUF_C_SET_VALUE(message, last_frame_id, tail->frameId);
        IHS_SessionSendControlMessage(channel->session, k_EStreamControlVideoOverflow,
                                      (const ProtobufCMessage *)&message);
    }
    /* Even a suppressed, smaller range becomes the next comparison point. */
    videoCh->states.overflowFrameSpan = frameSpan;
}

static void DiscardPending(IHS_SessionChannelVideo *channel) {
    IHS_FrameOutcome outcome = {.result = IHS_VideoFrameResultDroppedReset,
                                .completionUs = IHS_TimerNow() * 1000};
    if (channel->assembly) {
        if (channel->assembly != channel->incoming)
            IHS_FrameTicketComplete(channel->assembly, &outcome);
        IHS_FrameTicketRelease(channel->assembly);
        channel->assembly = NULL;
    }
    for (size_t n = 0; n < channel->frame.deferredCount; ++n) {
        IHS_FrameTicket *ticket = channel->frame.deferred[n];
        if (ticket != channel->incoming)
            IHS_FrameTicketComplete(ticket, &outcome);
        IHS_FrameTicketRelease(ticket);
    }
    channel->frame.deferredCount = 0;
    for (IHS_VideoPartialFrame *part = channel->frame.partial.head; part; part = part->next)
        if (part->ticket && part->ticket != channel->incoming)
            IHS_FrameTicketComplete(part->ticket, &outcome);
    IHS_BufferClear(&channel->frame.buffer, 0);
    channel->states.frameStarted = channel->states.frameFinished = false;
    size_t clearedCount = IHS_VideoPartialFramesClear(&channel->frame.partial);
    if (clearedCount > 0) {
        IHS_SessionLog(((IHS_SessionChannel *)channel)->session, IHS_LogLevelWarn, "Video",
                       "%zu partial frames were cleared", clearedCount);
    }
    channel->frame.flags = 0;
    channel->frame.expectedSubFrameStart = 0;
    channel->states.overflowFrameSpan = 0;
}

static bool AppendToFrameBuffer(IHS_SessionChannelVideo *channel, const IHS_Buffer *data,
                                const IHS_VideoFrameHeader *header) {
    if (!data->size || data->size > channel->frame.buffer.maxCapacity)
        return false;
    size_t required =
        (header->flags & VideoFrameFlagNeedEscape) ? data->size * 3 / 2 + 5 : data->size;
    if (required > channel->frame.buffer.maxCapacity - channel->frame.buffer.size)
        return false;
    switch (channel->config.codec) {
    case IHS_StreamVideoCodecH264:
        IHS_SessionVideoFrameAppendH264(&channel->frame.buffer, IHS_BufferPointer(data), data->size,
                                        header);
        break;
    case IHS_StreamVideoCodecHEVC:
        IHS_SessionVideoFrameAppendHEVC(&channel->frame.buffer, IHS_BufferPointer(data), data->size,
                                        header);
        break;
    default: {
        IHS_SessionLog(((IHS_SessionChannel *)channel)->session, IHS_LogLevelFatal, "Video",
                       "Unsupported codec %u", channel->config.codec);
        abort();
    }
    }
    if (header->flags & VideoFrameFlagKeyFrame) {
        channel->frame.flags |= IHS_StreamVideoFrameKeyFrame;
    }
    return true;
}

static IHS_StreamVideoSubmitResult SubmitFrame(IHS_SessionChannel *channel, uint16_t frameId,
                                               IHS_Buffer *data, IHS_StreamVideoFrameFlag flags) {
    IHS_Session *session = channel->session;
    const IHS_StreamVideoCallbacks *callbacks = session->callbacks.video;
    IHS_SessionChannelVideo *videoCh = (IHS_SessionChannelVideo *)channel;
    if (callbacks == NULL || (!videoCh->tracked && callbacks->submit == NULL))
        return IHS_StreamVideoSubmitOK;
    void *context = session->callbackContexts.video;
    IHS_StreamVideoSubmitResult result;
    if (videoCh->tracked) {
        bool taken = false;
        if (videoCh->assembly)
            IHS_FrameTicketSetSize(videoCh->assembly, data->size);
        result = videoCh->backendReady
            ? callbacks->submitTracked(session, &videoCh->epoch, frameId, videoCh->assembly,
                                        data, flags, &taken, context)
            : IHS_StreamVideoSubmitReset;
        if (!taken && videoCh->assembly) {
            IHS_FrameOutcome outcome = {.result = IHS_VideoFrameResultDroppedDecodeCorrupt,
                                        .completionUs = IHS_TimerNow() * 1000};
            IHS_FrameTicketComplete(videoCh->assembly, &outcome);
        }
    } else
        result = videoCh->backendReady ? callbacks->submit(session, frameId, data, flags, context)
                                      : IHS_StreamVideoSubmitReset;
    if (result == IHS_StreamVideoSubmitReset) {
        atomic_store(&videoCh->resetRequested, true);
    } else if (result == IHS_StreamVideoSubmitReportLost) {
        /* FinalDecode 0x7b1158..68 requests recovery without an unconditional
         * backend reset or strict keyframe wait. The backend may still decode
         * subsequent frames; damaged output is rejected by the application. */
        IHS_SessionChannelDataLost(channel);
    } else if (result == IHS_StreamVideoSubmitError) {
        IHS_SessionLog(session, IHS_LogLevelError, "Video",
                       "Decoder reported unrecoverable error.");
        IHS_SessionDisconnect(session);
    }
    return result;
}

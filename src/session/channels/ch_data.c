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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch_data.h"
#include "endianness.h"
#include "session/session_pri.h"

#include "ihs_buffer_ext.h"
#include "ihs_timer.h"

static void DataThreadWorker(IHS_SessionChannelData *channel);

static void DataThreadInterrupt(IHS_SessionChannelData *channel, bool restarting);

static const char *DataChannelName(IHS_SessionChannelType type);

typedef struct IHS_FECFrameNode {
    IHS_SessionFrame frame;
    struct IHS_FECFrameNode *next;
} IHS_FECFrameNode;

static bool PollFEC(IHS_SessionChannelData *channel, IHS_SessionFrame *frame) {
    IHS_FECFrameNode *node = channel->fecHead;
    if (!node)
        return false;
    channel->fecHead = node->next;
    if (!channel->fecHead)
        channel->fecTail = NULL;
    channel->fecQueuedBytes -= node->frame.body.capacity + sizeof(*node);
    channel->fecQueuedCount--;
    IHS_BufferClear(&frame->body, true);
    *frame = node->frame;
    free(node);
    return true;
}

static bool QueueFrame(IHS_SessionChannelData *channel, IHS_SessionFrame *frame) {
    IHS_Buffer *body = &frame->body;
    /* Admission is about owned bytes, not a protocol/HW maximum. Preserve
     * offset and suffix when shrinking geometric or padded FEC storage. */
    if (body->offset > body->capacity || body->suffix > body->capacity - body->offset ||
        body->size > body->capacity - body->offset - body->suffix ||
        (body->capacity && !body->data)) {
        IHS_BufferClear(body, true);
        return false;
    }
    /* Official client OnStreamPacket 7ad00c..7ad01c: transport windows,
     * reconstruction and feedback still run while paused; gate complete data
     * frames before handing them to the media consumer. Frames already owned
     * by that consumer are not retrospectively discarded on Pause. */
    IHS_Session *session = channel->base.session;
    if (frame->body.size && *IHS_BufferPointer(&frame->body) == k_EStreamDataPacket &&
        (session->state.connectionState != IHS_SessionConnectionStateConnected ||
         session->transport.clientPaused)) {
        IHS_BufferClear(&frame->body, true);
        return false;
    }
    size_t nodeSize = sizeof(IHS_FECFrameNode);
    size_t used = body->offset + body->size + body->suffix;
    if (channel->fecQueueLimit < nodeSize || used > channel->fecQueueLimit - nodeSize) {
        IHS_BufferClear(&frame->body, true);
        return false;
    }
    if (body->capacity > channel->fecQueueLimit - nodeSize) {
        if (!used) {
            IHS_BufferClear(body, true);
        } else {
            void *compact = realloc(body->data, used);
            if (!compact) {
                IHS_BufferClear(body, true);
                return false;
            }
            body->data = compact;
            body->capacity = used;
        }
    }
    size_t size = body->capacity + nodeSize;
    while (channel->fecHead && (channel->fecQueuedBytes > channel->fecQueueLimit - size ||
                                channel->fecQueuedCount == channel->fecQueueCapacity)) {
        IHS_SessionFrame old = {0};
        PollFEC(channel, &old);
        IHS_BufferClear(&old.body, true);
    }
    IHS_FECFrameNode *node = malloc(sizeof(*node));
    if (!node) {
        IHS_BufferClear(&frame->body, true);
        return false;
    }
    *node = (IHS_FECFrameNode){.frame = *frame};
    if (channel->fecTail)
        channel->fecTail->next = node;
    else
        channel->fecHead = node;
    channel->fecTail = node;
    channel->fecQueuedBytes += size;
    channel->fecQueuedCount++;
    memset(frame, 0, sizeof(*frame));
    return true;
}

IHS_SessionChannel *IHS_SessionChannelDataCreate(const IHS_SessionChannelDataClass *cls,
                                                 IHS_Session *session, IHS_SessionChannelType type,
                                                 IHS_SessionChannelId id, const void *config) {
    assert(cls->base.instanceSize >= sizeof(IHS_SessionChannelData));
    return IHS_SessionChannelCreate(&cls->base, session, type, id, config);
}

bool IHS_SessionChannelDataInit(IHS_SessionChannel *channel, uint16_t windowCapacity) {
    IHS_SessionChannelData *dataCh = (IHS_SessionChannelData *)channel;
    dataCh->lock = IHS_MutexCreate();
    dataCh->windowLock = IHS_MutexCreate();
    dataCh->windowCond = IHS_CondCreate();
    dataCh->decoderWakePending = false;
    dataCh->receive = IHS_SessionUnreliableGet(channel->session, channel->id, windowCapacity);
    if (dataCh->receive)
        dataCh->receive->maxReceiveDelay = 6553; /* StartAudio/VideoData setter */
    dataCh->fecQueueCapacity = windowCapacity;
    dataCh->fecQueueLimit = (size_t)windowCapacity * 2048;
    /* Retain the existing 2MiB floor for small audio/test windows. This FIFO
     * budget is independent of FEC data storage, not a wire/HW limit. */
    size_t minimumQueueBytes = 2u * 1024u * 1024u + sizeof(IHS_FECFrameNode);
    if (dataCh->fecQueueLimit < minimumQueueBytes)
        dataCh->fecQueueLimit = minimumQueueBytes;
    atomic_init(&dataCh->interrupted, false);
    if (!dataCh->lock || !dataCh->windowLock || !dataCh->windowCond || !dataCh->receive) {
        channel->initFailed = true;
        return false;
    }
    const IHS_SessionChannelDataClass *cls = (const void *)channel->cls;
    if (cls->startOnCaller) {
        dataCh->callerStarted = cls->start(channel);
        if (!dataCh->callerStarted) {
            /* Official player-start failure keeps the media channel mapping,
             * replays pending packets, and drops them without a decoder. */
            dataCh->discardFrames = true;
            return true;
        }
    }
    char threadName[16];
    snprintf(threadName, 16, "IHS%s", DataChannelName(channel->type));
    dataCh->worker = IHS_ThreadCreate((IHS_ThreadFunction *)DataThreadWorker, threadName, dataCh);
    if (!dataCh->worker && cls->startOnCaller) {
        /* Original OnStreamStarted ignores OS Thread::Start failure. Retain
         * the initialized backend and queue until Stop; the existing FIFO
         * memory bound still applies. Teardown purges before backend stop. */
        return true;
    }
    channel->initFailed = !dataCh->worker;
    if (channel->initFailed)
        DataThreadInterrupt(dataCh, false);
    return !channel->initFailed;
}

void IHS_SessionChannelDataDeinit(IHS_SessionChannel *channel) {
    IHS_SessionChannelData *dataCh = (IHS_SessionChannelData *)channel;
    if (channel->cls && channel->cls->dataInterrupt)
        channel->cls->dataInterrupt(channel, false);

    assert(!dataCh->worker || dataCh->interrupted);

    if (dataCh->worker)
        IHS_ThreadJoin(dataCh->worker);
    IHS_SessionFrame remaining = {0};
    while (PollFEC(dataCh, &remaining))
        IHS_BufferClear(&remaining.body, true);
    const IHS_SessionChannelDataClass *cls = (const void *)channel->cls;
    if (dataCh->callerStarted) {
        assert(cls && cls->startOnCaller);
        /* Original OnStreamStopped joins/purges before virtual StopDecoding. */
        cls->stop(channel);
        dataCh->callerStarted = false;
    }
    if (dataCh->windowLock)
        IHS_MutexDestroy(dataCh->windowLock);
    if (dataCh->windowCond)
        IHS_CondDestroy(dataCh->windowCond);
    if (dataCh->lock)
        IHS_MutexDestroy(dataCh->lock);
}

void IHS_SessionChannelDataReceived(IHS_SessionChannel *channel, IHS_SessionPacket *packet) {
    IHS_SessionUnreliableReceived(channel->session, packet);
}

void IHS_SessionChannelDataLost(IHS_SessionChannel *channel) {
    CStreamDataLostMsg message = CSTREAM_DATA_LOST_MSG__INIT;
    IHS_SessionPacket packet;
    /* Final Socket emission adds CRC only when negotiated (protocol RE §113). */
    IHS_SessionChannelInitializePacket(channel, &packet, IHS_SessionPacketTypeUnreliable, false,
                                       IHS_PACKET_ID_NEXT);
    IHS_BufferAppendUInt8(&packet.body, k_EStreamDataLost);
    IHS_BufferAppendMessage(&packet.body, (const ProtobufCMessage *)&message);
    IHS_SessionChannelQueuePacket(channel, &packet, false);
    IHS_SessionPacketClear(&packet, true);
}

void IHS_SessionChannelDataStopped(IHS_SessionChannel *channel) {
    DataThreadInterrupt((IHS_SessionChannelData *)channel, false);
}

void IHS_SessionChannelDataJoinAndPurge(IHS_SessionChannelData *data, bool restarting) {
    DataThreadInterrupt(data, restarting);
    atomic_store(&data->interrupted, true);
    if (data->worker)
        IHS_ThreadJoin(data->worker);
    data->worker = NULL;
    IHS_MutexLock(data->windowLock);
    IHS_SessionFrame frame = {0};
    while (PollFEC(data, &frame))
        IHS_BufferClear(&frame.body, true);
    IHS_MutexUnlock(data->windowLock);
}

void IHS_SessionChannelDataRestartWorker(IHS_SessionChannelData *data) {
    assert(!data->worker);
    IHS_MutexLock(data->windowLock);
    atomic_store(&data->interrupted, false);
    IHS_MutexUnlock(data->windowLock);
    char name[16];
    snprintf(name, sizeof(name), "IHS%s", DataChannelName(data->base.type));
    data->worker = IHS_ThreadCreate((IHS_ThreadFunction *)DataThreadWorker, name, data);
    /* Original Reset ignores the OS Start result and retains the actor. */
}

size_t IHS_SessionChannelDataFrameHeaderParse(IHS_SessionDataFrameHeader *header,
                                              const IHS_Buffer *data) {
    if (data->size < IHS_SESSION_DATA_FRAME_HEADER_SIZE)
        return 0;
    header->timestampsLocal = false;
    header->receivedRecorded = false;
    size_t offset = 0;
    offset += IHS_ReadUInt16LE(IHS_BufferPointerAt(data, offset), &header->id);
    offset += IHS_ReadUInt32LE(IHS_BufferPointerAt(data, offset), &header->timestamp);
    offset += IHS_ReadUInt16LE(IHS_BufferPointerAt(data, offset), &header->inputMark);
    offset += IHS_ReadUInt32LE(IHS_BufferPointerAt(data, offset), &header->inputRecvTimestamp);
    return offset;
}

void IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *channel,
                                                IHS_SessionFrame *frame) {
    IHS_SessionChannelData *data = (void *)channel;
    IHS_MutexLock(data->windowLock);
    if (!data->interrupted && !data->discardFrames) {
        IHS_SessionFrame owned = {.header = frame->header,
                                  .mediaOffset = frame->mediaOffset,
                                  .mediaPrepared = frame->mediaPrepared};
        IHS_BufferTransferOwnership(&frame->body, &owned.body);
        if (QueueFrame(data, &owned)) {
            data->decoderWakePending = true;
            IHS_CondSignal(data->windowCond);
        }
    }
    IHS_MutexUnlock(data->windowLock);
}

bool IHS_SessionChannelDataPollLocked(IHS_SessionChannelData *channel, IHS_SessionFrame *frame,
                                      uint16_t *missing) {
    *missing = 0;
    return PollFEC(channel, frame);
}

/* Caller holds windowLock. Original auto-reset event retains Set before Wait
 * and consumes it on return; spurious cond wakes do not authorize OnThink. */
static void WaitForDecoderEvent(IHS_SessionChannelData *channel, uint32_t timeout) {
    uint64_t start = timeout != UINT32_MAX ? IHS_TimerNow() : 0;
    while (!channel->interrupted && !channel->fecHead && !channel->decoderWakePending && timeout) {
        bool woke;
        if (timeout == UINT32_MAX) {
            woke = IHS_CondWait(channel->windowCond, channel->windowLock);
        } else {
            uint64_t elapsed = IHS_TimerNow() - start;
            if (elapsed >= timeout)
                break;
            woke = IHS_CondWaitTimeout(channel->windowCond, channel->windowLock,
                                       timeout - (uint32_t)elapsed);
        }
        if (!woke)
            break; /* Timeout/error, like the original platform boundary. */
    }
    channel->decoderWakePending = false;
}

static void DataThreadWorker(IHS_SessionChannelData *channel) {
    const IHS_SessionChannelDataClass *cls = (const IHS_SessionChannelDataClass *)channel->base.cls;
    IHS_SessionFrame frame;
    IHS_BufferInit(&frame.body, 1024, 1024 * 1024);
    const char *channelName = DataChannelName(channel->base.type);
    IHS_SessionLog(channel->base.session, IHS_LogLevelInfo, "Data", "Starting %s channel",
                   channelName);
    if (!cls->startOnCaller && !cls->start((IHS_SessionChannel *)channel)) {
        IHS_SessionLog(channel->base.session, IHS_LogLevelError, "Data",
                       "Failed to start %s channel", channelName);
        IHS_SessionDisconnect(channel->base.session);
        IHS_BufferClear(&frame.body, true);
        return;
    }
    IHS_SessionLog(channel->base.session, IHS_LogLevelInfo, "Data", "%s channel started",
                   channelName);
    while (!channel->interrupted) {
        IHS_MutexLock(channel->windowLock);
        uint16_t missing;
        bool hasFrame = IHS_SessionChannelDataPollLocked(channel, &frame, &missing);
        IHS_MutexUnlock(channel->windowLock);
        if (hasFrame) {
            IHS_SessionChannelDataReceiveFrame(channel, &frame);
            IHS_SessionPacketsWindowReleaseFrame(&frame);
            continue;
        }
        if (channel->interrupted)
            break;
        /* Official RunDecodeLoop 766dd8..766e48 drains queued DecodeFrame
         * inputs before OnThink. A newer key fragment can flush an older
         * complete image before it reaches the SDK. Never hold FIFO lock over
         * decoder/platform callbacks. */
        uint32_t timeout = IHS_SessionChannelDataThink(&channel->base);
        IHS_MutexLock(channel->windowLock);
        /* Cond signals are not persistent events. Recheck under the producer's
         * mutex so input/stop during OnThink cannot be followed by a lost wait. */
        if (cls->think) {
            WaitForDecoderEvent(channel, timeout);
        } else if (!channel->interrupted && !channel->fecHead) {
            if (timeout == UINT32_MAX)
                IHS_CondWait(channel->windowCond, channel->windowLock);
            else if (timeout)
                IHS_CondWaitTimeout(channel->windowCond, channel->windowLock, timeout);
        }
        IHS_MutexUnlock(channel->windowLock);
    }
    IHS_SessionLog(channel->base.session, IHS_LogLevelInfo, "Data", "Stopping %s channel",
                   channelName);
    IHS_BufferClear(&frame.body, true);
    if (!cls->startOnCaller)
        cls->stop((IHS_SessionChannel *)channel);
}

static void DataThreadInterrupt(IHS_SessionChannelData *channel, bool restarting) {
    if (channel->base.cls && channel->base.cls->dataInterrupt)
        channel->base.cls->dataInterrupt(&channel->base, restarting);
    if (!channel->worker || channel->interrupted) {
        /* OnStreamStopped 76716c clears active even when OS Start failed.
         * Partial initialization may not have created/initialized this FIFO. */
        if (channel->windowLock) {
            IHS_MutexLock(channel->windowLock);
            atomic_store(&channel->interrupted, true);
            channel->decoderWakePending = true;
            IHS_MutexUnlock(channel->windowLock);
        }
        return;
    }
    IHS_MutexLock(channel->lock);
    channel->interrupted = true;
    IHS_MutexUnlock(channel->lock);

    IHS_MutexLock(channel->windowLock);
    channel->decoderWakePending = true;
    IHS_CondSignal(channel->windowCond);
    IHS_MutexUnlock(channel->windowLock);
}

static const char *DataChannelName(IHS_SessionChannelType type) {
    switch (type) {
    case IHS_SessionChannelTypeDataAudio:
        return "Audio";
    case IHS_SessionChannelTypeDataVideo:
        return "Video";
    default:
        return "Data";
    }
}

bool IHS_SessionChannelDataBuildFeedback(IHS_SessionChannelData *channel, uint32_t now,
                                         IHS_SessionPacket *packet, uint32_t *waitTicks) {
    return IHS_SessionUnreliableBuildFeedback(channel->receive, now, packet, waitTicks);
}

void IHS_SessionChannelDataUpdateAt(IHS_SessionChannelData *channel, uint32_t tick) {
    IHS_SessionUnreliableUpdateAt(channel->base.session, channel->base.id, tick);
}

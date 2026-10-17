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

#include "ch_data.h"
#include "endianness.h"
#include "session/session_pri.h"

#include "ihs_buffer_ext.h"

static void FeedbackThreadWorker(IHS_SessionChannelData *channel);

static void DataThreadWorker(IHS_SessionChannelData *channel);

static void DataThreadInterrupt(IHS_SessionChannelData *channel);

void IHS_SessionChannelDataReceiveFrame(IHS_SessionChannelData *channel, IHS_SessionFrame *frame);

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
    /* The worker also reuses this buffer for ordinary assembled messages. */
    if (frame->body.maxCapacity < IHS_FEC_MAX_PAYLOAD)
        frame->body.maxCapacity = IHS_FEC_MAX_PAYLOAD;
    free(node);
    return true;
}

static void QueueFEC(IHS_SessionChannelData *channel, IHS_SessionFrame *frame) {
    size_t size = frame->body.capacity + sizeof(IHS_FECFrameNode);
    if (size > channel->fecQueueLimit) {
        IHS_BufferClear(&frame->body, true);
        return;
    }
    while (channel->fecHead &&
           (channel->fecQueuedBytes + size > channel->fecQueueLimit ||
            channel->fecQueuedCount == channel->fecQueueCapacity)) {
        IHS_SessionFrame old = {0};
        PollFEC(channel, &old);
        IHS_BufferClear(&old.body, true);
    }
    IHS_FECFrameNode *node = malloc(sizeof(*node));
    if (!node) {
        IHS_BufferClear(&frame->body, true);
        return;
    }
    *node = (IHS_FECFrameNode){.frame = *frame};
    if (channel->fecTail)
        channel->fecTail->next = node;
    else
        channel->fecHead = node;
    channel->fecTail = node;
    channel->fecQueuedBytes += size;
    channel->fecQueuedCount++;
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
    dataCh->feedbackCond = IHS_CondCreate();
    dataCh->window = IHS_SessionPacketsWindowCreateMedia(windowCapacity);
    dataCh->fec = IHS_FECReceiverCreate();
    dataCh->fecQueueCapacity = windowCapacity;
    dataCh->fecQueueLimit = (size_t)windowCapacity * 2048;
    atomic_init(&dataCh->interrupted, false);
    if (!dataCh->lock || !dataCh->windowLock || !dataCh->windowCond || !dataCh->feedbackCond ||
        !dataCh->window || !dataCh->fec) {
        channel->initFailed = true;
        return false;
    }
    char threadName[16];
    snprintf(threadName, 16, "IHS%s", DataChannelName(channel->type));
    dataCh->worker = IHS_ThreadCreate((IHS_ThreadFunction *)DataThreadWorker, threadName, dataCh);
    if (dataCh->worker)
        dataCh->feedbackWorker =
            IHS_ThreadCreate((IHS_ThreadFunction *)FeedbackThreadWorker, "IHSFeedback", dataCh);
    channel->initFailed = !dataCh->worker || !dataCh->feedbackWorker;
    if (channel->initFailed)
        DataThreadInterrupt(dataCh);
    return !channel->initFailed;
}

void IHS_SessionChannelDataDeinit(IHS_SessionChannel *channel) {
    IHS_SessionChannelData *dataCh = (IHS_SessionChannelData *)channel;

    assert(!dataCh->worker || dataCh->interrupted);

    if (dataCh->feedbackWorker)
        IHS_ThreadJoin(dataCh->feedbackWorker);
    if (dataCh->worker)
        IHS_ThreadJoin(dataCh->worker);
    IHS_SessionFrame remaining = {0};
    while (PollFEC(dataCh, &remaining))
        IHS_BufferClear(&remaining.body, true);
    IHS_FECReceiverDestroy(dataCh->fec);
    if (dataCh->window)
        IHS_SessionPacketsWindowDestroy(dataCh->window);
    if (dataCh->windowLock)
        IHS_MutexDestroy(dataCh->windowLock);
    if (dataCh->windowCond)
        IHS_CondDestroy(dataCh->windowCond);
    if (dataCh->feedbackCond)
        IHS_CondDestroy(dataCh->feedbackCond);
    dataCh->window = NULL;
    if (dataCh->lock)
        IHS_MutexDestroy(dataCh->lock);
}

void IHS_SessionChannelDataReceived(IHS_SessionChannel *channel, IHS_SessionPacket *packet) {
    IHS_SessionChannelData *dataCh = (IHS_SessionChannelData *)channel;
    assert(dataCh->window != NULL);
    IHS_SessionPacketType type = packet->header.type;
    if (type != IHS_SessionPacketTypeUnreliable && type != IHS_SessionPacketTypeUnreliableFrag &&
        type != IHS_SessionPacketTypeUnreliableFEC)
        return;
    IHS_MutexLock(dataCh->windowLock);
    if (dataCh->interrupted) {
        IHS_MutexUnlock(dataCh->windowLock);
        return;
    }
    if (type == IHS_SessionPacketTypeUnreliableFEC) {
        IHS_SessionFrame frame;
        if (IHS_FECReceiverAdd(dataCh->fec, packet, &frame)) {
            QueueFEC(dataCh, &frame);
            IHS_CondSignal(dataCh->windowCond);
        }
        IHS_MutexUnlock(dataCh->windowLock);
        return; /* no ordinary window, sender timestamp propagation or NACK */
    }
    bool inserted = false;
    if (!IHS_SessionPacketsWindowAddEx(dataCh->window, packet, &inserted)) {
        IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Data",
                       "%s channel items overflow! Available: %u", DataChannelName(channel->type),
                       IHS_SessionPacketsWindowAvailable(dataCh->window));
        /* Capacity recovery is independent of packet age: release incomplete
         * frames and holes too, then start a fresh receive window. */
        IHS_SessionPacketsWindowReleaseAll(dataCh->window);
        IHS_SessionChannelDataLost(channel);
        IHS_SessionPacketsWindowAddEx(dataCh->window, packet, &inserted);
    }
    if (!dataCh->lastPacketTimestamp ||
        (int32_t)(packet->header.sendTimestamp - dataCh->lastPacketTimestamp) > 0)
        dataCh->lastPacketTimestamp = packet->header.sendTimestamp;
    dataCh->feedbackDirty = true;
    IHS_CondSignal(dataCh->windowCond);
    IHS_CondSignal(dataCh->feedbackCond);
    IHS_MutexUnlock(dataCh->windowLock);
    if (inserted && packet->header.retransmitCount > 1)
        IHS_SessionRecordNetworkLost(channel->session, packet->header.receiveTimestamp,
                                      packet->header.retransmitCount - 1u);
}

void IHS_SessionChannelDataLost(IHS_SessionChannel *channel) {
    CStreamDataLostMsg message = CSTREAM_DATA_LOST_MSG__INIT;
    IHS_SessionPacket packet;
    IHS_SessionChannelInitializePacket(channel, &packet, IHS_SessionPacketTypeUnreliable, true,
                                       IHS_PACKET_ID_NEXT);
    IHS_BufferAppendUInt8(&packet.body, k_EStreamDataLost);
    IHS_BufferAppendMessage(&packet.body, (const ProtobufCMessage *)&message);
    IHS_SessionChannelQueuePacket(channel, &packet, false);
    IHS_SessionPacketClear(&packet, true);
}

void IHS_SessionChannelDataStopped(IHS_SessionChannel *channel) {
    DataThreadInterrupt((IHS_SessionChannelData *)channel);
}

size_t IHS_SessionChannelDataFrameHeaderParse(IHS_SessionDataFrameHeader *header,
                                              const IHS_Buffer *data) {
    if (data->size < IHS_SESSION_DATA_FRAME_HEADER_SIZE)
        return 0;
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
    if (!data->interrupted) {
        IHS_SessionFrame owned = {.header = frame->header};
        IHS_BufferTransferOwnership(&frame->body, &owned.body);
        QueueFEC(data, &owned); /* Shared FIFO for already-complete frames. */
        IHS_CondSignal(data->windowCond);
    }
    IHS_MutexUnlock(data->windowLock);
}

bool IHS_SessionChannelDataPollLocked(IHS_SessionChannelData *channel, IHS_SessionFrame *frame,
                                       uint16_t *missing) {
    *missing = 0;
    if (PollFEC(channel, frame) || IHS_SessionPacketsWindowPoll(channel->window, frame))
        return true;
    IHS_SessionPacketsWindowDiscardEx(channel->window,
        IHS_StreamClockMediaDelayTicks(&channel->base.session->clock), missing);
    return IHS_SessionPacketsWindowPoll(channel->window, frame);
}

static void DataThreadWorker(IHS_SessionChannelData *channel) {
    const IHS_SessionChannelDataClass *cls = (const IHS_SessionChannelDataClass *)channel->base.cls;
    IHS_SessionFrame frame;
    IHS_BufferInit(&frame.body, 1024, 1024 * 1024);
    const char *channelName = DataChannelName(channel->base.type);
    IHS_SessionLog(channel->base.session, IHS_LogLevelInfo, "Data", "Starting %s channel",
                   channelName);
    if (!cls->start((IHS_SessionChannel *)channel)) {
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
        bool hasFrame;
        uint16_t missing;
        uint32_t sampleTime;
        for (;;) {
            sampleTime = IHS_SessionPacketTimestamp();
            hasFrame = IHS_SessionChannelDataPollLocked(channel, &frame, &missing);
            /* A loss-only step must publish outside windowLock, even when no
             * complete frame can be decoded. Then recheck before waiting. */
            if (hasFrame || missing)
                break;
            /* Stop may have signalled before this worker acquired windowLock. */
            if (channel->interrupted)
                break;
            IHS_CondWait(channel->windowCond, channel->windowLock);
            if (!channel->worker || channel->interrupted) {
                break;
            }
        }
        IHS_MutexUnlock(channel->windowLock);
        IHS_SessionRecordNetworkLost(channel->base.session, sampleTime, missing);
        if (hasFrame) {
            IHS_SessionChannelDataReceiveFrame(channel, &frame);
            IHS_SessionPacketsWindowReleaseFrame(&frame);
        }
    }
    IHS_SessionLog(channel->base.session, IHS_LogLevelInfo, "Data", "Stopping %s channel",
                   channelName);
    IHS_BufferClear(&frame.body, true);
    cls->stop((IHS_SessionChannel *)channel);
}

static void DataThreadInterrupt(IHS_SessionChannelData *channel) {
    if (!channel->worker || channel->interrupted) {
        return;
    }
    IHS_MutexLock(channel->lock);
    channel->interrupted = true;
    IHS_MutexUnlock(channel->lock);

    IHS_MutexLock(channel->windowLock);
    IHS_CondSignal(channel->windowCond);
    IHS_CondSignal(channel->feedbackCond);
    IHS_MutexUnlock(channel->windowLock);
}

void IHS_SessionChannelDataReceiveFrame(IHS_SessionChannelData *channel, IHS_SessionFrame *frame) {
    assert(frame->header.type == IHS_SessionPacketTypeUnreliable ||
           frame->header.type == IHS_SessionPacketTypeUnreliableFEC ||
           frame->header.type == IHS_SessionPacketTypeReliable);
    if (frame->body.size < 1)
        return;
    EStreamDataMessage type = *IHS_BufferPointer(&frame->body);
    IHS_BufferOffsetBy(&frame->body, 1);
    if (type != k_EStreamDataPacket) {
        return;
    }
    IHS_SessionDataFrameHeader header;
    bool hasHeader = false;
    if (frame->body.size >= IHS_SESSION_DATA_FRAME_HEADER_SIZE) {
        hasHeader = true;
        size_t offset = IHS_SessionChannelDataFrameHeaderParse(&header, &frame->body);
        IHS_BufferOffsetBy(&frame->body, (int)offset);
        header.sendTimestamp = frame->header.sendTimestamp;
        header.receiveTimestamp = frame->header.receiveTimestamp;
        header.receiveBegin = frame->header.receiveBegin;
    }
    const IHS_SessionChannelDataClass *cls = (const IHS_SessionChannelDataClass *)channel->base.cls;
    if (hasHeader)
        cls->dataFrame((IHS_SessionChannel *)channel, &header, &frame->body);
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
    *waitTicks = UINT32_MAX;
    if (!channel->feedbackDirty)
        return false;
    channel->feedbackDirty = false;
    IHS_MediaFeedback feedback;
    if (!IHS_SessionPacketsWindowMediaFeedback(channel->window, &feedback))
        return false;
    int32_t wait = 0;
    if (feedback.retransmitCount == 1)
        wait = (int32_t)(feedback.receiveTimestamp + 65 - now);
    if (channel->haveFeedbackDeadline) {
        int32_t deadline = (int32_t)(channel->feedbackDeadline - now);
        if (deadline > wait)
            wait = deadline;
    }
    if (wait > 0) {
        channel->feedbackDirty = true;
        *waitTicks = wait;
        return false;
    }
    IHS_SessionChannelInitializePacket(&channel->base, packet, IHS_SessionPacketTypeUnreliableNack,
                                       false, feedback.base);
    IHS_BufferAppendUInt32LE(&packet->body, channel->lastPacketTimestamp);
    IHS_BufferAppendUInt16LE(&packet->body, feedback.confirmed);
    IHS_BufferAppendMem(&packet->body, feedback.bitmap, feedback.bitmapBytes);
    channel->feedbackDeadline =
        now + IHS_StreamClockFeedbackTicks(&channel->base.session->clock);
    channel->haveFeedbackDeadline = true;
    *waitTicks = 0;
    return true;
}

static void FeedbackThreadWorker(IHS_SessionChannelData *channel) {
    IHS_MutexLock(channel->windowLock);
    while (!channel->interrupted) {
        uint32_t waitTicks;
        IHS_SessionPacket packet;
        if (IHS_SessionChannelDataBuildFeedback(channel, IHS_SessionPacketTimestamp(), &packet,
                                                &waitTicks)) {
            IHS_SessionChannelQueuePacket(&channel->base, &packet, false);
            IHS_SessionPacketClear(&packet, true);
        } else if (waitTicks == UINT32_MAX) {
            IHS_CondWait(channel->feedbackCond, channel->windowLock);
        } else {
            uint32_t ms = ((uint64_t)waitTicks * 1000 + 65535) / 65536;
            IHS_CondWaitTimeout(channel->feedbackCond, channel->windowLock, ms);
        }
    }
    IHS_MutexUnlock(channel->windowLock);
}

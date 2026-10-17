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

static void QueueFrame(IHS_SessionChannelData *channel, IHS_SessionFrame *frame) {
    /* Official client OnStreamPacket 7ad00c..7ad01c: transport windows,
     * reconstruction and feedback still run while paused; gate complete data
     * frames before handing them to the media consumer. Frames already owned
     * by that consumer are not retrospectively discarded on Pause. */
    IHS_Session *session = channel->base.session;
    if (frame->body.size && *IHS_BufferPointer(&frame->body) == k_EStreamDataPacket &&
        (session->state.connectionState != IHS_SessionConnectionStateConnected ||
         session->transport.clientPaused)) {
        IHS_BufferClear(&frame->body, true);
        return;
    }
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
    memset(frame, 0, sizeof(*frame));
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
    dataCh->receive = IHS_SessionUnreliableGet(channel->session, channel->id, windowCapacity);
    if (dataCh->receive)
        dataCh->receive->maxReceiveDelay = 6553; /* StartAudio/VideoData setter */
    dataCh->fecQueueCapacity = windowCapacity;
    dataCh->fecQueueLimit = (size_t)windowCapacity * 2048;
    /* Keep a single supported payload admissible even for small audio/test
     * windows. This is the local FIFO memory bound, not a wire limit. */
    if (dataCh->fecQueueLimit < IHS_FEC_MAX_PAYLOAD + sizeof(IHS_FECFrameNode))
        dataCh->fecQueueLimit = IHS_FEC_MAX_PAYLOAD + sizeof(IHS_FECFrameNode);
    atomic_init(&dataCh->interrupted, false);
    if (!dataCh->lock || !dataCh->windowLock || !dataCh->windowCond ||
        !dataCh->receive) {
        channel->initFailed = true;
        return false;
    }
    char threadName[16];
    snprintf(threadName, 16, "IHS%s", DataChannelName(channel->type));
    dataCh->worker = IHS_ThreadCreate((IHS_ThreadFunction *)DataThreadWorker, threadName, dataCh);
    channel->initFailed = !dataCh->worker;
    if (channel->initFailed)
        DataThreadInterrupt(dataCh);
    return !channel->initFailed;
}

void IHS_SessionChannelDataDeinit(IHS_SessionChannel *channel) {
    IHS_SessionChannelData *dataCh = (IHS_SessionChannelData *)channel;

    assert(!dataCh->worker || dataCh->interrupted);

    if (dataCh->worker)
        IHS_ThreadJoin(dataCh->worker);
    IHS_SessionFrame remaining = {0};
    while (PollFEC(dataCh, &remaining))
        IHS_BufferClear(&remaining.body, true);
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
    if (!data->interrupted) {
        IHS_SessionFrame owned = {.header = frame->header, .mediaOffset = frame->mediaOffset,
                                   .mediaPrepared = frame->mediaPrepared};
        IHS_BufferTransferOwnership(&frame->body, &owned.body);
        QueueFrame(data, &owned);
        IHS_CondSignal(data->windowCond);
    }
    IHS_MutexUnlock(data->windowLock);
}

bool IHS_SessionChannelDataPollLocked(IHS_SessionChannelData *channel, IHS_SessionFrame *frame,
                                       uint16_t *missing) {
    *missing = 0;
    return PollFEC(channel, frame);
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
        for (;;) {
            hasFrame = IHS_SessionChannelDataPollLocked(channel, &frame, &missing);
            if (hasFrame)
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
    IHS_SessionDataFrameHeader header = {0};
    bool hasHeader = false;
    if (frame->body.size >= IHS_SESSION_DATA_FRAME_HEADER_SIZE) {
        hasHeader = true;
        size_t offset = IHS_SessionChannelDataFrameHeaderParse(&header, &frame->body);
        IHS_BufferOffsetBy(&frame->body, (int)offset);
        header.sendTimestamp = frame->header.sendTimestamp;
        header.receiveTimestamp = frame->header.receiveTimestamp;
        header.receiveBegin = frame->header.receiveBegin;
        if (frame->mediaPrepared) {
            header.timestamp -= frame->mediaOffset;
            header.sendTimestamp -= frame->mediaOffset;
            if (header.inputRecvTimestamp) header.inputRecvTimestamp -= frame->mediaOffset;
            header.timestampsLocal = true;
            header.receivedRecorded = true;
        }
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
    return IHS_SessionUnreliableBuildFeedback(channel->receive, now, packet, waitTicks);
}

void IHS_SessionChannelDataUpdateAt(IHS_SessionChannelData *channel, uint32_t tick) {
    IHS_SessionUnreliableUpdateAt(channel->base.session, channel->base.id, tick);
}

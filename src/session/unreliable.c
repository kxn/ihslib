/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (c) 2022 Mariotaku. Receive logic extracted from ch_data.c. */
/* Official Channel receive/UpdateUnreliableState: protocol reference §25/58/69.
 * The connection owns receive windows/FEC; a decoder owns only its input FIFO. */
#include "unreliable.h"
#include "channels/ch_data.h"
#include "ihs_buffer_ext.h"
#include "session_pri.h"
#include <stdlib.h>

IHS_UnreliableChannel *IHS_SessionUnreliableGet(IHS_Session *s, uint8_t id, uint16_t capacity) {
    if (id >= 32)
        return NULL;
    if (s->unreliable[id])
        return s->unreliable[id];
    IHS_UnreliableChannel *rx = calloc(1, sizeof(*rx));
    if (!rx)
        return NULL;
    rx->base.session = s;
    rx->base.id = id;
    rx->maxReceiveDelay = 65; /* Original generic channel ctor, before Start*Data. */
    rx->window = IHS_SessionPacketsWindowCreateMedia(capacity);
    rx->fec = IHS_FECReceiverCreate();
    rx->lock = IHS_MutexCreate();
    if (!rx->window || !rx->fec || !rx->lock) {
        if (rx->window)
            IHS_SessionPacketsWindowDestroy(rx->window);
        if (rx->fec)
            IHS_FECReceiverDestroy(rx->fec);
        if (rx->lock)
            IHS_MutexDestroy(rx->lock);
        free(rx);
        return NULL;
    }
    s->unreliable[id] = rx;
    return rx;
}

bool IHS_SessionUnreliableSetDelay(IHS_Session *s, uint8_t id, uint32_t delay) {
    IHS_OutgoingRegister(s, id);
    IHS_UnreliableChannel *rx = IHS_SessionUnreliableGet(s, id, 2048);
    if (!rx) {
        IHS_SessionDisconnect(s);
        return false;
    }
    rx->maxReceiveDelay = delay;
    return true;
}

void IHS_SessionUnreliableDeinit(IHS_Session *s) {
    for (unsigned id = 0; id < 32; ++id) {
        IHS_UnreliableChannel *rx = s->unreliable[id];
        if (!rx)
            continue;
        IHS_SessionPacketsWindowDestroy(rx->window);
        IHS_FECReceiverDestroy(rx->fec);
        IHS_MutexDestroy(rx->lock);
        free(rx);
        s->unreliable[id] = NULL;
    }
}

void IHS_SessionUnreliableReceived(IHS_Session *s, IHS_SessionPacket *packet) {
    uint8_t id = packet->header.channelId;
    IHS_SessionPacketType type = packet->header.type;
    if (type != IHS_SessionPacketTypeUnreliable && type != IHS_SessionPacketTypeUnreliableFrag &&
        type != IHS_SessionPacketTypeUnreliableFEC)
        return;
    IHS_OutgoingRegister(s, id);
    IHS_UnreliableChannel *rx = IHS_SessionUnreliableGet(s, id, 2048);
    if (!rx) {
        IHS_SessionDisconnect(s);
        return;
    }
    IHS_MutexLock(rx->lock);
    if (type == IHS_SessionPacketTypeUnreliableFEC) {
        IHS_SessionFrame frame = {0};
        bool ready = IHS_FECReceiverAdd(rx->fec, packet, &frame);
        IHS_MutexUnlock(rx->lock);
        if (ready)
            IHS_SessionDeliverReliableFrame(s, &frame);
        IHS_SessionFrameClear(&frame, true);
        return;
    }
    bool inserted = false;
    if (!IHS_SessionPacketsWindowAddEx(rx->window, packet, &inserted)) {
        /* Existing local bounded-window recovery; not a new FEC retry policy. */
        IHS_SessionPacketsWindowReleaseAll(rx->window);
        IHS_SessionChannelDataLost(&rx->base);
        IHS_SessionPacketsWindowAddEx(rx->window, packet, &inserted);
    }
    if (!rx->lastPacketTimestamp ||
        (int32_t)(packet->header.sendTimestamp - rx->lastPacketTimestamp) > 0)
        rx->lastPacketTimestamp = packet->header.sendTimestamp;
    rx->feedbackDirty = true;
    IHS_MutexUnlock(rx->lock);
    if (inserted && packet->header.retransmitCount > 1)
        IHS_SessionRecordNetworkLost(s, packet->header.receiveTimestamp,
                                     packet->header.retransmitCount - 1u);
}

bool IHS_SessionUnreliableBuildFeedback(IHS_UnreliableChannel *channel, uint32_t now,
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
    channel->feedbackDeadline = now + IHS_StreamClockFeedbackTicks(&channel->base.session->clock);
    channel->haveFeedbackDeadline = true;
    *waitTicks = 0;
    return true;
}

void IHS_SessionUnreliableUpdateAt(IHS_Session *s, uint8_t id, uint32_t tick) {
    IHS_UnreliableChannel *rx = id < 32 ? s->unreliable[id] : NULL;
    if (!rx)
        return;
    for (;;) {
        /* Ordinary assembly is not a type11 payload. The completed frame's
         * retained allocation is bounded by the consumer FIFO, like reliable
         * media. Do not impose the unrelated FEC length assertion here. */
        IHS_SessionFrame frame = {.body = IHS_BUFFER_INIT(1024, 0)};
        uint16_t missing = 0;
        IHS_MutexLock(rx->lock);
        bool ready = IHS_SessionPacketsWindowPoll(rx->window, &frame);
        IHS_WindowPollError error = IHS_SessionPacketsWindowLastError(rx->window);
        if (!ready && error == IHS_WindowPollOK) {
            IHS_SessionPacketsWindowDiscardEx(
                rx->window, IHS_StreamClockReceiveDelayTicks(&s->clock, rx->maxReceiveDelay),
                &missing);
            ready = IHS_SessionPacketsWindowPoll(rx->window, &frame);
            error = IHS_SessionPacketsWindowLastError(rx->window);
        }
        IHS_MutexUnlock(rx->lock);
        if (error != IHS_WindowPollOK) {
            IHS_BufferClear(&frame.body, true);
            /* Local resource failure, not a packet gap: do not fabricate a
             * NACK/DataLost or retry while retaining an unassemblable head. */
            IHS_SessionLog(s, IHS_LogLevelError, "Data", "Frame assembly resource failure: %d",
                           error);
            IHS_SessionDisconnect(s);
            return;
        }
        if (ready)
            IHS_SessionDeliverReliableFrame(s, &frame);
        IHS_BufferClear(&frame.body, true);
        IHS_SessionRecordNetworkLost(s, tick, missing);
        if (!ready && !missing)
            break;
    }
    IHS_SessionPacket packet;
    uint32_t waitTicks;
    IHS_MutexLock(rx->lock);
    bool send = IHS_SessionUnreliableBuildFeedback(rx, tick, &packet, &waitTicks);
    IHS_MutexUnlock(rx->lock);
    if (send) {
        IHS_SessionSendPacket(s, &packet);
        IHS_SessionPacketClear(&packet, true);
    }
}

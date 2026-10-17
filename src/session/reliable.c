/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Reliable transport extracted from ch_control.c.
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * Official generic reliable channel; protocol reference §45–46. */
#include "reliable.h"
#include "ihs_buffer_ext.h"
#include "session_pri.h"

static void ReliableOnNackPacket(IHS_Session *, IHS_ReliableChannel *, IHS_SessionPacket *);
IHS_ReliableChannel *IHS_SessionReliableGet(IHS_Session *s, uint8_t channel) {
    if (channel >= 32)
        return NULL;
    IHS_OutgoingRegister(s, channel);
    return &s->reliable[channel];
}
static uint32_t ReliableDrainFrames(IHS_Session *s, IHS_ReliableChannel *state) {
    uint32_t retries = 0;
    IHS_SessionFrame frame = {.body = IHS_BUFFER_INIT(1024, 0)};
    for (;;) {
        IHS_MutexLock(s->reliableLock);
        bool ready = state->framePacketWindow &&
                     IHS_SessionPacketsWindowPoll(state->framePacketWindow, &frame);
        IHS_MutexUnlock(s->reliableLock);
        if (!ready)
            break;
        retries += frame.receivedRetries;
        IHS_SessionDeliverReliableFrame(s, &frame);
        IHS_SessionPacketsWindowReleaseFrame(&frame);
    }
    IHS_BufferClear(&frame.body, true);
    return retries;
}
/* Original Channel::UpdateReliableState; see protocol reference §45. Run on
 * the receive worker so frame/decrypt callbacks keep their existing thread. */
void IHS_SessionReliableUpdateChannel(IHS_Session *session, uint8_t channelId) {
    if (channelId >= 32)
        return;
    IHS_ReliableChannel *state = &session->reliable[channelId];
    IHS_SessionChannel endpoint = {.id = channelId, .session = session};
    IHS_SessionChannel *channel = &endpoint;
    IHS_MutexLock(session->reliableLock);
    if (!state->feedbackDirty) {
        IHS_MutexUnlock(session->reliableLock);
        return;
    }
    state->feedbackDirty = false;
    uint32_t now = IHS_SessionPacketTimestamp();
    IHS_MutexUnlock(session->reliableLock);
    /* Application delegates retain their existing lock domain. Only feedback
     * state + Socket attempts need Base -> reliableLock serialization. */
    uint32_t retries = ReliableDrainFrames(session, state);
    IHS_SessionRecordNetworkLost(session, now, retries);
    IHS_BaseLock(&session->base);
    IHS_MutexLock(session->reliableLock);
    bool eligible = (int32_t)(now - state->feedbackDeadline) >= 0;
    bool attempted = false;
    uint16_t confirmed = IHS_SessionPacketsWindowContiguousId(state->framePacketWindow);
    if (confirmed != state->ackWatermark) {
        state->ackWatermark = confirmed;
        state->ackPending = true;
    }
    if (state->ackPending) {
        if (eligible) {
            IHS_SessionChannelPacketAck(channel, confirmed, 0, true,
                                        state->peerTimestamp + IHS_SessionPacketTimestamp() -
                                            state->peerReceiveTime);
            state->ackPending = false;
            attempted = true;
        } else {
            state->feedbackDirty = true;
        }
    }
    IHS_MediaFeedback feedback;
    bool mature;
    if (IHS_SessionPacketsWindowReliableFeedback(state->framePacketWindow, now, &feedback,
                                                 &mature)) {
        if (eligible && mature) {
            IHS_SessionPacket packet;
            IHS_SessionChannelInitializePacket(channel, &packet, IHS_SessionPacketTypeNACK, false,
                                               feedback.base);
            packet.header.fragmentId = 0;
            IHS_BufferAppendUInt32LE(&packet.body, state->peerTimestamp);
            IHS_BufferAppendUInt16LE(&packet.body, confirmed);
            IHS_BufferAppendMem(&packet.body, feedback.bitmap, feedback.bitmapBytes);
            /* Official attempts synchronously and does not retry this feedback
             * on Socket errors. A new receive event can re-arm the channel. */
            IHS_SessionSendPacket(session, &packet);
            IHS_SessionPacketClear(&packet, true);
            attempted = true;
        } else {
            state->feedbackDirty = true;
        }
    }
    if (attempted)
        state->feedbackDeadline = now + IHS_StreamClockFeedbackTicks(&session->clock);
    IHS_MutexUnlock(session->reliableLock);
    IHS_BaseUnlock(&session->base);
}

void IHS_SessionReliableReceived(IHS_Session *session, IHS_SessionPacket *packet) {
    IHS_ReliableChannel *state = IHS_SessionReliableGet(session, packet->header.channelId);
    if (!state)
        return;
    /* Receive only records the event; the RX worker's Update drains complete
     * frames and emits coalesced feedback, like the official channel. */
    switch (packet->header.type) {
    case IHS_SessionPacketTypeReliable:
    case IHS_SessionPacketTypeReliableFrag:
        IHS_MutexLock(session->reliableLock);
        state->feedbackDirty = true;
        if (!state->peerTimestamp ||
            (int32_t)(packet->header.sendTimestamp - state->peerTimestamp) > 0) {
            state->peerTimestamp = packet->header.sendTimestamp;
            state->peerReceiveTime = packet->header.receiveTimestamp;
        }
        if (!state->framePacketWindow)
            state->framePacketWindow = IHS_SessionPacketsWindowCreateReliable(64, 0);
        IHS_SessionPacketsWindow *window = state->framePacketWindow;
        if (window) {
            if ((int16_t)(IHS_SessionPacketsWindowNextNeededPacketId(window) -
                          packet->header.packetId) > 0)
                state->ackPending = true;
        }
        bool inserted = false;
        bool added = window && IHS_SessionPacketsWindowAddEx(window, packet, &inserted);
        if (!window)
            state->feedbackDirty = false;
        IHS_MutexUnlock(session->reliableLock);
        if (inserted && packet->header.retransmitCount > 1)
            IHS_SessionRecordNetworkLost(session, packet->header.receiveTimestamp,
                                          packet->header.retransmitCount - 1u);
        if (!added) {
            /* Log and disconnect once: the packets keep coming, and firing a
             * disconnect per packet floods the link and re-enters teardown. */
            if (!state->overflowed) {
                state->overflowed = true;
                IHS_SessionLog(session, IHS_LogLevelError, "Reliable", "Frames window overflow");
                IHS_SessionDisconnect(session);
            }
            return;
        }
        break;
    case IHS_SessionPacketTypeACK:
        /* SessionRecvCallback already processed the cumulative ACK. */
        break;
    case IHS_SessionPacketTypeNACK:
        ReliableOnNackPacket(session, state, packet);
        break;
    default:
        /* Official ignores unrecognized channel packet types (its channel
         * HandlePacket default falls through without action). Killing the
         * session on one unexpected packet turns a harmless difference
         * into a dead stream. */
        IHS_SessionLog(session, IHS_LogLevelWarn, "Reliable",
                       "Ignoring unrecognized packet type %u\n", packet->header.type);
        break;
    }
    /* Frame callbacks belong to the next channel Update, not HandlePacket. */
}

/* Peer reports its receive state for OUR reliable stream. Extended NACK body
 * (wire offsets after the 13B header): [0..3] u32 timestamp, [4..5] u16
 * contiguous delivery point, [6..] presence mask (bit=1: peer holds it).
 * bit=1 releases the packet from retransmission; bit=0 forces a resend.
 * Header-only (empty body) is the simple form: everything the reference
 * (header.packetId) covers is missing on the peer. */
static void ReliableOnNackPacket(IHS_Session *session, IHS_ReliableChannel *state,
                                 IHS_SessionPacket *packet) {
    uint64_t nowMs = IHS_TimerNow();
    /* HandleNackPacket 0x7f9610: ignore stale feedback, allowing equality. */
    if (state->lastNackTimestamp &&
        (int32_t)(packet->header.sendTimestamp - state->lastNackTimestamp) < 0)
        return;
    /* Timestamp zero is the source sentinel; even malformed bodies consume
     * a non-stale timestamp before the body-size guard. */
    state->lastNackTimestamp = packet->header.sendTimestamp;
    if (packet->body.size != 0 && packet->body.size < 6)
        return;
    uint32_t cutoff = IHS_SessionPacketTimestamp() - IHS_StreamClockNackAgeTicks(&session->clock);
    if (packet->body.size >= 6) {
        const uint8_t *body = IHS_BufferPointer(&packet->body);
        uint32_t seen =
            body[0] | (uint32_t)body[1] << 8 | (uint32_t)body[2] << 16 | (uint32_t)body[3] << 24;
        /* csel ..., lt at 0x7f977c selects seen when cutoff < seen (max). */
        if ((int32_t)(cutoff - seen) < 0)
            cutoff = seen;
        uint16_t contiguous = (uint16_t)(body[4] | (body[5] << 8));
        IHS_RetransmissionAcknowledgeThrough(&session->retransmission, packet->header.channelId,
                                             (uint16_t)(contiguous + 1u), nowMs);
        size_t maskLen = packet->body.size - 6;
        for (size_t j = 0; j < maskLen; j++) {
            uint8_t bits = body[6 + j];
            /* 0x7f9840 skips zero bytes; 0x7f9868..78 stops after the highest
             * set bit. Trailing zero bits are not explicit resend requests. */
            for (size_t k = 0; bits; k++, bits >>= 1) {
                uint16_t id = (uint16_t)(packet->header.packetId + j * 8 + k);
                if (bits & 1u) {
                    IHS_RetransmissionAcknowledge(&session->retransmission,
                                                  packet->header.channelId, id, INT16_MIN, nowMs);
                } else {
                    IHS_RetransmissionNackBefore(&session->retransmission, packet->header.channelId,
                                                 id, false, cutoff, nowMs);
                }
            }
        }
    }
    /* Header-only and extended forms also request IDs BELOW the bitmap base. */
    IHS_RetransmissionNackBefore(&session->retransmission, packet->header.channelId,
                                 packet->header.packetId, true, cutoff, nowMs);
}

void IHS_SessionReliableUpdate(IHS_Session *s) {
    IHS_MutexLock(s->sendQueueMutex);
    unsigned count = s->outgoing.count;
    IHS_MutexUnlock(s->sendQueueMutex);
    for (unsigned ch = 0; ch < count; ++ch)
        IHS_SessionReliableUpdateChannel(s, ch);
}
void IHS_SessionReliableDeinit(IHS_Session *s) {
    for (unsigned ch = 0; ch < 32; ++ch)
        if (s->reliable[ch].framePacketWindow)
            IHS_SessionPacketsWindowDestroy(s->reliable[ch].framePacketWindow);
    IHS_MutexDestroy(s->reliableLock);
}

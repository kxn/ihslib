/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Official Android 1.3.32: SendUnreliablePackets 0x7f8cbc, NACK 0x7f95dc,
 * Socket::Send 0x7ff1dc, Connection::Update 0x7fc108. See protocol RE §37–38. */
#include "outgoing.h"
#include "session_pri.h"
#include <stdlib.h>
#include <string.h>

static int packet_class(unsigned type) {
    type &= 127;
    return type == 3 || type == 4 ? 0 : type == 5 || type == 6 ? 1 : -1;
}
void IHS_OutgoingInit(IHS_Session *s, uint32_t now) {
    memset(&s->outgoing, 0, sizeof(s->outgoing));
    s->outgoing.rate[0] = 12500000;
    s->outgoing.rate[1] = 625000;
    s->outgoing.deadline[0] = s->outgoing.deadline[1] = now;
}
static void release(IHS_OutgoingWindow *w, uint16_t id) {
    if (!w->capacity)
        return;
    unsigned slot = id & (w->capacity - 1);
    IHS_OutgoingPacket *p = w->slots[slot];
    if (p) {
        IHS_SessionPacketClear(&p->packet, true);
        free(p);
        w->slots[slot] = NULL;
    }
}
static IHS_OutgoingPacket *get(IHS_OutgoingWindow *w, uint16_t id) {
    if (!w->capacity || (uint16_t)(id - w->head) >= (uint16_t)(w->tail - w->head))
        return NULL;
    return w->slots[id & (w->capacity - 1)];
}
static void advance(IHS_OutgoingWindow *w, unsigned count) {
    while (count--) {
        release(w, w->head);
        if (w->capacity)
            w->created[w->head & (w->capacity - 1)] = 0;
        w->head++;
    }
}
void IHS_OutgoingDeinit(IHS_Session *s) {
    for (unsigned ch = 0; ch < 32; ++ch) {
        IHS_OutgoingWindow *w = &s->outgoing.unreliable[ch];
        advance(w, (uint16_t)(w->tail - w->head));
        free(w->slots);
        free(w->created);
    }
}
void IHS_OutgoingRegister(IHS_Session *s, uint8_t channel) {
    if (channel >= 32)
        return;
    IHS_MutexLock(s->sendQueueMutex);
    /* GetChannel 0x7fba68 returns an existing slot directly; creating a new
     * channel fills every missing slot from zero through the requested ID. */
    if (!s->outgoing.active[channel])
        for (unsigned id = 0; id <= channel; ++id)
            if (!s->outgoing.active[id]) {
                s->reliable[id].feedbackDeadline = IHS_SessionPacketTimestamp();
                s->outgoing.active[id] = true;
                s->outgoing.creation[s->outgoing.count] = id;
                s->outgoing.order[s->outgoing.count++] = id;
                s->outgoing.dirty = true;
            }
    IHS_MutexUnlock(s->sendQueueMutex);
}
void IHS_OutgoingPriority(IHS_Session *s, uint8_t channel, int32_t priority) {
    if (channel >= 32)
        return;
    IHS_MutexLock(s->sendQueueMutex);
    IHS_OutgoingRegister(s, channel);
    s->outgoing.priority[channel] = priority;
    s->outgoing.dirty = true;
    IHS_MutexUnlock(s->sendQueueMutex);
}
uint16_t IHS_OutgoingNextId(IHS_Session *s, uint8_t channel, bool reliable) {
    if (channel >= 32)
        return 0;
    IHS_MutexLock(s->sendQueueMutex);
    uint16_t id =
        reliable ? s->outgoing.reliableId[channel]++ : s->outgoing.unreliableId[channel]++;
    IHS_MutexUnlock(s->sendQueueMutex);
    return id;
}
bool IHS_OutgoingCanSend(IHS_Session *s, unsigned type, uint32_t now) {
    int cls = packet_class(type);
    bool allowed = cls < 0 || (int32_t)(now - s->outgoing.deadline[cls]) >= 0;
    s->outgoing.blocked = !allowed;
    return allowed;
}
void IHS_OutgoingSent(IHS_Session *s, unsigned type, uint32_t stamp, uint32_t wireBytes) {
    int cls = packet_class(type);
    if (cls < 0)
        return;
    int32_t rate = s->outgoing.rate[cls];
    uint32_t deadline = stamp;
    if (rate) {
        uint32_t floor = stamp - 65;
        uint32_t prior = s->outgoing.deadline[cls];
        if ((int32_t)(prior - floor) >= 0)
            floor = prior;
        /* The ARM implementation shifts a signed 32-bit numerator then sdiv;
         * widen the divide to avoid C's INT_MIN/-1 UB while retaining its bits. */
        int64_t numerator = (int32_t)(wireBytes << 16);
        deadline = floor + (uint32_t)(numerator / rate);
    }
    s->outgoing.deadline[cls] = deadline;
}
static bool capacity(IHS_OutgoingWindow *w, unsigned need) {
    if (need <= w->capacity)
        return true;
    if (need > 32768)
        return false; /* Reject ambiguous half-range spans. */
    unsigned size = w->capacity ? w->capacity : 64;
    while (size < need)
        size *= 2;
    IHS_OutgoingPacket **slots = calloc(size, sizeof(*slots));
    uint32_t *times = calloc(size, sizeof(*times));
    if (!slots || !times) {
        free(slots);
        free(times);
        return false;
    }
    if (w->capacity)
        for (unsigned i = 0; i < (uint16_t)(w->tail - w->head); ++i) {
            uint16_t id = w->head + i;
            slots[id & (size - 1)] = w->slots[id & (w->capacity - 1)];
            times[id & (size - 1)] = w->created[id & (w->capacity - 1)];
        }
    free(w->slots);
    free(w->created);
    w->slots = slots;
    w->created = times;
    w->capacity = size;
    return true;
}
bool IHS_OutgoingRetain(IHS_Session *s, IHS_SessionPacket *packet, uint32_t created) {
    uint8_t ch = packet->header.channelId;
    if (ch >= 32)
        return false;
    IHS_BaseLock(&s->base);
    IHS_OutgoingRegister(s, ch);
    IHS_OutgoingWindow *w = &s->outgoing.unreliable[ch];
    uint16_t id = packet->header.packetId;
    if (!w->initialized) {
        w->head = w->tail = w->cursor = id;
        w->initialized = true;
    }
    unsigned distance = (uint16_t)(id - w->head);
    bool ok = distance < 32768 && capacity(w, distance + 1);
    IHS_OutgoingPacket *copy = ok ? calloc(1, sizeof(*copy)) : NULL;
    if (copy && !w->slots[id & (w->capacity - 1)]) {
        copy->packet.header = packet->header;
        copy->packet.crc = packet->crc;
        IHS_BufferTransferOwnership(&packet->body, &copy->packet.body);
        w->slots[id & (w->capacity - 1)] = copy;
        w->created[id & (w->capacity - 1)] = created;
        if ((uint16_t)(w->tail - w->head) <= distance)
            w->tail = id + 1;
    } else {
        free(copy);
        ok = false;
    }
    IHS_BaseUnlock(&s->base);
    return ok;
}
static int attempt(IHS_Session *s, IHS_OutgoingPacket *p) {
    int result = IHS_SessionSendPacketResult(s, &p->packet);
    if (!result && p->packet.header.retransmitCount < 255)
        p->packet.header.retransmitCount++;
    return result;
}
int IHS_OutgoingSendUnreliable(IHS_Session *s, uint8_t ch, bool retry) {
    if (ch >= 32)
        return 2;
    IHS_BaseLock(&s->base);
    uint32_t now = IHS_SessionPacketTimestamp();
    IHS_OutgoingWindow *w = &s->outgoing.unreliable[ch];
    float rttMs = IHS_StreamClockRTTMilliseconds(&s->clock);
    uint32_t ms = (uint32_t)(rttMs + 2.0f);
    uint32_t keep = 13107 + 2 * (uint32_t)((uint64_t)ms * 65536 / 1000);
    while (w->head != w->tail &&
           (int32_t)(w->created[w->head & (w->capacity - 1)] + keep - now) < 0)
        advance(w, 1);
    if ((uint16_t)(w->cursor - w->head) > (uint16_t)(w->tail - w->head))
        w->cursor = w->head;
    uint16_t start = w->cursor;
    int result = 0;
    while (w->cursor != w->tail) {
        IHS_OutgoingPacket *p = get(w, w->cursor);
        if (p && (result = attempt(s, p)))
            break;
        w->cursor++;
    }
    if (!result && retry)
        for (uint16_t id = w->head; id != start; ++id) {
            IHS_OutgoingPacket *p = get(w, id);
            if (p && !p->packet.header.sendTimestamp && (result = attempt(s, p)))
                break;
        }
    IHS_BaseUnlock(&s->base);
    return result == 1 ? 0 : result;
}
static void mark(IHS_OutgoingWindow *w, uint16_t id, uint32_t cutoff) {
    IHS_OutgoingPacket *p = get(w, id);
    if (p && p->packet.header.retransmitCount && p->packet.header.sendTimestamp &&
        (int32_t)(cutoff - p->packet.header.sendTimestamp) >= 0)
        p->packet.header.sendTimestamp = 0;
}
void IHS_OutgoingNack(IHS_Session *s, const IHS_SessionPacket *packet) {
    uint8_t ch = packet->header.channelId;
    if (ch >= 32)
        return;
    IHS_BaseLock(&s->base);
    uint32_t now = IHS_SessionPacketTimestamp();
    IHS_OutgoingWindow *w = &s->outgoing.unreliable[ch];
    uint32_t stamp = packet->header.sendTimestamp;
    if (w->lastFeedback && (int32_t)(stamp - w->lastFeedback) < 0)
        goto done;
    w->lastFeedback = stamp;
    size_t size = packet->body.size;
    if (size && size < 6)
        goto done;
    uint32_t cutoff = now - IHS_StreamClockNackAgeTicks(&s->clock);
    uint16_t base = packet->header.packetId;
    if (size >= 6) {
        const uint8_t *b = IHS_BufferPointer(&packet->body);
        uint32_t echo =
            (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        if ((int32_t)(cutoff - echo) < 0)
            cutoff = echo;
        uint16_t confirmed = (uint16_t)(b[4] | (uint16_t)b[5] << 8);
        int count = (int16_t)(confirmed - w->head + 1);
        unsigned span = (uint16_t)(w->tail - w->head);
        if (count > 0)
            advance(w, (unsigned)count > span ? span : (unsigned)count);
        while (w->head != w->tail && !get(w, w->head))
            advance(w, 1);
        for (size_t j = 0; j < size - 6; ++j)
            for (unsigned bit = 0, bits = b[6 + j]; bits; ++bit, bits >>= 1) {
                uint16_t id = base + j * 8 + bit;
                if (bits & 1) {
                    if ((int16_t)(w->cursor - id) > 0 && get(w, id))
                        release(w, id);
                } else
                    mark(w, id, cutoff);
            }
    }
    int count = (int)base - (int)w->head;
    unsigned span = (uint16_t)(w->tail - w->head);
    if (count > 0)
        for (unsigned i = 0; i < ((unsigned)count > span ? span : (unsigned)count); ++i)
            mark(w, w->head + i, cutoff);
done:
    IHS_BaseUnlock(&s->base);
}
static bool send_reliable(IHS_SessionPacket *p, void *ctx) {
    return IHS_SessionSendPacket(ctx, p);
}
unsigned IHS_OutgoingSnapshot(IHS_Session *s, uint8_t order[32], unsigned *rotation) {
    IHS_Outgoing *o = &s->outgoing;
    IHS_MutexLock(s->sendQueueMutex);
    if (o->dirty) {
        memcpy(o->order, o->creation, o->count);
        IHS_OutgoingSort(o);
        o->dirty = false;
    }
    unsigned count = o->count;
    if (rotation)
        *rotation = o->rotation;
    memcpy(order, o->order, count);
    IHS_MutexUnlock(s->sendQueueMutex);
    return count;
}
void IHS_OutgoingUpdateChannel(IHS_Session *s, uint8_t channel, uint32_t tick) {
    IHS_BaseLock(&s->base);
    if (!s->base.interrupted && !s->transport.detached &&
        IHS_SessionTransportState(s) == IHS_TransportConnected) {
        /* Source 7fc390 passes the batch tick to SendReliablePackets; the
         * unreliable sender and Socket::Send obtain fresh timestamps. */
        IHS_RetransmissionProcessChannel(&s->retransmission, IHS_TimerNow(), tick, channel,
                                         send_reliable, s);
        IHS_OutgoingSendUnreliable(s, channel, false);
    }
    IHS_BaseUnlock(&s->base);
}
void IHS_OutgoingRetryPass(IHS_Session *s, const uint8_t order[32], unsigned count,
                           unsigned rotation) {
    IHS_BaseLock(&s->base);
    if (s->base.interrupted || s->transport.detached ||
        IHS_SessionTransportState(s) != IHS_TransportConnected) {
        IHS_BaseUnlock(&s->base);
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (rotation >= count)
            rotation = 0;
        IHS_OutgoingSendUnreliable(s, order[rotation++], true);
    }
    IHS_MutexLock(s->sendQueueMutex);
    if (++s->outgoing.rotation > count)
        s->outgoing.rotation = 0;
    IHS_MutexUnlock(s->sendQueueMutex);
    IHS_BaseUnlock(&s->base);
}
void IHS_OutgoingUpdate(IHS_Session *s, uint32_t now) {
    IHS_BaseLock(&s->base);
    if (IHS_SessionTransportState(s) != IHS_TransportConnected || s->transport.detached) {
        IHS_BaseUnlock(&s->base);
        return;
    }
    uint8_t order[32];
    unsigned rotation;
    unsigned count = IHS_OutgoingSnapshot(s, order, &rotation);
    /* Standalone component boundary. Production interleaves RX and TX for
     * each channel in IHS_SessionNetworkUpdateAt, using this same snapshot. */
    for (unsigned i = 0; i < count; ++i)
        IHS_OutgoingUpdateChannel(s, order[i], now);
    IHS_OutgoingRetryPass(s, order, count, rotation);
    IHS_BaseUnlock(&s->base);
}

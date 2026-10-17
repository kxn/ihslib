/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "network_stats.h"
#include "session_pri.h"
#include <stdlib.h>
#include <string.h>

#define WINDOW_TICKS 0x30000u

void IHS_SessionRecordNetworkReceived(IHS_Session *s, uint32_t now, int32_t bytes) {
    IHS_BaseLock(&s->base);
    IHS_NetworkPacketRecord(&s->networkPackets, now, 1, false);
    IHS_NetworkBitrateRecord(&s->networkRates[1], now, bytes);
    IHS_BaseUnlock(&s->base);
}
void IHS_SessionRecordNetworkSent(IHS_Session *s, uint32_t now, int32_t bytes) {
    IHS_BaseLock(&s->base);
    IHS_NetworkBitrateRecord(&s->networkRates[0], now, bytes);
    IHS_BaseUnlock(&s->base);
}
void IHS_SessionGetNetworkSample(IHS_Session *s, float values[5]) {
    IHS_BaseLock(&s->base);
    values[0] = IHS_StreamClockRTTMilliseconds(&s->clock);
    values[1] = s->networkRates[1].bitrate * 0.001f;
    values[2] = s->networkRates[0].bitrate * 0.001f;
    /* Video link sampling still needs the actual frame receive metadata.
     * It was zero before this connection-sampling change; protocol §57. */
    values[3] = 0;
    values[4] = s->networkPackets.total
        ? ((float)(int32_t)s->networkPackets.lost * 100.0f) / (float)(int32_t)s->networkPackets.total
        : 0;
    IHS_BaseUnlock(&s->base);
}

void IHS_NetworkSampleQueueDestroy(IHS_NetworkSampleQueue *q) {
    free(q->data);
    memset(q, 0, sizeof(*q));
}
static IHS_NetworkSample *at(IHS_NetworkSampleQueue *q, size_t index) {
    return &q->data[(q->head + index) & (q->capacity - 1)];
}
static bool reserve(IHS_NetworkSampleQueue *q) {
    if (q->count < q->capacity) return true;
    size_t capacity = q->capacity ? q->capacity * 2 : 8;
    if (capacity < q->capacity || capacity > SIZE_MAX / sizeof(*q->data)) return false;
    IHS_NetworkSample *data = malloc(capacity * sizeof(*data));
    if (!data) return false;
    for (size_t i = 0; i < q->count; ++i) data[i] = *at(q, i);
    free(q->data);
    q->data = data;
    q->capacity = capacity;
    q->head = 0;
    return true;
}
static bool expired(IHS_NetworkSampleQueue *q, uint32_t now) {
    return q->count && (uint32_t)(now - at(q, 0)->timestamp) > WINDOW_TICKS;
}
static void pop(IHS_NetworkSampleQueue *q) {
    q->head = (q->head + 1) & (q->capacity - 1);
    --q->count;
}
static void push(IHS_NetworkSampleQueue *q, IHS_NetworkSample sample) {
    *at(q, q->count++) = sample;
}
bool IHS_NetworkBitrateRecord(IHS_NetworkBitrateStats *s, uint32_t now, int32_t bytes) {
    IHS_NetworkSampleQueue *q = &s->queue;
    IHS_NetworkSample *last = q->count ? at(q, q->count - 1) : NULL;
    if (last && last->timestamp == now) {
        last->values[0] += (uint32_t)bytes;
    } else {
        if (!reserve(q)) return false;
        while (expired(q, now)) {
            s->bytes -= (uint64_t)(int64_t)(int32_t)at(q, 0)->values[0];
            pop(q);
        }
        push(q, (IHS_NetworkSample){.timestamp = now, .values = {(uint32_t)bytes}});
    }
    s->bytes += (uint64_t)(int64_t)bytes;
    s->bitrate = (float)(int64_t)(s->bytes << 3) / 3.0f;
    return true;
}
bool IHS_NetworkPacketRecord(IHS_NetworkPacketStats *s, uint32_t now, int32_t count, bool lost) {
    IHS_NetworkSampleQueue *q = &s->queue;
    IHS_NetworkSample *last = q->count ? at(q, q->count - 1) : NULL;
    if (last && last->timestamp == now && last->values[1] == lost) {
        last->values[0] += (uint32_t)count;
    } else {
        if (!reserve(q)) return false;
        while (expired(q, now)) {
            IHS_NetworkSample *old = at(q, 0);
            s->total -= old->values[0];
            if (old->values[1]) s->lost -= old->values[0];
            pop(q);
        }
        push(q, (IHS_NetworkSample){.timestamp = now, .values = {(uint32_t)count, lost}});
    }
    s->total += (uint32_t)count;
    if (lost) s->lost += (uint32_t)count;
    return true;
}
float IHS_NetworkPingMeanMilliseconds(uint32_t sum, uint32_t count) {
    uint32_t mean = count ? sum / count : 0;
    float whole = (float)((mean >> 16) * 1000u);
    float fraction = (float)((mean & 65535u) * 1000u) * (1.0f / 65536.0f);
    return whole + fraction;
}
bool IHS_NetworkPingRecord(IHS_NetworkPingStats *s, uint32_t ticks, uint32_t now) {
    IHS_NetworkSampleQueue *q = &s->queue;
    if (!reserve(q)) return false;
    while (expired(q, now)) {
        s->sum -= at(q, 0)->values[0];
        pop(q);
    }
    push(q, (IHS_NetworkSample){.timestamp = now, .values = {ticks}});
    s->sum += ticks;
    s->milliseconds = IHS_NetworkPingMeanMilliseconds(s->sum, (uint32_t)q->count);
    return true;
}
static float seconds(uint32_t ticks) {
    return (float)(ticks >> 16) + (float)(ticks & 65535u) * (1.0f / 65536.0f);
}
bool IHS_NetworkVideoRecord(IHS_NetworkVideoStats *s, uint32_t now,
                            uint32_t receiveBegin, uint32_t receiveEnd, uint16_t frame, int32_t bytes) {
    if (s->frame != frame) {
        if (s->firstSent && s->frameBytes &&
            (int32_t)(s->lastReceived - s->firstSent) > 0 &&
            (int32_t)(now - s->lastReceived) >= 0) {
            IHS_NetworkSampleQueue *q = &s->queue;
            if (!reserve(q)) return false;
            while (expired(q, s->lastReceived)) {
                for (unsigned i = 0; i < 4; ++i) s->totals[i] -= at(q, 0)->values[i];
                pop(q);
            }
            IHS_NetworkSample sample = {.timestamp = s->lastReceived,
                .values = {receiveEnd - receiveBegin, s->lastReceived - s->firstSent,
                           now - s->lastReceived, s->frameBytes}};
            push(q, sample);
            for (unsigned i = 0; i < 4; ++i) s->totals[i] += sample.values[i];
            if (q->count >= 10) {
                s->bitrate = ((float)s->totals[3] / seconds(s->totals[1] + s->totals[2])) * 8.0f;
                if (s->totals[0]) s->link = ((float)s->totals[3] / seconds(s->totals[0])) * 8.0f;
            }
        }
        s->frame = frame;
        s->firstSent = now;
        s->frameBytes = 0;
    }
    s->lastReceived = receiveEnd;
    s->frameBytes += (uint32_t)bytes;
    return true;
}

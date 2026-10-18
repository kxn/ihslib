/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Official CNetworkStats 77a0f8/77a15c/77a228/77a494/77a844/77a908;
 * source observations and floating-point branches are in protocol RE §117. */
#include "media_network_stats.h"
#include "session_pri.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void IHS_MediaNetworkStatsInit(IHS_MediaNetworkStats *s) {
    memset(s, 0, sizeof(*s));
    s->valid = true;
}
void IHS_MediaNetworkStatsDestroy(IHS_MediaNetworkStats *s) {
    free(s->windows);
    memset(s, 0, sizeof(*s));
}
void IHS_MediaNetworkStatsStart(IHS_MediaNetworkStats *s) {
    s->next = 0;
    s->total = s->batch = 0;
    s->count = s->windowCount = 0;
    s->average = (IHS_MediaNetworkWindow){0};
    s->valid = true;
    s->finalized = false;
}
static float milliseconds(uint32_t ticks) {
    float whole = (float)((ticks >> 16) * 1000u);
    float part = (float)((ticks & 65535u) * 1000u) * (1.0f / 65536.0f);
    return whole + part;
}
IHS_MediaNetworkWindow IHS_MediaNetworkStatsCurrent(const IHS_MediaNetworkStats *s) {
    IHS_MediaNetworkWindow result = {.loss = 100.0f};
    if (s->batch)
        result.loss -= ((float)s->count * 100.0f) / (float)s->batch;
    if (!s->count)
        return result;
    float sum = 0;
    for (size_t i = 0; i < s->count; ++i)
        sum += milliseconds(s->latencies[i]);
    result.mean = sum / (float)s->count;
    if (s->count < 2)
        return result;
    float variance = 0;
    size_t vectorCount = s->count & ~(size_t)7;
    for (size_t i = 0; i < s->count; ++i) {
        float difference = milliseconds(s->latencies[i]) - result.mean;
        if (i < vectorCount) {
            /* Source's eight-wide branch rounds multiply before add. The
             * scalar tail uses FMADD. Prevent compiler contraction here. */
            volatile float square = difference * difference;
            variance += square;
        } else {
            variance = fmaf(difference, difference, variance);
        }
    }
    result.deviation = sqrtf(variance / (float)(s->count - 1));
    return result;
}
static bool reserve(IHS_MediaNetworkStats *s) {
    if (s->windowCount < s->capacity)
        return true;
    size_t size = s->capacity ? s->capacity * 2 : 8;
    if (size < s->capacity || size > SIZE_MAX / sizeof(*s->windows))
        return false;
    IHS_MediaNetworkWindow *windows = realloc(s->windows, size * sizeof(*windows));
    if (!windows)
        return false;
    s->windows = windows;
    s->capacity = size;
    return true;
}
bool IHS_MediaNetworkStatsFold(IHS_MediaNetworkStats *s) {
    if (!s->valid || !reserve(s)) {
        /* Local allocation guard: stop trusting this collector, while media
         * delivery and transport continue. Start can reset this condition. */
        s->valid = false;
        s->batch = 0;
        s->count = 0;
        return false;
    }
    s->windows[s->windowCount++] = IHS_MediaNetworkStatsCurrent(s);
    s->batch = 0;
    s->count = 0;
    return true;
}
bool IHS_MediaNetworkStatsRecord(IHS_MediaNetworkStats *s, uint16_t frame, uint32_t sent,
                                 uint32_t received) {
    if ((int16_t)(s->next - frame) > 0)
        return true;
    uint32_t span = (uint16_t)(frame - s->next) + 1u;
    s->total += span;
    s->next = frame + 1u;
    if (!s->valid)
        return false;
    s->batch += span;
    uint32_t elapsed = received - sent;
    if ((int32_t)elapsed < 0)
        elapsed = 0;
    /* Each received frame advances the span at least once. A completed
     * batch is folded below, so at most 600 latency samples are stored. */
    assert(s->count < 600);
    s->latencies[s->count++] = elapsed;
    if (s->batch >= 600)
        return IHS_MediaNetworkStatsFold(s);
    return true;
}
bool IHS_MediaNetworkStatsStop(IHS_MediaNetworkStats *s) {
    bool ok = IHS_MediaNetworkStatsFold(s);
    s->finalized = true;
    if (!ok)
        return false;
    IHS_MediaNetworkWindow sum = {0};
    for (size_t i = 0; i < s->windowCount; ++i) {
        sum.loss += s->windows[i].loss;
        sum.mean += s->windows[i].mean;
        sum.deviation += s->windows[i].deviation;
    }
    float count = (float)s->windowCount;
    s->average =
        (IHS_MediaNetworkWindow){sum.loss / count, sum.mean / count, sum.deviation / count};
    return true;
}
unsigned IHS_MediaNetworkStatsResult(float mean, float deviation, float loss) {
    /* Source's ARM signed-LT branches include unordered floating compares.
     * Negated GE preserves that behavior for explicit NaN component inputs. */
    if (mean == 0 || deviation >= 16.0f || loss >= 1.0f)
        return 0;
    return !(loss >= 0.25f) && !(deviation >= 8.0f) ? 2 : 1;
}
void IHS_SessionGetMediaNetworkStats(IHS_Session *s, IHS_SessionMediaNetworkStats *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!s)
        return;
    IHS_BaseLock(&s->base);
    const IHS_MediaNetworkStats *stats = &s->mediaNetworkStats;
    IHS_MediaNetworkWindow current = IHS_MediaNetworkStatsCurrent(stats);
    out->nextFrameId = stats->next;
    out->frameSpan = stats->total;
    out->windowFrameSpan = stats->batch;
    out->windowReceived = (uint32_t)stats->count;
    out->windows = (uint32_t)stats->windowCount;
    out->windowLossPercent = current.loss;
    out->windowMeanNetworkMs = current.mean;
    out->windowDeviationMs = current.deviation;
    out->lossPercent = stats->average.loss;
    out->meanNetworkMs = stats->average.mean;
    out->deviationMs = stats->average.deviation;
    out->result = IHS_MediaNetworkStatsResult(stats->average.mean, stats->average.deviation,
                                              stats->average.loss);
    out->valid = stats->valid;
    out->finalized = stats->finalized;
    IHS_BaseUnlock(&s->base);
}

/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "clock.h"
#include <stdlib.h>
#include <string.h>

struct IHS_ClockSample {
    uint32_t timestamp;
    int32_t offset;
    IHS_ClockSample *next;
};

void IHS_StreamClockInit(IHS_StreamClock *clock) {
    memset(clock, 0, sizeof(*clock));
    clock->lock = IHS_MutexCreate();
}
void IHS_StreamClockDeinit(IHS_StreamClock *clock) {
    while (clock->head) {
        IHS_ClockSample *next = clock->head->next;
        free(clock->head);
        clock->head = next;
    }
    while (clock->rttHead) {
        IHS_ClockSample *next = clock->rttHead->next;
        free(clock->rttHead); clock->rttHead = next;
    }
    IHS_MutexDestroy(clock->lock);
}
void IHS_StreamClockFeedback(IHS_StreamClock *clock, uint32_t echo,
                             uint32_t peer, uint32_t now) {
    IHS_MutexLock(clock->lock);
    /* ProcessTimestamp 0x7f9a5c..68 tolerates two 65-tick update intervals. */
    if (clock->haveEcho && (int32_t) (echo - clock->lastEcho) < -130) goto done;
    clock->haveEcho = true;
    clock->lastEcho = echo;
    int32_t rtt = (int32_t) (now - echo);
    uint32_t nonnegativeRTT = rtt > 0 ? (uint32_t) rtt : 0;
    IHS_ClockSample *ping = malloc(sizeof(*ping));
    if (ping) {
        *ping = (IHS_ClockSample){.timestamp=now, .offset=(int32_t)nonnegativeRTT};
        while (clock->rttHead && (uint32_t)(now-clock->rttHead->timestamp) > 3u*65536) {
            IHS_ClockSample *old = clock->rttHead;
            clock->rttSum -= old->offset; clock->rttSamples--;
            clock->rttHead = old->next; free(old);
        }
        if (!clock->rttHead) clock->rttTail = NULL;
        if (clock->rttTail) clock->rttTail->next = ping;
        else clock->rttHead = ping;
        clock->rttTail = ping;
        clock->rttSum += nonnegativeRTT; clock->rttSamples++;
    }
    clock->smoothedRTT = clock->smoothedRTT ?
        (uint32_t)(((uint64_t)clock->smoothedRTT + nonnegativeRTT) / 2) : nonnegativeRTT;
    if (rtt > 0 && (!clock->minRTT || nonnegativeRTT < clock->minRTT)) {
        clock->minRTT = nonnegativeRTT;
    } else if (clock->minRTT && (int32_t) (now - clock->minRTTUpdate) > 0) {
        clock->minRTT += 65;
        clock->minRTTUpdate = now + 65536 / 4;
    }
    /* Near-minimum RTT samples only (0x7f9b08..34), ten-second mean
     * (0x7fc958, 0x7fc9c8..d8, 0x7fcac8..cc). */
    if (nonnegativeRTT > clock->minRTT + 65) goto done;
    IHS_ClockSample *sample = malloc(sizeof(*sample));
    if (!sample) goto done; /* Retain the last valid estimate on allocation failure. */
    sample->timestamp = now;
    sample->offset = (int32_t) (peer - echo - (uint32_t)(rtt / 2));
    sample->next = NULL;
    while (clock->head && (uint32_t) (now - clock->head->timestamp) > 10u * 65536) {
        IHS_ClockSample *old = clock->head;
        clock->offsetSum -= old->offset;
        clock->samples--;
        clock->head = old->next;
        free(old);
    }
    if (!clock->head) clock->tail = NULL;
    if (clock->tail) clock->tail->next = sample;
    else clock->head = sample;
    clock->tail = sample;
    clock->offsetSum += sample->offset;
    clock->samples++;
    clock->offset = (int32_t) (clock->offsetSum / clock->samples);
done:
    IHS_MutexUnlock(clock->lock);
}
int32_t IHS_StreamClockOffset(IHS_StreamClock *clock) {
    IHS_MutexLock(clock->lock);
    int32_t offset = clock->offset;
    IHS_MutexUnlock(clock->lock);
    return offset;
}

uint32_t IHS_StreamClockRetryTicks(IHS_StreamClock *clock) {
    IHS_MutexLock(clock->lock);
    uint32_t ticks = clock->smoothedRTT;
    IHS_MutexUnlock(clock->lock);
    if (ticks < 65) ticks = 65;
    if (ticks > 3276) ticks = 3276;
    return ticks + ticks / 4; /* SendReliablePackets 0x7f8b04..2c */
}
uint32_t IHS_StreamClockNackAgeTicks(IHS_StreamClock *clock) {
    IHS_MutexLock(clock->lock);
    float meanMs = clock->rttSamples ?
        (float)clock->rttSum / clock->rttSamples * (1000.0f / 65536.0f) : 0;
    IHS_MutexUnlock(clock->lock);
    uint32_t ageMs = (uint32_t)((meanMs + 2.0f) * 0.75f);
    uint32_t ticks = (uint32_t)((uint64_t)ageMs * 65536 / 1000);
    return ticks < 3276 ? ticks : 3276;
}

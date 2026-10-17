/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ihs_thread.h"
#include "network_stats.h"

typedef struct IHS_ClockSample IHS_ClockSample;
typedef struct IHS_StreamClock {
    IHS_Mutex *lock;
    IHS_ClockSample *head, *tail;
    IHS_NetworkPingStats ping;
    uint32_t smoothedRTT;
    int64_t offsetSum;
    uint32_t samples;
    uint32_t lastEcho, minRTT, minRTTUpdate;
    int32_t offset;
    bool haveEcho;
} IHS_StreamClock;

void IHS_StreamClockInit(IHS_StreamClock *clock);
void IHS_StreamClockDeinit(IHS_StreamClock *clock);
/* All values are 16.16 seconds. Offset maps local timestamps into peer time. */
void IHS_StreamClockFeedback(IHS_StreamClock *clock, uint32_t echoedLocal,
                             uint32_t peerSend, uint32_t localReceive);
int32_t IHS_StreamClockOffset(IHS_StreamClock *clock);
float IHS_StreamClockRTTMilliseconds(IHS_StreamClock *clock);

uint32_t IHS_StreamClockRetryTicks(IHS_StreamClock *clock);
uint32_t IHS_StreamClockNackAgeTicks(IHS_StreamClock *clock);

uint32_t IHS_StreamClockReceiveDelayTicks(IHS_StreamClock *, uint32_t budget);
uint32_t IHS_StreamClockMediaDelayTicks(IHS_StreamClock *clock);
uint32_t IHS_StreamClockFeedbackTicks(IHS_StreamClock *clock);

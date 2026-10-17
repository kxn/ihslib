/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Official CStreamSampleQueue/collectors, protocol reference §56. Zero-init
 * before use. All calls require the owner's lock; these containers do not
 * create worker threads. Release queues during owner destruction. */
typedef struct IHS_NetworkSample {
    uint32_t timestamp, values[4];
} IHS_NetworkSample;
typedef struct IHS_NetworkSampleQueue {
    IHS_NetworkSample *data;
    size_t head, count, capacity;
} IHS_NetworkSampleQueue;
typedef struct IHS_NetworkBitrateStats {
    IHS_NetworkSampleQueue queue;
    uint64_t bytes;
    float bitrate;
} IHS_NetworkBitrateStats;
typedef struct IHS_NetworkPacketStats {
    IHS_NetworkSampleQueue queue;
    uint32_t total, lost;
} IHS_NetworkPacketStats;
typedef struct IHS_NetworkPingStats {
    IHS_NetworkSampleQueue queue;
    uint32_t sum;
    float milliseconds;
} IHS_NetworkPingStats;
typedef struct IHS_NetworkVideoStats {
    IHS_NetworkSampleQueue queue;
    uint16_t frame;
    uint32_t firstSent, lastReceived, frameBytes, totals[4];
    float bitrate, link;
} IHS_NetworkVideoStats;

void IHS_NetworkSampleQueueDestroy(IHS_NetworkSampleQueue *);
bool IHS_NetworkBitrateRecord(IHS_NetworkBitrateStats *, uint32_t timestamp, int32_t bytes);
bool IHS_NetworkPacketRecord(IHS_NetworkPacketStats *, uint32_t timestamp, int32_t count, bool lost);
bool IHS_NetworkPingRecord(IHS_NetworkPingStats *, uint32_t ticks, uint32_t timestamp);
float IHS_NetworkPingMeanMilliseconds(uint32_t sum, uint32_t count);
bool IHS_NetworkVideoRecord(IHS_NetworkVideoStats *, uint32_t frameSend,
                            uint32_t receiveBegin, uint32_t receiveEnd, uint16_t frame, int32_t bytes);

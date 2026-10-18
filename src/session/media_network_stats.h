/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Official CNetworkStats default recording path, protocol reference §117.
 * The session Base lock owns this state. These are frame statistics, separate
 * from transport packet loss. No worker, file IO or adaptive FEC is involved. */
typedef struct IHS_MediaNetworkWindow {
    float loss, mean, deviation;
} IHS_MediaNetworkWindow;
typedef struct IHS_MediaNetworkStats {
    uint16_t next;
    uint32_t total, batch, latencies[600];
    size_t count, windowCount, capacity;
    IHS_MediaNetworkWindow *windows;
    IHS_MediaNetworkWindow average;
    bool valid, finalized;
} IHS_MediaNetworkStats;
void IHS_MediaNetworkStatsInit(IHS_MediaNetworkStats *);
void IHS_MediaNetworkStatsDestroy(IHS_MediaNetworkStats *);
void IHS_MediaNetworkStatsStart(IHS_MediaNetworkStats *);
bool IHS_MediaNetworkStatsRecord(IHS_MediaNetworkStats *, uint16_t frame, uint32_t sent,
                                 uint32_t received);
bool IHS_MediaNetworkStatsFold(IHS_MediaNetworkStats *);
bool IHS_MediaNetworkStatsStop(IHS_MediaNetworkStats *);
IHS_MediaNetworkWindow IHS_MediaNetworkStatsCurrent(const IHS_MediaNetworkStats *);
unsigned IHS_MediaNetworkStatsResult(float mean, float deviation, float loss);

/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once
#include "common.h"
#include <stddef.h>
typedef struct IHS_Session IHS_Session;
typedef enum IHS_NetworkTestMode {
    IHS_NetworkTestFixed = 1,
    IHS_NetworkTestBandwidth = 2,
} IHS_NetworkTestMode;
typedef enum IHS_NetworkTestStatus {
    IHS_NetworkTestIdle = 0,
    IHS_NetworkTestRunning = 1,
    IHS_NetworkTestComplete = 2,
    IHS_NetworkTestCancelled = 3,
    IHS_NetworkTestFailed = 4,
} IHS_NetworkTestStatus;
typedef struct IHS_NetworkTestSample {
    uint32_t targetKbps, networkTicks;
} IHS_NetworkTestSample;
typedef struct IHS_NetworkTestResult {
    IHS_NetworkTestStatus status;
    IHS_NetworkTestMode mode;
    uint32_t frameSpan, targetKbps, previousTargetKbps, lastDataTick;
    size_t samples;
    float progressPercent, lossPercent, meanNetworkMs, deviationMs;
    unsigned result;
    bool valid;
} IHS_NetworkTestResult;
/** Select the official dedicated network-test protocol before Connect. Target
 * is explicit (positive int32 kbps); request is 600 frames/60fps/burst0. Use a fresh
 * ordinary Session for subsequent streaming.
 * The caller supplies an authorized network-test endpoint/key; this API does
 * not acquire the discovery ticket.
 * A 16- or 32-byte session key is required. No decoder, game start/stop, preset/FEC
 * change or UI is implied. Cancel with IHS_SessionDisconnect, then join/destroy.
 * Session lifecycle callbacks still run; the stream connected callback does not. */
bool IHS_SessionConfigureNetworkTest(IHS_Session *, IHS_NetworkTestMode, uint32_t targetKbps);
/** Bounded snapshot; final metrics belong to status Complete. Test loss uses
 * span/600, separately from default media batch loss (protocol reference §119). */
void IHS_SessionGetNetworkTestResult(IHS_Session *, IHS_NetworkTestResult *);
/** Copy at most 256 optional full samples per call, starting at offset.
 * The caller owns output; result.samples gives the available count. */
size_t IHS_SessionReadNetworkTestSamples(IHS_Session *, size_t offset,
                                         IHS_NetworkTestSample *output, size_t capacity);

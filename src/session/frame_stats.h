/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs_thread.h"
#include "ihslib/session.h"
#include "protobuf/remoteplay.pb-c.h"

/**
 * Per-frame timestamp + result record. Mirrors Steam's CFastFrameStats:
 * a fixed-size slot in a 240-entry ring keyed by frameId % 240. Each frame
 * carries up to 19 event timestamps (one per EStreamFrameEvent), the size of
 * the frame data, and the final IHS_VideoFrameResult.
 *
 * Timestamps are in IHS_SessionPacketTimestamp units (16.16 fixed-point seconds).
 *
 * eventMask is local collection bookkeeping. Wire Save uses raw zero as absence;
 * the original setter stores timestamps 0/1 as 1 and retains first Send/DecodeBegin.
 */
#define IHS_FRAME_STATS_EVENT_COUNT 19

typedef struct IHS_FrameStatsSlot {
    uint16_t frameId;
    uint32_t eventMask; /* bit i set iff events[i] is valid */
    uint32_t events[IHS_FRAME_STATS_EVENT_COUNT];
    uint32_t frameSize; /* bytes; 0 if unknown */
    uint16_t inputMark; /* 0 if not bound to an input mark */
    IHS_VideoFrameResult result;
    float frameStartDelta, frameDisplayDelta; /* original CFastFrameStats ms */
    bool inUse, tracked;
    bool complete; /* event 18 (Complete) recorded */
} IHS_FrameStatsSlot;

/**
 * Float32 sample vectors, matching original AddSample/Calculate. The report
 * snapshot owns moved vectors; resetting one accumulator never frees another's.
 */
#define IHS_FRAME_STATS_ACCUM_SLOTS 19

typedef struct IHS_FrameStatsAccumSlot {
    uint32_t count;
    float sum;
    float *samples;
    uint32_t capacity;
} IHS_FrameStatsAccumSlot;

typedef struct IHS_FrameStatsAccumulator {
    IHS_FrameStatsAccumSlot slots[IHS_FRAME_STATS_ACCUM_SLOTS];
} IHS_FrameStatsAccumulator;

/**
 * Video reporting ring + independent audio RX ring + shared input marks.
 * One aggregator per session. All accessors take
 * `lock` internally; callers must not hold any other session lock when
 * invoking them to avoid lock-order inversion.
 */
#define IHS_FRAME_STATS_RING_SIZE 240
#define IHS_FRAME_REPORT_CAPACITY IHS_FRAME_STATS_RING_SIZE

typedef struct IHS_FrameStatsReport {
    uint64_t serial;
    IHS_FrameStatsAccumulator accumulator;
    IHS_FrameStatsSlot frames[IHS_FRAME_REPORT_CAPACITY];
    size_t count;
    uint16_t latestFrameId;
    bool fullReporting;
} IHS_FrameStatsReport;

/* Allocate outside the report hot path, one per report consumer. */
typedef struct IHS_FrameStatsMessage {
    CFrameStatsListMsg message;
    CFrameStats frames[IHS_FRAME_REPORT_CAPACITY];
    CFrameStats *framePointers[IHS_FRAME_REPORT_CAPACITY];
    CFrameEvent events[IHS_FRAME_REPORT_CAPACITY * IHS_FRAME_STATS_EVENT_COUNT];
    CFrameEvent *eventPointers[IHS_FRAME_REPORT_CAPACITY * IHS_FRAME_STATS_EVENT_COUNT];
    CFrameStatAccumulatedValue accumulated[IHS_FRAME_STATS_ACCUM_SLOTS];
    CFrameStatAccumulatedValue *accumulatedPointers[IHS_FRAME_STATS_ACCUM_SLOTS];
} IHS_FrameStatsMessage;
void IHS_FrameStatsEncodeReport(const IHS_FrameStatsReport *, int32_t offset,
                                 IHS_FrameStatsMessage *);
bool IHS_SessionSendFrameEvents(IHS_Session *, IHS_FrameStatsMessage *);

typedef struct IHS_FrameStatsAggregator {
    IHS_Mutex *lock;
    IHS_Mutex *collectorLock;
    struct IHS_TrackedFrame *trackedScratch;
    /* Session/RX-owned consumer scratch; allocated with the aggregator before
     * workers start, released only after all workers have joined. */
    IHS_FrameStatsMessage messageScratch;
    IHS_FrameStatsSlot ring[IHS_FRAME_STATS_RING_SIZE]; /* Video: existing reporting/tracker. */
    IHS_FrameStatsSlot audioRing[IHS_FRAME_STATS_RING_SIZE]; /* Independent RX slots; shared marks. */
    IHS_FrameStatsAccumulator accumulator;
    uint16_t lastSentFrameId;
    uint16_t lastDisplayedFrameId;
    uint32_t previousFrameStart, previousDisplay;
    uint16_t lastInputMark;
    struct {
        uint16_t id;
        uint32_t begin, sent;
    } inputMarks[1024];
    bool fullReporting;          /* mirrors CStreamClient::sendFullFrameStats */
    /* Latest collector output, consumed once per nonempty report. Values are
     * already in the original AddNetworkSample units, not packet timestamps. */
    float networkSample[5];

    IHS_FrameStatsReport pendingReport;
    uint64_t reportSerial;
    bool reportPending;
    uint16_t lastQueuedFrameId; /* queue acceptance, separate from ring drain */
    uint64_t closedUnsentFrames;
} IHS_FrameStatsAggregator;

IHS_FrameStatsAggregator *IHS_FrameStatsAggregatorCreate(void);

void IHS_FrameStatsAggregatorDestroy(IHS_FrameStatsAggregator *agg);

void IHS_FrameStatsAggregatorSetFullReporting(IHS_FrameStatsAggregator *agg, bool enabled);

size_t IHS_FrameStatsEncodeEvents(const IHS_FrameStatsSlot *slot, int32_t clockOffset,
                                  CFrameEvent *events, CFrameEvent **pointers);
void IHS_FrameStatsEncodeFrame(const IHS_FrameStatsSlot *, int32_t clockOffset,
                                CFrameStats *, CFrameEvent *, CFrameEvent **);

/**
 * Drain like IHS_FrameStatsAggregatorDrain, additionally copying each folded
 * slot (events still in absolute timestamp units) into `out` (up to max).
 * Returns the number of slots drained. */
size_t IHS_FrameStatsAggregatorDrainSlots(IHS_FrameStatsAggregator *agg, IHS_FrameStatsSlot *out,
                                          size_t max, uint16_t *outLatestFrameId);

/**
 * Record one of the four app-driven events (DecodeBegin/DecodeEnd/UploadBegin/UploadEnd).
 * `timestamp` is in 16.16 fixed-point seconds; `0` means "use IHS_SessionTimestampNow".
 *
 * Only Receive assigns a slot to a different frame ID. Events for a missing
 * or overwritten ID are ignored (official RecordFrameEvent 0x7ac35c).
 */
void IHS_FrameStatsRecordStage(IHS_FrameStatsAggregator *agg, uint16_t frameId,
                               IHS_VideoFrameStage stage, uint32_t timestamp);

/**
 * Finish a matching Pending slot, stamping event 18 once. Previous frame-start
 * and display timestamps follow official RecordFrameComplete 0x7ac3f8.
 */
void IHS_FrameStatsRecordCompleteAt(IHS_FrameStatsAggregator *, uint16_t, IHS_VideoFrameResult, uint32_t);
void IHS_FrameStatsRecordComplete(IHS_FrameStatsAggregator *agg, uint16_t frameId,
                                  IHS_VideoFrameResult result);

/**
 * Internal hook for the video-receive path. Seeds events 5 (Start), 12 (Send),
 * 13 (Recv) on first observation of a frame; subsequent partial-receives only
 * refresh event 13. frameSize is the data payload size before video-header parsing.
 */
void IHS_FrameStatsRecordReceived(IHS_FrameStatsAggregator *agg, uint16_t frameId,
                                  uint32_t senderFrameTimestamp, uint32_t senderSendTimestamp,
                                  uint32_t recvTimestamp, uint32_t frameSize, uint16_t inputMark);
/* Original RecordFrameReceived selects one ring by media type, then consumes
 * a matching input mark from the single client-wide mark table (§74). */
typedef enum IHS_FrameStatsMedia {
    IHS_FrameStatsMediaAudio = 0,
    IHS_FrameStatsMediaVideo = 1,
} IHS_FrameStatsMedia;
void IHS_FrameStatsRecordMediaReceived(IHS_FrameStatsAggregator *, IHS_FrameStatsMedia,
                                       uint16_t frameId, uint32_t frameTimestamp,
                                       uint32_t sendTimestamp, uint32_t receiveTimestamp,
                                       uint32_t frameSize, uint16_t inputMark,
                                       uint32_t inputReceived);
void IHS_FrameStatsRecordReceivedInput(IHS_FrameStatsAggregator *, uint16_t frameId,
                                       uint32_t frameTimestamp, uint32_t sendTimestamp,
                                       uint32_t receiveTimestamp, uint32_t frameSize,
                                       uint16_t inputMark, uint32_t inputReceived);
/* Official CreateInputMark primitive. A caller must supply the input timestamp;
 * creating and sending marks from input producers is a separate contract. */
uint16_t IHS_FrameStatsCreateInputMark(IHS_FrameStatsAggregator *, uint32_t begin);

/**
 * Drain the ring for everything between lastSentFrameId and lastDisplayedFrameId
 * (exclusive→inclusive), fold completed slots into the accumulator, and reset
 * them. Returns the count of slots folded; the accumulator and the returned
 * `latestFrameId` are the inputs for one CFrameStatsListMsg flush.
 *
 * Caller is expected to call IHS_FrameStatsAccumulatorReset on the accumulator
 * AFTER serializing the flush packet.
 */
size_t IHS_FrameStatsAggregatorDrain(IHS_FrameStatsAggregator *agg, uint16_t *outLatestFrameId);

void IHS_FrameStatsAccumulatorReset(IHS_FrameStatsAccumulator *accum);
bool IHS_FrameStatsAccumulatorAdd(IHS_FrameStatsAccumulator *, unsigned stat, float value);
size_t IHS_FrameStatsAccumulatorEncode(const IHS_FrameStatsAccumulator *,
                                       CFrameStatAccumulatedValue *, CFrameStatAccumulatedValue **);
void IHS_FrameStatsSetNetworkSample(IHS_FrameStatsAggregator *, const float values[5]);

/* One report consumer, excluded from destruction. Begin retires protocol data;
 * Finish releases the immutable snapshot after ONE send attempt, including
 * rejection. Receive/settle continue accumulating in the next interval. */
const IHS_FrameStatsReport *IHS_FrameStatsReportBegin(IHS_FrameStatsAggregator *);
bool IHS_FrameStatsReportFinish(IHS_FrameStatsAggregator *, uint64_t serial, bool accepted);
/* Exclude the report consumer first (stop its timer); never replay an old
 * channel's failed report from a replacement channel. */
void IHS_FrameStatsReportAbandon(IHS_FrameStatsAggregator *);

struct IHS_FrameTracker;
void IHS_FrameStatsCollectTracked(IHS_FrameStatsAggregator *, struct IHS_FrameTracker *);
void IHS_FrameStatsSettleTracked(IHS_FrameStatsAggregator *, struct IHS_FrameTracker *,
                                 uint64_t nowUs);

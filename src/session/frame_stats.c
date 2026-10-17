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

#include "frame_stats.h"
#include "frame_tracker.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "session_pri.h"

/* EStreamFrameEvent ids that drive the accumulator. Mirrors Steam's
 * AddFrameSample event references — used as indices into events[]. */
enum {
    EV_INPUT_START = 0,
    EV_INPUT_SEND = 1,
    EV_INPUT_RECV = 2,
    EV_FRAME_START = 5,
    EV_CAPTURE_BEGIN = 6,
    EV_CAPTURE_END = 7,
    EV_CONVERT_BEGIN = 8,
    EV_CONVERT_END = 9,
    EV_ENCODE_BEGIN = 10,
    EV_ENCODE_END = 11,
    EV_FRAME_SEND = 12,
    EV_FRAME_RECV = 13,
    EV_DECODE_BEGIN = 14,
    EV_DECODE_END = 15,
    EV_UPLOAD_BEGIN = 16,
    EV_UPLOAD_END = 17,
    EV_COMPLETE = 18,
};

/* All events are 16.16 fixed-point seconds (IHS_SessionPacketTimestamp,
 * matching GetStreamTimestamp per STEAMLINK_PROTOCOL_RE.md §9c.1). Deltas
 * between two such stamps convert to ms by ×1000/65536. Wrapped subtraction
 * keeps timestamps that crossed the uint32 boundary correct (16.16 wraps
 * every ~18.2 hours; the cost is one cast). */
static float ticks_to_ms(uint32_t earlier, uint32_t later) {
    uint32_t delta = later - earlier;
    bool negative = (int32_t)delta < 0;
    uint32_t magnitude = negative ? 0u - delta : delta;
    float whole = (float)((magnitude >> 16) * 1000u);
    float fraction = (float)((magnitude & 65535u) * 1000u) * (1.0f / 65536.0f);
    float value = whole + fraction;
    return negative ? -value : value;
}

IHS_FrameStatsAggregator *IHS_FrameStatsAggregatorCreate(void) {
    IHS_FrameStatsAggregator *agg = calloc(1, sizeof(*agg));
    if (agg == NULL) {
        return NULL;
    }
    agg->lock = IHS_MutexCreate();
    if (agg->lock == NULL) {
        free(agg);
        return NULL;
    }
    return agg;
}

void IHS_FrameStatsAggregatorDestroy(IHS_FrameStatsAggregator *agg) {
    if (agg == NULL) {
        return;
    }
    IHS_MutexDestroy(agg->lock);
    free(agg);
}

size_t IHS_FrameStatsEncodeEvents(const IHS_FrameStatsSlot *slot, int32_t clockOffset,
                                  CFrameEvent *events, CFrameEvent **pointers) {
    size_t count = 0;
    uint32_t previous = 0;
    for (unsigned e = 0; e < IHS_FRAME_STATS_EVENT_COUNT; e++) {
        if ((e >= 2 && e <= 12) || !slot->events[e])
            continue;
        cframe_event__init(&events[count]);
        events[count].event_id = (EStreamFrameEvent)e;
        events[count].timestamp =
            count ? slot->events[e] - previous : slot->events[e] + (uint32_t)clockOffset;
        previous = slot->events[e];
        pointers[count] = &events[count];
        count++;
    }
    return count;
}

void IHS_FrameStatsEncodeFrame(const IHS_FrameStatsSlot *slot, int32_t clockOffset,
                                CFrameStats *row, CFrameEvent *events, CFrameEvent **pointers) {
    cframe_stats__init(row);
    row->frame_id = slot->frameId;
    row->result = (EStreamFrameResult)slot->result;
    row->events = pointers;
    row->n_events = IHS_FrameStatsEncodeEvents(slot, clockOffset, events, pointers);
    if (slot->inputMark) {
        row->has_input_mark = 1;
        row->input_mark = slot->inputMark;
    }
    if (slot->frameStartDelta != 0) {
        row->has_frame_start_delta = 1;
        row->frame_start_delta = slot->frameStartDelta;
    }
    if (slot->frameDisplayDelta != 0) {
        row->has_frame_display_delta = 1;
        row->frame_display_delta = slot->frameDisplayDelta;
    }
    /* Official Save(..., true, ...) excludes frame_size and server events. */
}

void IHS_FrameStatsAggregatorSetFullReporting(IHS_FrameStatsAggregator *agg, bool enabled) {
    IHS_MutexLock(agg->lock);
    agg->fullReporting = enabled;
    IHS_MutexUnlock(agg->lock);
}

static IHS_FrameStatsSlot *find_slot(IHS_FrameStatsAggregator *agg, uint16_t frameId) {
    IHS_FrameStatsSlot *slot = &agg->ring[frameId % IHS_FRAME_STATS_RING_SIZE];
    return slot->frameId == frameId ? slot : NULL;
}

static void set_event(IHS_FrameStatsSlot *slot, uint32_t eventId, uint32_t timestamp) {
    assert(eventId < IHS_FRAME_STATS_EVENT_COUNT);
    /* Original SetEventTimestamp: Send and DecodeBegin keep their first value;
     * zero is reserved for absence, so timestamps 0/1 are both stored as 1. */
    if ((eventId == EV_FRAME_SEND || eventId == EV_DECODE_BEGIN) && slot->events[eventId])
        return;
    slot->events[eventId] = timestamp > 1 ? timestamp : 1;
    slot->eventMask |= (1u << eventId);
}

static bool has_event(const IHS_FrameStatsSlot *slot, uint32_t eventId) {
    return (slot->eventMask & (1u << eventId)) != 0;
}

void IHS_FrameStatsRecordStage(IHS_FrameStatsAggregator *agg, uint16_t frameId,
                               IHS_VideoFrameStage stage, uint32_t timestamp) {
    /* IHS_VideoFrameStage values are deliberately equal to EStreamFrameEvent ids. */
    uint32_t ev = (uint32_t)stage;
    if (ev >= IHS_FRAME_STATS_EVENT_COUNT) {
        return;
    }
    IHS_MutexLock(agg->lock);
    IHS_FrameStatsSlot *slot = find_slot(agg, frameId);
    if (slot)
        set_event(slot, ev, timestamp ? timestamp : IHS_SessionPacketTimestamp());
    IHS_MutexUnlock(agg->lock);
}

static void complete_slot(IHS_FrameStatsAggregator *agg, IHS_FrameStatsSlot *slot,
                          IHS_VideoFrameResult result, uint32_t now) {
    if (!slot || slot->result != IHS_VideoFrameResultPending)
        return;
    set_event(slot, EV_COMPLETE, now);
    now = slot->events[EV_COMPLETE];
    if (agg->previousFrameStart && slot->events[EV_FRAME_START]) {
        slot->frameStartDelta = ticks_to_ms(agg->previousFrameStart, slot->events[EV_FRAME_START]);
        if (!(slot->frameStartDelta > 0))
            slot->frameStartDelta = 0.001f;
    }
    if (slot->events[EV_FRAME_START])
        agg->previousFrameStart = slot->events[EV_FRAME_START];
    if (agg->previousDisplay && now) {
        slot->frameDisplayDelta = ticks_to_ms(agg->previousDisplay, now);
        if (!(slot->frameDisplayDelta > 0))
            slot->frameDisplayDelta = 0.001f;
    }
    slot->result = result;
    slot->complete = result != IHS_VideoFrameResultPending;
    if (result == IHS_VideoFrameResultDisplayed) {
        agg->lastDisplayedFrameId = slot->frameId;
        agg->previousDisplay = now;
    }
}

void IHS_FrameStatsRecordCompleteAt(IHS_FrameStatsAggregator *agg, uint16_t frameId,
                                    IHS_VideoFrameResult result, uint32_t now) {
    IHS_MutexLock(agg->lock);
    complete_slot(agg, find_slot(agg, frameId), result, now);
    IHS_MutexUnlock(agg->lock);
}
void IHS_FrameStatsRecordComplete(IHS_FrameStatsAggregator *agg, uint16_t frameId,
                                  IHS_VideoFrameResult result) {
    IHS_FrameStatsRecordCompleteAt(agg, frameId, result, IHS_SessionPacketTimestamp());
}

void IHS_FrameStatsRecordReceived(IHS_FrameStatsAggregator *agg, uint16_t frameId,
                                  uint32_t senderFrameTimestamp, uint32_t senderSendTimestamp,
                                  uint32_t recvTimestamp, uint32_t frameSize, uint16_t inputMark) {
    IHS_FrameStatsRecordReceivedInput(agg, frameId, senderFrameTimestamp, senderSendTimestamp,
                                     recvTimestamp, frameSize, inputMark, 0);
}

uint16_t IHS_FrameStatsCreateInputMark(IHS_FrameStatsAggregator *agg, uint32_t begin) {
    IHS_MutexLock(agg->lock);
    uint16_t mark = agg->lastInputMark == UINT16_MAX ? 1 : (uint16_t)(agg->lastInputMark + 1);
    agg->lastInputMark = mark;
    unsigned index = mark & 1023u;
    agg->inputMarks[index].id = mark;
    agg->inputMarks[index].begin = begin;
    agg->inputMarks[index].sent = IHS_SessionPacketTimestamp();
    IHS_MutexUnlock(agg->lock);
    return mark;
}

void IHS_FrameStatsRecordReceivedInput(IHS_FrameStatsAggregator *agg, uint16_t frameId,
                                       uint32_t senderFrameTimestamp, uint32_t senderSendTimestamp,
                                       uint32_t recvTimestamp, uint32_t frameSize,
                                       uint16_t inputMark, uint32_t inputReceived) {
    IHS_MutexLock(agg->lock);
    IHS_FrameStatsSlot *slot = &agg->ring[frameId % IHS_FRAME_STATS_RING_SIZE];
    if (slot->frameId != frameId) {
        memset(slot, 0, sizeof(*slot));
        slot->frameId = frameId;
        slot->inUse = true;
        slot->frameSize = frameSize;
        /* Original shifts frame-start with send when receive precedes send. */
        if ((int32_t)(recvTimestamp - senderSendTimestamp) < 0) {
            if (inputReceived)
                inputReceived -= senderSendTimestamp - recvTimestamp;
            senderFrameTimestamp -= senderSendTimestamp - recvTimestamp;
            senderSendTimestamp = recvTimestamp;
        }
        set_event(slot, EV_FRAME_START, senderFrameTimestamp);
        set_event(slot, EV_FRAME_SEND, senderSendTimestamp);
        set_event(slot, EV_FRAME_RECV, recvTimestamp);
        /* The original indexes with the CURRENT mark, not the received mark.
         * A matched entry is consumed once. Unknown marks are not echoed. */
        unsigned index = agg->lastInputMark & 1023u;
        if (inputMark && (int16_t)(uint16_t)(inputMark - agg->lastInputMark) <= 0 &&
            agg->inputMarks[index].id == inputMark) {
            slot->inputMark = inputMark;
            set_event(slot, EV_INPUT_START, agg->inputMarks[index].begin);
            set_event(slot, EV_INPUT_SEND, agg->inputMarks[index].sent);
            set_event(slot, EV_INPUT_RECV, inputReceived);
            agg->inputMarks[index].id = 0;
        }
    } else {
        /* Additional partial — only refresh recv timestamp. */
        set_event(slot, EV_FRAME_RECV, recvTimestamp);
    }
    IHS_MutexUnlock(agg->lock);
}

static void accum_add(IHS_FrameStatsAccumulator *accum, int statId, double value) {
    if (statId < 0 || statId >= IHS_FRAME_STATS_ACCUM_SLOTS) {
        return;
    }
    if (accum->slots[statId].count == INT32_MAX)
        return;
    accum->slots[statId].count++;
    accum->slots[statId].sum += value;
    accum->slots[statId].sumSquares += value * value;
}

void IHS_FrameStatsAccumulatorReset(IHS_FrameStatsAccumulator *accum) {
    memset(accum, 0, sizeof(*accum));
}

/* Fold one completed slot into the accumulator. Slot lock held by caller. */
static void fold_slot(IHS_FrameStatsAggregator *agg, IHS_FrameStatsSlot *slot) {
    IHS_FrameStatsAccumulator *accum = &agg->accumulator;

    /* Inter-frame interval (stat 10) — needs the previous frame's event 18. */
    if (has_event(slot, EV_COMPLETE) && agg->lastFrameTimestamp != 0) {
        accum_add(accum, 10 /* FrameDurationMS */,
                  ticks_to_ms(agg->lastFrameTimestamp, slot->events[EV_COMPLETE]));
    }
    if (has_event(slot, EV_COMPLETE)) {
        agg->lastFrameTimestamp = slot->events[EV_COMPLETE];
    }

    /* Network duration (stat 6): event 12 → 13. */
    if (has_event(slot, EV_FRAME_SEND) && has_event(slot, EV_FRAME_RECV)) {
        accum_add(accum, 6, ticks_to_ms(slot->events[EV_FRAME_SEND], slot->events[EV_FRAME_RECV]));
    }
    /* Decode duration (stat 7): event 14 → 15. */
    if (has_event(slot, EV_DECODE_BEGIN) && has_event(slot, EV_DECODE_END)) {
        accum_add(accum, 7,
                  ticks_to_ms(slot->events[EV_DECODE_BEGIN], slot->events[EV_DECODE_END]));
    }
    /* Display / upload duration (stat 8): event 16 → 17. */
    if (has_event(slot, EV_UPLOAD_BEGIN) && has_event(slot, EV_UPLOAD_END)) {
        accum_add(accum, 8,
                  ticks_to_ms(slot->events[EV_UPLOAD_BEGIN], slot->events[EV_UPLOAD_END]));
    }
    /* Client end-to-end (stat 9): event 13 → 18. */
    if (has_event(slot, EV_FRAME_RECV) && has_event(slot, EV_COMPLETE)) {
        accum_add(accum, 9, ticks_to_ms(slot->events[EV_FRAME_RECV], slot->events[EV_COMPLETE]));
    }

    /* Stats 0 (FPS), 11/12/13 (input latency variants), 14 (ping), 15/16/17
     * (bitrates), 18 (loss) require either an FPS sampler, the input-mark
     * pipeline, or network-stats hooks that ihslib does not yet own. They are
     * left out of this MVP; when those signals land they can fold in here. */
}

static size_t FrameStatsDrainLocked(IHS_FrameStatsAggregator *agg, IHS_FrameStatsSlot *out,
                                    size_t max, uint16_t *outLatestFrameId) {
    size_t folded = 0;

    uint16_t lastSent = agg->lastSentFrameId;
    uint16_t lastDisplayed = agg->lastDisplayedFrameId;
    while (lastSent != lastDisplayed) {
        uint32_t candidate = (uint32_t)lastSent + 1u;
        uint16_t frameId = (uint16_t)candidate;
        IHS_FrameStatsSlot *slot = &agg->ring[frameId % IHS_FRAME_STATS_RING_SIZE];
        /* Original 0x7a8398..0x7a83bc compares the widened increment with
         * the uint16 slot ID. At FFFF -> 0 it advances but does not emit 0. */
        if (slot->frameId == candidate) {
            if (slot->result == IHS_VideoFrameResultPending) {
                complete_slot(agg, slot, IHS_VideoFrameResultDroppedLate, IHS_SessionPacketTimestamp());
            }
            if (!agg->fullReporting)
                fold_slot(agg, slot);
            if (out != NULL && folded < max) {
                out[folded] = *slot;
            }
            memset(slot, 0, sizeof(*slot));
            folded++;
        }
        lastSent = frameId;
    }

    agg->lastSentFrameId = lastSent;
    if (outLatestFrameId != NULL) {
        *outLatestFrameId = lastSent;
    }
    return folded;
}

static size_t FrameStatsDrainCore(IHS_FrameStatsAggregator *agg, IHS_FrameStatsSlot *out,
                                  size_t max, uint16_t *latest) {
    IHS_MutexLock(agg->lock);
    size_t count = FrameStatsDrainLocked(agg, out, max, latest);
    IHS_MutexUnlock(agg->lock);
    return count;
}

const IHS_FrameStatsReport *IHS_FrameStatsReportBegin(IHS_FrameStatsAggregator *agg) {
    IHS_MutexLock(agg->lock);
    IHS_FrameStatsReport *report = &agg->pendingReport;
    if (!agg->reportPending && agg->reportSerial != UINT64_MAX) {
        if (agg->trackedMode) {
            report->count = agg->trackedDisplayed ? agg->trackedCount : 0;
            if (report->count) {
                memcpy(report->frames, agg->trackedFrames, report->count * sizeof(*report->frames));
                report->latestFrameId = agg->trackedLatest;
                agg->trackedCount = 0;
            }
        } else
            report->count = FrameStatsDrainLocked(agg, report->frames, IHS_FRAME_STATS_RING_SIZE,
                                                  &report->latestFrameId);
        if (report->count) {
            report->serial = ++agg->reportSerial;
            report->accumulator = agg->accumulator;
            report->fullReporting = agg->fullReporting;
            IHS_FrameStatsAccumulatorReset(&agg->accumulator);
            agg->reportPending = true;
        }
    }
    bool pending = agg->reportPending;
    IHS_MutexUnlock(agg->lock);
    return pending ? report : NULL;
}

bool IHS_FrameStatsReportCommit(IHS_FrameStatsAggregator *agg, uint64_t serial) {
    IHS_MutexLock(agg->lock);
    bool ok = agg->reportPending && agg->pendingReport.serial == serial;
    if (ok) {
        agg->lastQueuedFrameId = agg->pendingReport.latestFrameId;
        agg->reportPending = false;
    }
    IHS_MutexUnlock(agg->lock);
    return ok;
}

void IHS_FrameStatsReportAbandon(IHS_FrameStatsAggregator *agg) {
    IHS_MutexLock(agg->lock);
    if (agg->reportPending) {
        uint64_t count = agg->pendingReport.count;
        agg->closedUnsentFrames = UINT64_MAX - agg->closedUnsentFrames < count
                                      ? UINT64_MAX
                                      : agg->closedUnsentFrames + count;
        agg->reportPending = false;
    }
    IHS_MutexUnlock(agg->lock);
}

size_t IHS_FrameStatsAggregatorDrain(IHS_FrameStatsAggregator *agg, uint16_t *outLatestFrameId) {
    return FrameStatsDrainCore(agg, NULL, 0, outLatestFrameId);
}

size_t IHS_FrameStatsAggregatorDrainSlots(IHS_FrameStatsAggregator *agg, IHS_FrameStatsSlot *out,
                                          size_t max, uint16_t *outLatestFrameId) {
    return FrameStatsDrainCore(agg, out, max, outLatestFrameId);
}

static uint32_t us_to_ticks(uint64_t us) {
    return (uint32_t)(us / 1000000) * 65536u + (uint32_t)((us % 1000000) * 65536u / 1000000);
}
void IHS_FrameStatsBeginTrackedEpoch(IHS_FrameStatsAggregator *agg) {
    IHS_MutexLock(agg->lock);
    IHS_FrameStatsReportAbandon(agg);
    IHS_FrameStatsAccumulatorReset(&agg->accumulator);
    agg->trackedMode = true;
    agg->trackedDisplayed = false;
    agg->trackedCount = 0;
    agg->lastPresentationSerial = 0;
    agg->lastFrameTimestamp = 0;
    IHS_MutexUnlock(agg->lock);
}
void IHS_FrameStatsSettleTracked(IHS_FrameStatsAggregator *agg, IHS_FrameTracker *tracker,
                                 uint64_t nowUs) {
    /* Small bounded scratch; repeat bounded by the tracker capacity. No renderer
     * lock is acquired here, and no ticket is retained by a report snapshot. */
    IHS_TrackedFrame frames[8];
    IHS_MutexLock(agg->lock);
    for (unsigned batch = 0; batch < IHS_FRAME_TRACKER_CAPACITY / 8; ++batch) {
        size_t count = IHS_FrameTrackerSettle(tracker, nowUs, 2000000, frames, 8);
        for (size_t i = 0; i < count; ++i) {
            const IHS_TrackedFrame *f = &frames[i];
            IHS_FrameStatsSlot slot = {.frameId = f->identity.frameId,
                                       .inUse = true,
                                       .tracked = true,
                                       .complete = true,
                                       .frameSize = f->receive.frameSize,
                                       .inputMark = f->receive.inputMark,
                                       .result = f->outcome.result};
            set_event(&slot, EV_FRAME_START, f->receive.senderFrameTimestamp);
            set_event(&slot, EV_FRAME_SEND, f->receive.senderSendTimestamp);
            set_event(&slot, EV_FRAME_RECV, f->receive.receiveTimestamp);
            if (f->hasDecodeBegin)
                set_event(&slot, EV_DECODE_BEGIN, us_to_ticks(f->decodeBeginUs));
            if (f->hasDecodeEnd)
                set_event(&slot, EV_DECODE_END, us_to_ticks(f->decodeEndUs));
            if (f->outcome.hasUpload) {
                set_event(&slot, EV_UPLOAD_BEGIN, us_to_ticks(f->outcome.uploadBeginUs));
                set_event(&slot, EV_UPLOAD_END, us_to_ticks(f->outcome.uploadEndUs));
            }
            set_event(&slot, EV_COMPLETE, us_to_ticks(f->outcome.completionUs));
            agg->lastFrameTimestamp = 0; /* interval is supplied by presentation order */
            if (!agg->fullReporting)
                fold_slot(agg, &slot);
            agg->lastFrameTimestamp = 0;
            if (f->outcome.result == IHS_VideoFrameResultDisplayed) {
                if (!agg->fullReporting && f->outcome.hasPresentationInterval)
                    accum_add(&agg->accumulator, 10, f->outcome.presentationIntervalUs / 1000.0);
                if (f->outcome.presentationSerial > agg->lastPresentationSerial) {
                    agg->lastPresentationSerial = f->outcome.presentationSerial;
                    agg->trackedLatest = f->identity.frameId;
                    agg->trackedDisplayed = true;
                }
            }
            if (agg->trackedCount == IHS_FRAME_REPORT_CAPACITY) {
                /* Fixed detail window; accumulated samples remain accounted. */
                memmove(agg->trackedFrames, agg->trackedFrames + 1,
                        (IHS_FRAME_REPORT_CAPACITY - 1) * sizeof(*agg->trackedFrames));
                --agg->trackedCount;
            }
            agg->trackedFrames[agg->trackedCount++] = slot;
        }
        if (count < 8)
            break;
    }
    IHS_MutexUnlock(agg->lock);
}

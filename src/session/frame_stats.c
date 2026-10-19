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
#include "channels/ch_stats.h"

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
    agg->collectorLock = IHS_MutexCreate();
    agg->trackedScratch = calloc(IHS_FRAME_TRACKER_CAPACITY, sizeof(IHS_TrackedFrame));
    agg->trackedCapacity = IHS_FRAME_TRACKER_CAPACITY;
    if (!agg->lock || !agg->collectorLock || !agg->trackedScratch) {
        if (agg->lock) IHS_MutexDestroy(agg->lock);
        if (agg->collectorLock) IHS_MutexDestroy(agg->collectorLock);
        free(agg->trackedScratch);
        free(agg);
        return NULL;
    }
    return agg;
}

void IHS_FrameStatsAggregatorDestroy(IHS_FrameStatsAggregator *agg) {
    if (agg == NULL) {
        return;
    }
    IHS_FrameStatsAccumulatorReset(&agg->accumulator);
    IHS_FrameStatsAccumulatorReset(&agg->pendingReport.accumulator);
    free(agg->trackedScratch);
    IHS_MutexDestroy(agg->collectorLock);
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
    IHS_FrameStatsRecordMediaReceived(agg, IHS_FrameStatsMediaVideo, frameId, senderFrameTimestamp,
                                      senderSendTimestamp, recvTimestamp, frameSize, inputMark,
                                      inputReceived);
}

void IHS_FrameStatsRecordMediaReceived(IHS_FrameStatsAggregator *agg, IHS_FrameStatsMedia media,
                                       uint16_t frameId, uint32_t senderFrameTimestamp,
                                       uint32_t senderSendTimestamp, uint32_t recvTimestamp,
                                       uint32_t frameSize, uint16_t inputMark,
                                       uint32_t inputReceived) {
    if (media != IHS_FrameStatsMediaAudio && media != IHS_FrameStatsMediaVideo)
        return;
    IHS_MutexLock(agg->lock);
    IHS_FrameStatsSlot *ring = media == IHS_FrameStatsMediaAudio ? agg->audioRing : agg->ring;
    IHS_FrameStatsSlot *slot = &ring[frameId % IHS_FRAME_STATS_RING_SIZE];
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

bool IHS_FrameStatsAccumulatorAdd(IHS_FrameStatsAccumulator *accum, unsigned statId, float value) {
    if (statId >= IHS_FRAME_STATS_ACCUM_SLOTS)
        return false;
    IHS_FrameStatsAccumSlot *slot = &accum->slots[statId];
    if (slot->count == INT32_MAX)
        return false;
    if (slot->count == slot->capacity) {
        uint32_t capacity = slot->capacity ? slot->capacity : 8;
        if (capacity <= slot->count)
            capacity = capacity > INT32_MAX / 2 ? INT32_MAX : capacity * 2;
        if ((uint64_t)capacity * sizeof(float) > SIZE_MAX)
            return false;
        float *samples = realloc(slot->samples, (size_t)capacity * sizeof(float));
        if (!samples)
            return false; /* Local allocation protection, not an official OOM policy. */
        slot->samples = samples;
        slot->capacity = capacity;
    }
    slot->sum += value;
    slot->samples[slot->count++] = value;
    return true;
}

void IHS_FrameStatsAccumulatorReset(IHS_FrameStatsAccumulator *accum) {
    for (unsigned i = 0; i < IHS_FRAME_STATS_ACCUM_SLOTS; ++i)
        free(accum->slots[i].samples);
    memset(accum, 0, sizeof(*accum));
}

size_t IHS_FrameStatsAccumulatorEncode(const IHS_FrameStatsAccumulator *accum,
                                       CFrameStatAccumulatedValue *rows,
                                       CFrameStatAccumulatedValue **pointers) {
    size_t count = 0;
    for (unsigned stat = 0; stat < IHS_FRAME_STATS_ACCUM_SLOTS; ++stat) {
        const IHS_FrameStatsAccumSlot *s = &accum->slots[stat];
        if (!s->count)
            continue;
        CFrameStatAccumulatedValue *row = &rows[count];
        cframe_stat_accumulated_value__init(row);
        row->stat_type = (EFrameAccumulatedStat)stat;
        row->count = (int32_t)s->count;
        row->average = s->count >= 2 ? s->sum / (float)s->count : s->sum;
        float squaredSum = 0;
        uint32_t grouped = s->count & ~7u;
        for (uint32_t i = 0; i < grouped; ++i) {
            float delta = s->samples[i] - row->average;
            /* Original vector FMUL rounds before each sequential FADD. Avoid
             * compiler contraction here; the scalar tail explicitly uses FMA. */
            volatile float square = delta * delta;
            squaredSum += square;
        }
        for (uint32_t i = grouped; i < s->count; ++i) {
            float delta = s->samples[i] - row->average;
            squaredSum = fmaf(delta, delta, squaredSum);
        }
        float stddev = s->count >= 2 ? sqrtf(squaredSum / (float)(s->count - 1)) : 0;
        if (stddev != 0) {
            row->has_stddev = 1;
            row->stddev = stddev;
        }
        pointers[count] = row;
        ++count;
    }
    return count;
}

void IHS_FrameStatsEncodeReport(const IHS_FrameStatsReport *report, int32_t offset,
                                 IHS_FrameStatsMessage *output) {
    CFrameStatsListMsg *message = &output->message;
    cframe_stats_list_msg__init(message);
    message->data_type = k_EStreamingVideoData;
    message->latest_frame_id = report->latestFrameId;
    message->n_accumulated_stats = IHS_FrameStatsAccumulatorEncode(
        &report->accumulator, output->accumulated, output->accumulatedPointers);
    message->accumulated_stats = output->accumulatedPointers;
    if (report->fullReporting) {
        size_t eventCount = 0;
        assert(report->count <= IHS_FRAME_REPORT_CAPACITY);
        for (size_t i = 0; i < report->count; ++i) {
            IHS_FrameStatsEncodeFrame(&report->frames[i], offset, &output->frames[i],
                                      &output->events[eventCount], &output->eventPointers[eventCount]);
            eventCount += output->frames[i].n_events;
            output->framePointers[i] = &output->frames[i];
        }
        message->n_stats = report->count;
        message->stats = output->framePointers;
    }
}

bool IHS_SessionSendFrameEvents(IHS_Session *session, IHS_FrameStatsMessage *scratch) {
    IHS_SessionChannel *stats = IHS_SessionChannelFor(session, IHS_SessionChannelIdStats);
    if (!stats || !session->frameStats)
        return false;
    if (session->frameTracker)
        IHS_FrameStatsCollectTracked(session->frameStats, session->frameTracker);
    const IHS_FrameStatsReport *report = IHS_FrameStatsReportBegin(session->frameStats);
    if (!report)
        return false;
    IHS_FrameStatsEncodeReport(report, IHS_StreamClockOffset(&session->clock), scratch);
    uint64_t serial = report->serial;
    bool accepted = IHS_SessionChannelStatsSend(stats, k_EStreamStatsFrameEvents,
        (const ProtobufCMessage *)&scratch->message, IHS_PACKET_ID_NEXT);
    IHS_FrameStatsReportFinish(session->frameStats, serial, accepted);
    return true; /* An attempt, not an assertion that transport accepted it. */
}

void IHS_FrameStatsSetNetworkSample(IHS_FrameStatsAggregator *agg, const float values[5]) {
    IHS_MutexLock(agg->lock);
    memcpy(agg->networkSample, values, sizeof(agg->networkSample));
    IHS_MutexUnlock(agg->lock);
}

static float unsigned_ticks_to_ms(uint32_t delta) {
    float whole = (float)((delta >> 16) * 1000u);
    float fraction = (float)((delta & 65535u) * 1000u) * (1.0f / 65536.0f);
    return whole + fraction;
}

/* Fold one completed slot into the accumulator. Slot lock held by caller. */
static void fold_slot(IHS_FrameStatsAggregator *agg, IHS_FrameStatsSlot *slot) {
    IHS_FrameStatsAccumulator *accum = &agg->accumulator;
    const uint32_t *e = slot->events;
    if (e[EV_FRAME_RECV])
        IHS_FrameStatsAccumulatorAdd(accum, 6, ticks_to_ms(e[EV_FRAME_SEND], e[EV_FRAME_RECV]));
    if (e[EV_DECODE_END])
        IHS_FrameStatsAccumulatorAdd(accum, 7, ticks_to_ms(e[EV_DECODE_BEGIN], e[EV_DECODE_END]));
    if (slot->result != IHS_VideoFrameResultDisplayed)
        return;
    IHS_FrameStatsAccumulatorAdd(accum, 8, ticks_to_ms(e[EV_UPLOAD_BEGIN], e[EV_COMPLETE]));
    IHS_FrameStatsAccumulatorAdd(accum, 9, ticks_to_ms(e[EV_FRAME_RECV], e[EV_COMPLETE]));
    IHS_FrameStatsAccumulatorAdd(accum, 10, ticks_to_ms(e[EV_FRAME_START], e[EV_COMPLETE]));
    float frameFps = slot->frameStartDelta != 0 ? 1000.0f / slot->frameStartDelta : 0;
    float displayFps = slot->frameDisplayDelta != 0 ? 1000.0f / slot->frameDisplayDelta : 0;
    float delta = frameFps - displayFps;
    float fps = delta * delta > 10000.0f && frameFps < displayFps ? frameFps : displayFps;
    IHS_FrameStatsAccumulatorAdd(accum, 0, fps);
    if (e[EV_INPUT_START]) {
        uint32_t inputDelta = (e[EV_INPUT_START] - e[EV_INPUT_RECV]) & 0x80000000u
                                  ? e[EV_INPUT_RECV] - e[EV_INPUT_START] : 0;
        IHS_FrameStatsAccumulatorAdd(accum, 11, unsigned_ticks_to_ms(inputDelta));
        IHS_FrameStatsAccumulatorAdd(accum, 12, unsigned_ticks_to_ms(e[EV_FRAME_START] - e[EV_INPUT_RECV]));
        IHS_FrameStatsAccumulatorAdd(accum, 13, unsigned_ticks_to_ms(e[EV_COMPLETE] - e[EV_INPUT_START]));
    }
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
    if (!agg->reportPending) {
        report->count = FrameStatsDrainLocked(agg, report->frames, IHS_FRAME_STATS_RING_SIZE,
                                                  &report->latestFrameId);
        if (report->count) {
            for (unsigned i = 0; i < 5; ++i)
                IHS_FrameStatsAccumulatorAdd(&agg->accumulator, 14 + i, agg->networkSample[i]);
            if (++agg->reportSerial == 0)
                ++agg->reportSerial; /* Opaque local borrow token, never a protocol stop. */
            report->serial = agg->reportSerial;
            report->accumulator = agg->accumulator;
            report->fullReporting = agg->fullReporting;
            /* Move ownership to the immutable report. */
            memset(&agg->accumulator, 0, sizeof(agg->accumulator));
            agg->reportPending = true;
        } else {
            IHS_MutexUnlock(agg->lock);
            return NULL;
        }
    } else {
        IHS_MutexUnlock(agg->lock);
        return NULL;
    }
    IHS_MutexUnlock(agg->lock);
    return report;
}

bool IHS_FrameStatsReportFinish(IHS_FrameStatsAggregator *agg, uint64_t serial, bool accepted) {
    IHS_MutexLock(agg->lock);
    bool ok = agg->reportPending && agg->pendingReport.serial == serial;
    if (ok) {
        if (accepted)
            agg->lastQueuedFrameId = agg->pendingReport.latestFrameId;
        IHS_FrameStatsAccumulatorReset(&agg->pendingReport.accumulator);
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
        IHS_FrameStatsAccumulatorReset(&agg->pendingReport.accumulator);
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
static int compare_completions(const void *left, const void *right) {
    const IHS_TrackedFrame *a = left, *b = right;
    return (a->completionSerial > b->completionSerial) -
           (a->completionSerial < b->completionSerial);
}

void IHS_FrameStatsCollectTracked(IHS_FrameStatsAggregator *agg, IHS_FrameTracker *tracker) {
    /* Reservation and application must remain ordered across RX/report callers.
     * Never wait on tracker while holding the protocol stats lock. */
    IHS_MutexLock(agg->collectorLock);
    size_t capacity = IHS_FrameTrackerStorageCapacity(tracker);
    if (capacity > agg->trackedCapacity) {
        /* Snapshot all allocated blocks. On OOM preserve both old scratch and
         * unpublished completions so a later collection can retry. */
        if (capacity > SIZE_MAX / sizeof(IHS_TrackedFrame)) {
            IHS_MutexUnlock(agg->collectorLock);
            return;
        }
        IHS_TrackedFrame *grown = realloc(agg->trackedScratch, capacity * sizeof(*grown));
        if (!grown) {
            IHS_MutexUnlock(agg->collectorLock);
            return;
        }
        agg->trackedScratch = grown;
        agg->trackedCapacity = capacity;
    }
    IHS_TrackedFrame *frames = agg->trackedScratch;
    size_t count = IHS_FrameTrackerCopyStats(tracker, frames, agg->trackedCapacity);
    size_t pending = 0;
    for (size_t i = 0; i < count; ++i) {
        IHS_TrackedFrame *f = &frames[i];
        uint16_t id = f->identity.frameId;
        if (f->hasDecodeBegin) {
            uint32_t tick = us_to_ticks(f->decodeBeginUs);
            IHS_FrameStatsRecordStage(agg, id, IHS_VideoFrameStageDecodeBegin, tick ? tick : 1);
        }
        if (f->hasDecodeEnd) {
            uint32_t tick = us_to_ticks(f->decodeEndUs);
            IHS_FrameStatsRecordStage(agg, id, IHS_VideoFrameStageDecodeEnd, tick ? tick : 1);
        }
        if (f->hasUploadBegin) {
            uint32_t tick = us_to_ticks(f->uploadBeginUs);
            IHS_FrameStatsRecordStage(agg, id, IHS_VideoFrameStageUploadBegin, tick ? tick : 1);
        }
        if (f->hasUploadEnd) {
            uint32_t tick = us_to_ticks(f->uploadEndUs);
            IHS_FrameStatsRecordStage(agg, id, IHS_VideoFrameStageUploadEnd, tick ? tick : 1);
        }
        if (!f->statsComplete)
            continue;
        /* Existing producers may supply upload timing only with their outcome. */
        if (f->outcome.hasUpload && !f->hasUploadBegin) {
            uint32_t tick = us_to_ticks(f->outcome.uploadBeginUs);
            IHS_FrameStatsRecordStage(agg, id, IHS_VideoFrameStageUploadBegin, tick ? tick : 1);
        }
        if (f->outcome.hasUpload && !f->hasUploadEnd) {
            uint32_t tick = us_to_ticks(f->outcome.uploadEndUs);
            IHS_FrameStatsRecordStage(agg, id, IHS_VideoFrameStageUploadEnd, tick ? tick : 1);
        }
        /* Completed rows compact toward the beginning, never over an unread
         * row. Avoid a second fixed-size array of pointers to snapshot rows. */
        frames[pending++] = *f;
    }
    if (pending > 1) qsort(frames, pending, sizeof(*frames), compare_completions);
    for (size_t i = 0; i < pending; ++i) {
        IHS_TrackedFrame *f = &frames[i];
        IHS_FrameStatsRecordCompleteAt(agg, f->identity.frameId, f->outcome.result, f->completionTimestamp);
    }
    IHS_MutexUnlock(agg->collectorLock);
}

void IHS_FrameStatsSettleTracked(IHS_FrameStatsAggregator *agg, IHS_FrameTracker *tracker,
                                 uint64_t nowUs) {
    IHS_FrameStatsCollectTracked(agg, tracker);
    /* Resource retirement never fabricates a protocol Complete. Protocol
     * cancellation is controlled solely by the 240-slot report window. */
    IHS_TrackedFrame frames[8];
    size_t capacity = IHS_FrameTrackerStorageCapacity(tracker);
    for (size_t batch = 0; batch < capacity / 8; ++batch) {
        size_t count = IHS_FrameTrackerSettle(tracker, nowUs, 2000000, frames, 8);
        if (count < 8)
            break;
    }
}

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

#include "report.h"

#include <memory.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>

#include "crc32.h"

#include "protobuf/pb_utils.h"

static int ComputeDelta(const uint8_t *previous, const uint8_t *current, size_t inputLen, size_t reportLen,
                        uint8_t *delta);

static void ClearMessage(IHS_HIDReportHolder *holder, bool promotePending);

void IHS_HIDReportHolderInit(IHS_HIDReportHolder *holder, uint32_t deviceId) {
    chidmessage_from_remote__device_input_reports__device_input_report__init(&holder->report);
    PROTOBUF_C_SET_VALUE(holder->report, device, deviceId);
    IHS_BufferInit(&holder->dataBuffer, 256, 8192);
    IHS_ArrayListInit(&holder->reportItems, sizeof(CHIDDeviceInputReport));
    IHS_ArrayListInit(&holder->reportPointers, sizeof(CHIDDeviceInputReport *));
    IHS_ArrayListInit(&holder->reportOffsets, sizeof(size_t));
    holder->report.reports = NULL; /* bound in GetMessage */
    holder->reportLength = 0;
    holder->lastSent = NULL;
    holder->lastSentLen = 0;
    holder->pendingCurrent = NULL;
    holder->pendingCurrentLen = 0;
    holder->bufferCapacity = 0;
}

void IHS_HIDReportHolderDeinit(IHS_HIDReportHolder *holder) {
    holder->report.reports = NULL;
    IHS_ArrayListDeinit(&holder->reportOffsets);
    IHS_ArrayListDeinit(&holder->reportPointers);
    IHS_ArrayListDeinit(&holder->reportItems);
    IHS_BufferClear(&holder->dataBuffer, true);
    free(holder->lastSent);
    free(holder->pendingCurrent);
}

// Capture `current` as the pending state for the next send. Allocates the two scratch
// buffers lazily and grows them on demand.
static void Stash(IHS_HIDReportHolder *holder, const uint8_t *current, size_t len) {
    if (len > holder->bufferCapacity) {
        holder->lastSent = realloc(holder->lastSent, len);
        holder->pendingCurrent = realloc(holder->pendingCurrent, len);
        holder->bufferCapacity = len;
    }
    memcpy(holder->pendingCurrent, current, len);
    holder->pendingCurrentLen = len;
}

// True when `current` is byte-identical to what was last flushed, so a full report
// carrying it would tell the host nothing it does not already know.
static bool SameAsLastSent(const IHS_HIDReportHolder *holder, const uint8_t *current, size_t len) {
    return holder->lastSent != NULL && holder->lastSentLen == len &&
           memcmp(holder->lastSent, current, len) == 0;
}

void IHS_HIDReportHolderSetReportLength(IHS_HIDReportHolder *holder, size_t reportLen) {
    holder->reportLength = reportLen;
}

static void AddFull(IHS_HIDReportHolder *holder, const uint8_t *current, size_t len, bool force) {
    assert(holder->reportLength >= len);
    /* A full report is self-contained, so one that repeats the last flushed state is
     * normally pure noise. Forced callers use it as an explicit host-requested refresh
     * or low-rate heartbeat, so those bypass the byte-identical drop. */
    if (!force && SameAsLastSent(holder, current, len)) {
        return;
    }
    Stash(holder, current, len);
    size_t offset = holder->dataBuffer.size;
    uint8_t *data = IHS_BufferPointerForAppend(&holder->dataBuffer, len);
    memcpy(data, current, len);
    holder->dataBuffer.size += len;
    CHIDDeviceInputReport *item = IHS_ArrayListAppend(&holder->reportItems, NULL);

    chiddevice_input_report__init(item);
    item->has_full_report = true;
    item->full_report.data = NULL; /* bound in GetMessage */
    item->full_report.len = len;

    IHS_ArrayListAppend(&holder->reportOffsets, &offset);
    holder->report.n_reports = holder->reportItems.size;
}

void IHS_HIDReportHolderAddFull(IHS_HIDReportHolder *holder, const uint8_t *current, size_t len) {
    AddFull(holder, current, len, false);
}

void IHS_HIDReportHolderAddFullForced(IHS_HIDReportHolder *holder, const uint8_t *current, size_t len) {
    AddFull(holder, current, len, true);
}

void IHS_HIDReportHolderReplaceWithFullForced(IHS_HIDReportHolder *holder,
                                              const uint8_t *current, size_t len) {
    ClearMessage(holder, false);
    AddFull(holder, current, len, true);
}

void IHS_HIDReportHolderAddForcedFullMaskDelta(IHS_HIDReportHolder *holder,
                                               const uint8_t *current, size_t len) {
    /* Same wire shape as a delta report, but with every mask bit forced so the
     * payload carries the complete state: the host can resync from any baseline.
     * The official client expresses resync state this way - its
     * CHIDDeviceInputReport::set_full_report has no call sites. */
    assert(holder->reportLength >= len);
    Stash(holder, current, len);
    size_t offset = holder->dataBuffer.size;
    size_t maskSize = ((holder->reportLength + 7) >> 3);
    size_t deltaMax = maskSize + len;
    uint8_t *data = IHS_BufferPointerForAppend(&holder->dataBuffer, deltaMax);
    memset(data, 0, maskSize);
    memcpy(data + maskSize, current, len);
    for (size_t i = 0; i < len; ++i) {
        data[i >> 3] |= 1 << (i % 8);
    }
    holder->dataBuffer.size += deltaMax;
    CHIDDeviceInputReport *item = IHS_ArrayListAppend(&holder->reportItems, NULL);

    chiddevice_input_report__init(item);
    item->has_delta_report = true;
    item->delta_report.data = NULL; /* bound in GetMessage */
    item->delta_report.len = deltaMax;
    PROTOBUF_C_P_SET_VALUE(item, delta_report_crc, IHS_CRC32(current, len));
    PROTOBUF_C_P_SET_VALUE(item, delta_report_size, len);

    IHS_ArrayListAppend(&holder->reportOffsets, &offset);
}

void IHS_HIDReportHolderAddDelta(IHS_HIDReportHolder *holder, const uint8_t *previous, const uint8_t *current,
                                 size_t len) {
    /* Never drop a delta, even one that lands back on the last flushed state. Deltas
     * are a chain: the host applies each to the state the previous one left it in, and
     * the caller has already advanced its own `previous` past this one. Press-then-
     * release inside a single frame used to lose the release, leaving the host with a
     * held button and every later delta applied to a state that no longer exists.
     * The callers only add a delta when the bytes actually changed, so there is
     * nothing to deduplicate here anyway. */
    Stash(holder, current, len);
    size_t offset = holder->dataBuffer.size;
    /* Worst case is every byte changed: a full changed-byte bitmask, then a copy of
     * each changed byte. Reserving only reportLength overflows the buffer by the
     * size of the mask, which stayed hidden while a message held a single report. */
    size_t deltaMax = ((holder->reportLength + 7) >> 3) + len;
    uint8_t *data = IHS_BufferPointerForAppend(&holder->dataBuffer, deltaMax);
    int deltaLen = ComputeDelta(previous, current, len, holder->reportLength, data);
    assert(deltaLen >= 0 && (size_t) deltaLen <= deltaMax);
    uint32_t crc = IHS_CRC32(current, len);
    CHIDDeviceInputReport *item = IHS_ArrayListAppend(&holder->reportItems, NULL);
    chiddevice_input_report__init(item);

    /* Official adaptive rule (libmain 0x7d035c): delta only when it actually
     * compresses — deltaLen + 8 < fullLen — otherwise emit the full state.
     * On a 48-byte gamepad state this almost always stays a delta, matching
     * the official wire shape. */
    if ((size_t) deltaLen + 8 >= len) {
        holder->dataBuffer.size += deltaMax; /* reserve; full path binds below */
        size_t offset = holder->dataBuffer.size;
        uint8_t *full = IHS_BufferPointerForAppend(&holder->dataBuffer, len);
        memcpy(full, current, len);
        holder->dataBuffer.size += len;
        item->has_full_report = true;
        item->full_report.data = NULL; /* bound in GetMessage */
        item->full_report.len = len;
        IHS_ArrayListAppend(&holder->reportOffsets, &offset);
        holder->report.n_reports = holder->reportItems.size;
        return;
    }
    holder->dataBuffer.size += deltaLen;
    item->has_delta_report = true;
    item->delta_report.data = NULL; /* bound in GetMessage */
    item->delta_report.len = deltaLen;
    PROTOBUF_C_P_SET_VALUE(item, delta_report_crc, crc);
    PROTOBUF_C_P_SET_VALUE(item, delta_report_size, len);

    IHS_ArrayListAppend(&holder->reportOffsets, &offset);
    holder->report.n_reports = holder->reportItems.size;
}

IHS_HIDDeviceReportMessage *IHS_HIDReportHolderGetMessage(IHS_HIDReportHolder *holder) {
    if (holder->reportItems.size == 0) {
        return NULL;
    }
    /* Bind data pointers now: dataBuffer, reportItems and reportPointers all
     * reallocate as reports accumulate, so anything captured at Add time dangles.
     * Only ever safe when the message held a single report — it no longer does. */
    uint8_t *base = IHS_BufferPointer(&holder->dataBuffer);
    IHS_ArrayListClear(&holder->reportPointers);
    for (size_t i = 0; i < holder->reportItems.size; i++) {
        CHIDDeviceInputReport *item = IHS_ArrayListGet(&holder->reportItems, i);
        size_t offset = *(size_t *) IHS_ArrayListGet(&holder->reportOffsets, i);
        if (item->has_full_report) {
            item->full_report.data = base + offset;
        } else {
            item->delta_report.data = base + offset;
        }
        IHS_ArrayListAppend(&holder->reportPointers, &item);
    }
    holder->report.reports = holder->reportPointers.data;
    holder->report.n_reports = holder->reportItems.size;
    return &holder->report;
}

void IHS_HIDReportHolderResetMessage(IHS_HIDReportHolder *holder) {
    ClearMessage(holder, true);
}

static void ClearMessage(IHS_HIDReportHolder *holder, bool promotePending) {
    holder->report.n_reports = 0;
    holder->report.reports = NULL;
    IHS_BufferClear(&holder->dataBuffer, false);
    IHS_ArrayListClear(&holder->reportItems);
    IHS_ArrayListClear(&holder->reportPointers);
    IHS_ArrayListClear(&holder->reportOffsets);
    // Promote the most recent pending state to lastSent so the next dedup check
    // compares against what just went out on the wire.
    if (promotePending && holder->pendingCurrentLen > 0) {
        uint8_t *swap = holder->lastSent;
        holder->lastSent = holder->pendingCurrent;
        holder->lastSentLen = holder->pendingCurrentLen;
        holder->pendingCurrent = swap;
        holder->pendingCurrentLen = 0;
    } else if (!promotePending) {
        holder->pendingCurrentLen = 0;
    }
}

static int ComputeDelta(const uint8_t *previous, const uint8_t *current, size_t inputLen, size_t reportLen,
                        uint8_t *delta) {
    int size = (int) (reportLen + 7) >> 3;
    memset(delta, 0, size);
    for (size_t i = 0; i < inputLen; ++i) {
        if (previous[i] == current[i]) {
            continue;
        }
        delta[i >> 3] |= 1 << (i % 8);
        delta[size] = current[i];
        size++;
    }
    return size;
}

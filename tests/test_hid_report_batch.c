/*
 * Regression: several reports accumulated into one message. dataBuffer, reportItems
 * and reportPointers all reallocate as they grow, so any pointer captured when the
 * report was added dangles. Only ever safe while a message held a single report.
 *
 * Run under ASan: the failure mode is a use-after-realloc, not a wrong assert.
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "hid/report.h"

#define REPORT_LEN 48
#define N_REPORTS 32 /* far past dataBuffer's initial 256-byte capacity */

int main() {
    IHS_HIDReportHolder holder;
    IHS_HIDReportHolderInit(&holder, 1);
    IHS_HIDReportHolderSetReportLength(&holder, REPORT_LEN);

    uint8_t previous[REPORT_LEN], current[REPORT_LEN];
    memset(previous, 0, sizeof(previous));

    for (int i = 0; i < N_REPORTS; i++) {
        /* Worst case on purpose: EVERY byte changes, so the delta is a full bitmask
         * plus a full copy of the report — larger than the report itself. */
        memset(current, (uint8_t) (i + 1), sizeof(current));
        IHS_HIDReportHolderAddDelta(&holder, previous, current, REPORT_LEN);
        memcpy(previous, current, REPORT_LEN);
    }

    IHS_HIDDeviceReportMessage *msg = IHS_HIDReportHolderGetMessage(&holder);
    assert(msg != NULL);
    assert(msg->n_reports == N_REPORTS);

    /* Adaptive rule (official 0x7d035c): every byte changed makes the delta
     * (mask + every byte) bigger than the full report, so each item must be a
     * FULL report instead. Every item must be reachable, in-bounds, and carry
     * the exact current bytes. */
    for (size_t i = 0; i < msg->n_reports; i++) {
        CHIDDeviceInputReport *item = msg->reports[i];
        assert(item != NULL);
        assert(item->has_full_report);
        assert(!item->has_delta_report);
        assert(item->full_report.data != NULL);
        assert(item->full_report.len == REPORT_LEN);
        /* Touch it: ASan traps here if the pointer outlived a realloc. */
        volatile uint8_t first = item->full_report.data[0];
        (void) first;
        assert(item->full_report.data[0] == (uint8_t) (i + 1));
    }

    /* The items must be distinct, i.e. pointers were rebound per item, not all
     * left pointing at the last appended chunk. */
    assert(msg->reports[0]->full_report.data != msg->reports[N_REPORTS - 1]->full_report.data);

    /* A small change still compresses to a delta: flip one byte. */
    IHS_HIDReportHolderResetMessage(&holder);
    memset(previous, 0, sizeof(previous));
    memcpy(current, previous, sizeof(current));
    current[0] = 0xff;
    IHS_HIDReportHolderAddDelta(&holder, previous, current, REPORT_LEN);
    msg = IHS_HIDReportHolderGetMessage(&holder);
    assert(msg != NULL && msg->n_reports == 1);
    assert(msg->reports[0]->has_delta_report);
    assert(msg->reports[0]->delta_report.len == (REPORT_LEN + 7) / 8 + 1); /* 6-byte mask + 1 changed byte */

    IHS_HIDReportHolderResetMessage(&holder);
    assert(IHS_HIDReportHolderGetMessage(&holder) == NULL);

    IHS_HIDReportHolderDeinit(&holder);
    puts("hid report batch OK");
    return 0;
}

/*
 * Regression: a delta that lands back on the last flushed state must still be sent.
 *
 * Deltas are a chain. The host applies each one to the state the previous one left it
 * in. Press-then-release inside a single frame produces two deltas whose net effect is
 * zero, and the old dedup dropped the second because its bytes matched the last flushed
 * report. The host was left holding a button forever, and every later delta was applied
 * to a state that no longer existed.
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "hid/report.h"

#define REPORT_LEN 48

int main() {
    IHS_HIDReportHolder holder;
    IHS_HIDReportHolderInit(&holder, 1);
    IHS_HIDReportHolderSetReportLength(&holder, REPORT_LEN);

    uint8_t idle[REPORT_LEN], pressed[REPORT_LEN];
    memset(idle, 0, sizeof(idle));
    memcpy(pressed, idle, sizeof(idle));
    pressed[0] = 0x01; /* one button down */

    /* Flush an idle report so lastSent == idle. */
    IHS_HIDReportHolderAddFull(&holder, idle, REPORT_LEN);
    assert(IHS_HIDReportHolderGetMessage(&holder) != NULL);
    IHS_HIDReportHolderResetMessage(&holder);

    /* Press, then release, before the next send. */
    IHS_HIDReportHolderAddDelta(&holder, idle, pressed, REPORT_LEN);
    IHS_HIDReportHolderAddDelta(&holder, pressed, idle, REPORT_LEN);

    IHS_HIDDeviceReportMessage *msg = IHS_HIDReportHolderGetMessage(&holder);
    assert(msg != NULL);
    /* Both deltas, or the host never sees the button come back up. */
    assert(msg->n_reports == 2);
    assert(msg->reports[0]->has_delta_report);
    assert(msg->reports[1]->has_delta_report);
    assert(msg->reports[0]->delta_report.data != msg->reports[1]->delta_report.data);

    /* A repeated full report carries nothing new and may be dropped: it breaks no chain. */
    IHS_HIDReportHolderResetMessage(&holder);
    IHS_HIDReportHolderAddFull(&holder, idle, REPORT_LEN);
    assert(IHS_HIDReportHolderGetMessage(&holder) == NULL);

    IHS_HIDReportHolderDeinit(&holder);
    puts("hid report chain OK");
    return 0;
}

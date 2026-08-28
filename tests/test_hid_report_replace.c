#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hid/report.h"

#define REPORT_LEN 48

int main(void) {
    IHS_HIDReportHolder holder;
    IHS_HIDReportHolderInit(&holder, 7);
    IHS_HIDReportHolderSetReportLength(&holder, REPORT_LEN);

    uint8_t idle[REPORT_LEN] = {0};
    uint8_t pressed[REPORT_LEN] = {0};
    uint8_t latest[REPORT_LEN] = {0};
    pressed[0] = 1;
    latest[4] = 0x7f;

    IHS_HIDReportHolderAddDelta(&holder, idle, pressed, REPORT_LEN);
    IHS_HIDReportHolderAddDelta(&holder, pressed, idle, REPORT_LEN);
    IHS_HIDReportHolderReplaceWithFullForced(&holder, latest, REPORT_LEN);

    IHS_HIDDeviceReportMessage *message = IHS_HIDReportHolderGetMessage(&holder);
    assert(message != NULL && message->n_reports == 1);
    assert(message->reports[0]->has_full_report);
    assert(message->reports[0]->full_report.len == REPORT_LEN);
    assert(memcmp(message->reports[0]->full_report.data, latest, REPORT_LEN) == 0);

    IHS_HIDReportHolderResetMessage(&holder);
    IHS_HIDReportHolderReplaceWithFullForced(&holder, latest, REPORT_LEN);
    message = IHS_HIDReportHolderGetMessage(&holder);
    assert(message != NULL && message->n_reports == 1);
    assert(message->reports[0]->has_full_report);

    IHS_HIDReportHolderDeinit(&holder);
    puts("HID report replacement OK");
    return 0;
}

/* Production report serialization fixtures for the pinned Windows host audit.
 * Synthetic samples; no SDL device, network or session is opened. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hid/sdl/src/sdl_hid_common.h"
#include "hid/report.h"

static void hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) printf("%02x", data[i]);
}

static void emit(IHS_HIDReportHolder *holder, IHS_HIDStateSDL *current,
                 IHS_HIDStateSDL *previous, const char *name, bool motion, bool full) {
    uint8_t wire[72], prev[72];
    IHS_HIDReportSDLPackWire(wire, sizeof(wire), current);
    IHS_HIDReportSDLPackWire(prev, sizeof(prev), previous);
    if (full) IHS_HIDReportHolderAddFullForced(holder, wire, sizeof(wire));
    else IHS_HIDReportHolderAddDelta(holder, prev, wire, sizeof(wire));
    IHS_HIDDeviceReportMessage *message = IHS_HIDReportHolderGetMessage(holder);
    assert(message && message->n_reports == 1);
    const CHIDDeviceInputReport *item = message->reports[0];
    printf("{\"name\":\"%s\",\"motion\":%s,\"wire\":\"", name, motion ? "true" : "false");
    hex(wire, sizeof(wire));
    printf("\",\"kind\":\"%s\",\"payload\":\"", item->has_full_report ? "full" : "delta");
    if (item->has_full_report) hex(item->full_report.data, item->full_report.len);
    else hex(item->delta_report.data, item->delta_report.len);
    printf("\",\"size\":%u,\"crc\":%u}\n", item->delta_report_size, item->delta_report_crc);
    IHS_HIDReportHolderResetMessage(holder);
    *previous = *current;
}

int main(void) {
    IHS_HIDStateSDL current, previous;
    IHS_HIDReportSDLInit(&current);
    IHS_HIDReportSDLInit(&previous);
    IHS_HIDReportHolder holder;
    IHS_HIDReportHolderInit(&holder, 1);
    IHS_HIDReportHolderSetReportLength(&holder, 73);
    const float gyro[3] = {1.0f, -2.0f, 3.0f}, accel[3] = {0.0f, 9.80665f, 0.0f};
    IHS_HIDReportSDLSetButton(&current, SDL_CONTROLLER_BUTTON_A, true);
    IHS_HIDReportSDLSetAxis(&current, SDL_CONTROLLER_AXIS_LEFTX, 1234);
    emit(&holder, &current, &previous, "controls-only-full", false, true);
    IHS_HIDReportSDLSetGyro(&current, gyro);
    IHS_HIDReportSDLSetAccel(&current, accel);
    IHS_HIDReportSDLSetSensorTime(&current, 100000000, 100000000);
    emit(&holder, &current, &previous, "motion-delta", true, false);
    assert(!IHS_HIDReportSDLSetGyro(&current, gyro));
    assert(IHS_HIDReportSDLSetSensorTime(&current, 104000000, 104000000));
    emit(&holder, &current, &previous, "same-values-new-time", true, false);
    IHS_HIDReportSDLSetButton(&current, SDL_CONTROLLER_BUTTON_A, false);
    IHS_HIDReportSDLBeginInput(&current, &previous);
    emit(&holder, &current, &previous, "button-release-no-stale-imu", false, false);
    IHS_HIDReportSDLSetGyro(&current, gyro);
    IHS_HIDReportSDLSetSensorTime(&current, 108000000, 108000000);
    IHS_HIDReportSDLSetButton(&current, SDL_CONTROLLER_BUTTON_B, true);
    IHS_HIDReportSDLBeginInput(&current, &previous);
    emit(&holder, &current, &previous, "pending-motion-and-button", true, false);
    emit(&holder, &current, &previous, "requested-full-motion", true, true);
    IHS_HIDReportSDLClearSensors(&current);
    emit(&holder, &current, &previous, "sensors-cleared-controls-kept", false, false);
    IHS_HIDReportHolderDeinit(&holder);
    return 0;
}

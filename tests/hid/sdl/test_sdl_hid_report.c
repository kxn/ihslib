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
#include <assert.h>
#include <string.h>
#include "hid/sdl/src/sdl_hid_report.h"
#include "hid/sdl/src/sdl_hid_common.h"

int main() {
    IHS_HIDStateSDL report;
    IHS_HIDReportSDLInit(&report);
    IHS_HIDReportSDLSetRequestedReportVersion(&report, 1);

    assert(!IHS_HIDReportSDLSetButton(&report, SDL_CONTROLLER_BUTTON_PADDLE1, true));
    assert(IHS_HIDReportSDLSetButton(&report, SDL_CONTROLLER_BUTTON_A, true));
    assert(IHS_HIDReportSDLSetButton(&report, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, true));

    int8_t buttonsExpected[] = {0x01, 0x40};
    assert(memcmp(((int8_t *) &report) + 16, buttonsExpected, 2) == 0);

    assert(IHS_HIDReportSDLSetButton(&report, SDL_CONTROLLER_BUTTON_A, false));
    assert(IHS_HIDReportSDLSetButton(&report, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, false));

    assert(!IHS_HIDReportSDLSetAxis(&report, SDL_CONTROLLER_AXIS_MAX, 0));
    assert(IHS_HIDReportSDLSetAxis(&report, SDL_CONTROLLER_AXIS_LEFTX, INT16_MAX));

    // Regression: SetAccel previously wrote into report->gyro[] instead of report->accel[].
    float accel[3] = {1.0f, 2.0f, 3.0f};
    float gyro[3] = {-1.0f, -2.0f, -3.0f};
    assert(IHS_HIDReportSDLSetAccel(&report, accel));
    // Gyro must remain untouched by SetAccel.
    assert(report.gyro[0] == 0 && report.gyro[1] == 0 && report.gyro[2] == 0);
    // Accel must contain the remapped values, not zero.
    assert(report.accel[0] != 0 && report.accel[1] != 0 && report.accel[2] != 0);
    int16_t accelBefore[3] = {report.accel[0], report.accel[1], report.accel[2]};
    // SetGyro must update gyro and leave accel intact.
    assert(IHS_HIDReportSDLSetGyro(&report, gyro));
    assert(report.gyro[0] != 0 && report.gyro[1] != 0 && report.gyro[2] != 0);
    assert(report.accel[0] == accelBefore[0]);
    assert(report.accel[1] == accelBefore[1]);
    assert(report.accel[2] == accelBefore[2]);
    // Official OnSensorEvent sets these presence bits; the host uses them to
    // decide whether the IMU fields in the RAW report are meaningful.
    assert((report.flags & 0x6u) == 0x6u);

    // Wire layout from the official Linux 1.3.9.258 client:
    //   flags u32 @12, version @27 (nonzero = RAW), gyro s16[3] @28,
    //   accel s16[3] @34. The packer must not zero that area.
    uint8_t wire[72];
    memset(wire, 0xa5, sizeof(wire));
    IHS_HIDReportSDLPackWire(wire, sizeof(wire), &report);
    uint32_t wireFlags;
    memcpy(&wireFlags, wire + 12, sizeof(wireFlags));
    assert(wireFlags == 0x406u);
    assert(wire[27] == 3);
    int16_t wireGyro[3], wireAccel[3];
    memcpy(wireGyro, wire + 28, sizeof(wireGyro));
    memcpy(wireAccel, wire + 34, sizeof(wireAccel));
    assert(wireGyro[0] == report.gyro[0] && wireGyro[1] == report.gyro[1] &&
           wireGyro[2] == report.gyro[2]);
    assert(wireAccel[0] == report.accel[0] && wireAccel[1] == report.accel[1] &&
           wireAccel[2] == report.accel[2]);
    for (size_t i = 56; i < sizeof(wire); i++) {
        assert(wire[i] == 0);
    }

    // A 28-byte ENCODED-sized packet still carries the version byte but no IMU.
    uint8_t shortWire[28];
    memset(shortWire, 0xa5, sizeof(shortWire));
    IHS_HIDReportSDLPackWire(shortWire, sizeof(shortWire), &report);
    assert(shortWire[27] == 3);
    // Clearing sensors must remove the bits without disturbing a button.
    assert(IHS_HIDReportSDLSetButton(&report, SDL_CONTROLLER_BUTTON_A, true));
    assert(IHS_HIDReportSDLClearSensors(&report));
    assert((report.flags & 0x406u) == 0);
    assert(report.buttons != 0);

    // Pin the official fixed-point scale: +/-2 g and +/-2000 dps map to the
    // full s16 range, so 1 g and 1000 dps are ~16384 counts.
    IHS_HIDStateSDL scale;
    IHS_HIDReportSDLInit(&scale);
    const float one_g[3] = {9.80665f, 0.0f, 0.0f};
    const float one_kdps[3] = {17.45329f, 0.0f, 0.0f}; /* 1000 deg/s in rad/s */
    IHS_HIDReportSDLSetAccel(&scale, one_g);
    IHS_HIDReportSDLSetGyro(&scale, one_kdps);
    assert(scale.accel[0] >= 16382 && scale.accel[0] <= 16386);
    assert(scale.gyro[0] >= 16382 && scale.gyro[0] <= 16386);

    // Identical angular velocity at a new time is still a new sensor sample.
    assert(IHS_HIDReportSDLSetSensorTime(&scale, 100000000, 100000000));
    IHS_HIDStateSDL submitted = scale;
    assert(IHS_HIDReportSDLSetSensorTime(&scale, 104000000, 104000000));
    IHS_HIDReportSDLBeginInput(&scale, &submitted);
    assert(scale.flags & IHS_HID_SDL_SENSOR_FRESH); // keep unsubmitted IMU
    submitted = scale;
    IHS_HIDReportSDLBeginInput(&scale, &submitted);
    assert(!(scale.flags & IHS_HID_SDL_SENSOR_FRESH)); // no stale replay
    assert(scale.gyroTimestamp100ns == 0);
    assert(submitted.gyroTimestamp100ns == 1040000); // delta baseline untouched
    // Full 32-bit timestamp domains wrap independently; no signed arithmetic.
    IHS_HIDReportSDLSetSensorTime(&scale, UINT64_C(4294967300) * 1000,
                                 UINT64_C(4294967300) * 100);
    assert(scale.eventTimestampUs == 4 && scale.gyroTimestamp100ns == 4);
    IHS_HIDReportSDLPackWire(wire, sizeof(wire), &scale);
    uint32_t times[2];
    memcpy(times, wire + 48, sizeof(times));
    assert(times[0] == 4 && times[1] == 4);

    return 0;
}
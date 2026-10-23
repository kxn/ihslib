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

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <SDL3/SDL_gamepad.h>

typedef struct __attribute__((__packed__)) IHS_HIDReportSDL {
    int16_t axes[6];
    uint32_t flags;
    uint16_t buttons;
    uint8_t padding1[9];
    uint8_t reportUnknown;
    int16_t gyro[3];
    int16_t accel[3];
    int16_t touch[2];
    uint8_t padding2[4];
    uint32_t eventTimestampUs;
    uint32_t gyroTimestamp100ns;
} IHS_HIDStateSDL;

_Static_assert(sizeof(IHS_HIDStateSDL) == 56, "");
_Static_assert(offsetof(IHS_HIDStateSDL, eventTimestampUs) == 0x30, "RAW event time");
_Static_assert(offsetof(IHS_HIDStateSDL, gyroTimestamp100ns) == 0x34, "RAW gyro time");

#define IHS_HID_SDL_GYRO_PRESENT 0x2u
#define IHS_HID_SDL_ACCEL_PRESENT 0x4u
#define IHS_HID_SDL_SENSOR_FRESH 0x400u

/** Record a sample's time independently of its quantized values. Wire time
 * units follow Steam's SDL3 producer (event ns/1000, sensor ns/100). */
bool IHS_HIDReportSDLSetSensorTime(IHS_HIDStateSDL *report, uint64_t eventNs,
                                  uint64_t sensorNs);

/** A button/axis/accel-only event must not replay an already submitted gyro. */
void IHS_HIDReportSDLBeginInput(IHS_HIDStateSDL *report, const IHS_HIDStateSDL *previous);

bool IHS_HIDReportSDLInit(IHS_HIDStateSDL *report);

bool IHS_HIDReportSDLSetRequestedReportVersion(IHS_HIDStateSDL *report, uint8_t version);

bool IHS_HIDReportSDLSetButton(IHS_HIDStateSDL *report, SDL_GamepadButton button, bool pressed);

bool IHS_HIDReportSDLSetAxis(IHS_HIDStateSDL *report, SDL_GamepadAxis axis, int16_t value);

bool IHS_HIDReportSDLSetAccel(IHS_HIDStateSDL *report, const float accel[3]);

bool IHS_HIDReportSDLSetGyro(IHS_HIDStateSDL *report, const float gyro[3]);

/** Clear only the IMU fields and their presence flags, leaving buttons/sticks. */
bool IHS_HIDReportSDLClearSensors(IHS_HIDStateSDL *report);

bool IHS_HIDReportSDLClear(IHS_HIDStateSDL *report);

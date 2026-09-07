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

#include "ihslib/hid/sdl.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "hid/device.h"

#include "sdl_hid_report.h"
#include "hid/manager.h"

typedef struct IHS_HIDDeviceSDL {
    IHS_HIDDevice base;
    /**
     * If true, the controller will be closed when the device is closed
     */
    bool controllerManaged;
    SDL_JoystickID instanceId;
    SDL_Gamepad *controller;
    int playerIndex;
    struct {
        IHS_HIDStateSDL current;
        IHS_HIDStateSDL previous;
    } states;
    /** Snapshot of the values packed into the most recent wire submission. */
    IHS_HIDStateSDL lastSubmitted;
    uint64_t lastSubmittedSeq;
    /** Host DeviceWrite commands queued on the session receive thread and
     * applied on the flush thread: SDL gamepad calls must stay off the
     * receive thread (SDL_RumbleGamepad there deadlocks against
     * SDL_PollEvent on the media thread — verified on hardware). */
    IHS_Buffer pendingWrites;
} IHS_HIDDeviceSDL;

/** Apply queued host device-write commands (rumble etc.). Must be called
 * with the device lock HELD, from the flush thread only. */
void IHS_HIDDeviceSDLApplyPendingWrites(IHS_HIDDeviceSDL *sdl);

#define IHS_HID_SDL_BASE_REPORT_LEN ((size_t) sizeof(IHS_HIDStateSDL))
#define IHS_HID_SDL_WIRE_REPORT_MAX 72U

/** Wire length for a device: the host-declared report length capped at the
 * RAW V2 struct size (libmain Pack RAW mode memcpy's at most 72 bytes,
 * 0x7cfd8c). Never exceed what the host announced (delta masks are sized
 * from it), never send zero (callers treat that as "reports not started"). */
static inline size_t IHS_HIDDeviceSDLWireReportLength(const IHS_HIDManagedDevice *managed) {
    size_t requested = managed != NULL ? managed->reportHolder.reportLength : 0;
    if (requested == 0) {
        return 0;
    }
    return requested < IHS_HID_SDL_WIRE_REPORT_MAX ? requested : IHS_HID_SDL_WIRE_REPORT_MAX;
}

/** SDL3 gamepad button -> wire bit. The wire bitfield at state+16 uses
 * SDL_GamepadButton positions directly: the official client's
 * OnButtonEvent (libmain 0x754034) writes `1 << ev->button` into the u32
 * at +16, and ihslib's original IHS_HIDReportSDLSetButton did the same.
 * (The EGamepadButton protobuf enum is a different thing — used by config
 * messages, not by this bitfield; mapping wire bits to it scrambled every
 * button on hardware on 2026-09-07.) SDL3 and SDL2 agree on 0-based
 * positions: SOUTH/A=0, EAST/B=1, WEST/X=2, NORTH/Y=3, BACK=4, GUIDE=5,
 * START=6, L3=7, R3=8, LB=9, RB=10, DPad Up=11 Down=12 Left=13 Right=14,
 * MISC1=15. The internal state already stores exactly these bits, so the
 * u16 passes through unchanged. */

/** Pack the internal SDL state into the official Generic Gamepad wire
 * format: RAW mode of HIDDeviceSDLGamepadStateV2_t (byte 27 = version 3).
 * Layout: axes 6×s16 @0, flags u32 @12 (0), buttons bitfield @16
 * (SDL_GamepadButton positions, official OnButtonEvent 0x754034), version
 * byte @27, IMU/touch area beyond. */
static inline void IHS_HIDReportSDLPackWire(uint8_t *dest, size_t len,
                                            const IHS_HIDStateSDL *state) {
    memset(dest, 0, len);
    memcpy(dest, state->axes, sizeof(state->axes));
    if (len >= 18) {
        dest[16] = (uint8_t) (state->buttons & 0xff);
        dest[17] = (uint8_t) ((state->buttons >> 8) & 0xff);
    }
    if (len >= 28) {
        dest[27] = 3;
    }
}

IHS_HIDDevice *IHS_HIDDeviceSDLCreate(IHS_HIDProvider *provider, SDL_Gamepad *controller, bool managed);

bool IHS_HIDDeviceIsSDL(const IHS_HIDDevice *device);

int IHS_HIDDeviceSDLWrite(IHS_HIDDevice *device, const uint8_t *data, size_t dataLen);

int IHS_HIDDeviceSDLGetFeatureReport(IHS_HIDDevice *device, const uint8_t *reportNumber, size_t reportNumberLen,
                                     IHS_Buffer *dest, size_t length);

IHS_HIDManagedDevice *IHS_HIDManagerDeviceByJoystickID(IHS_HIDManager *manager, SDL_JoystickID joystickId);

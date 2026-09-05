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
#define IHS_HID_SDL_WIRE_REPORT_MAX 256U

static inline size_t IHS_HIDDeviceSDLWireReportLength(const IHS_HIDManagedDevice *managed) {
    size_t requested = managed != NULL ? managed->reportHolder.reportLength : 0;
    if (requested < IHS_HID_SDL_BASE_REPORT_LEN ||
        requested > IHS_HID_SDL_WIRE_REPORT_MAX) {
        return IHS_HID_SDL_BASE_REPORT_LEN;
    }
    return requested;
}

static inline void IHS_HIDReportSDLPackWire(uint8_t *dest, size_t len,
                                            const IHS_HIDStateSDL *state) {
    memset(dest, 0, len);
    memcpy(dest, state, IHS_HID_SDL_BASE_REPORT_LEN);
}

IHS_HIDDevice *IHS_HIDDeviceSDLCreate(IHS_HIDProvider *provider, SDL_Gamepad *controller, bool managed);

bool IHS_HIDDeviceIsSDL(const IHS_HIDDevice *device);

int IHS_HIDDeviceSDLWrite(IHS_HIDDevice *device, const uint8_t *data, size_t dataLen);

int IHS_HIDDeviceSDLGetFeatureReport(IHS_HIDDevice *device, const uint8_t *reportNumber, size_t reportNumberLen,
                                     IHS_Buffer *dest, size_t length);

IHS_HIDManagedDevice *IHS_HIDManagerDeviceByJoystickID(IHS_HIDManager *manager, SDL_JoystickID joystickId);

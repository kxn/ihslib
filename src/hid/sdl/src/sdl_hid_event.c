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

#include "ihslib/hid/sdl.h"

#include "hid/manager.h"

#include <stdlib.h> /* SDL2 pulled this in transitively; SDL3 does not */

#include "sdl_hid_common.h"
#include "session/session_pri.h"

static bool HandleRemoveEvent(IHS_HIDManager *manager, const SDL_GamepadDeviceEvent *event);

static bool HandleCButtonEvent(IHS_HIDManager *manager, const SDL_GamepadButtonEvent *event);

static bool HandleCAxisEvent(IHS_HIDManager *manager, const SDL_GamepadAxisEvent *event);

static bool HandleSensorEvent(IHS_HIDManager *manager, const SDL_GamepadSensorEvent *event);

bool IHS_HIDFlushSDLGameControllers(IHS_Session *session) {
    /* Official-client semantics: input reports are masked deltas against the
     * previous flushed state, batched once per frame. full_report is never
     * sent on this path (set_full_report has zero call sites in the official
     * client); periodic full state is the heartbeat's job. */
    IHS_HIDManager *manager = session->hidManager;
    bool queued = false;
    size_t count;
    IHS_HIDManagedDevice **snapshot = IHS_HIDManagerSnapshotOpenDevices(manager, &count);
    for (size_t i = 0; i < count; ++i) {
        IHS_HIDManagedDevice *managed = snapshot[i];
        if (!IHS_HIDDeviceIsSDL(managed->device)) {
            continue;
        }
        if (managed->reportHolder.reportLength == 0) {
            continue;
        }
        IHS_HIDDeviceLock(managed->device);
        IHS_HIDDeviceSDL *device = (IHS_HIDDeviceSDL *) managed->device;
        if (memcmp(&device->states.previous, &device->states.current,
                   sizeof(IHS_HIDStateSDL)) == 0) {
            IHS_HIDDeviceUnlock(managed->device);
            continue;
        }
        IHS_HIDDeviceReportAddDelta((IHS_HIDDevice *) device,
                                    (const uint8_t *) &device->states.previous,
                                    (const uint8_t *) &device->states.current, 48);
        device->lastSubmitted = device->states.current;
        device->lastSubmittedSeq++;
        device->states.previous = device->states.current;
        IHS_HIDDeviceUnlock(managed->device);
        queued = true;
    }
    free(snapshot);
    if (!queued) {
        return false;
    }
    return IHS_SessionHIDSendReport(session);
}

bool IHS_HIDRefreshSDLGameControllers(IHS_Session *session) {
    IHS_HIDManager *manager = session->hidManager;
    bool queued = false;
    size_t count;
    IHS_HIDManagedDevice **snapshot = IHS_HIDManagerSnapshotOpenDevices(manager, &count);
    for (size_t i = 0; i < count; ++i) {
        IHS_HIDManagedDevice *managed = snapshot[i];
        if (!IHS_HIDDeviceIsSDL(managed->device)) {
            continue;
        }
        /* Only devices whose host started input reports carry a wire report length.
         * AddFullForced asserts reportLength >= len, and reporting state the host never
         * subscribed to is wrong regardless. */
        if (managed->reportHolder.reportLength == 0) {
            continue;
        }
        IHS_HIDDeviceLock(managed->device);
        IHS_HIDDeviceSDL *device = (IHS_HIDDeviceSDL *) managed->device;
        /* Resync state rides a full-mask delta: the official client never sends
         * the full_report field (set_full_report has zero call sites). */
        IHS_HIDDeviceReportAddForcedFullMaskDelta((IHS_HIDDevice *) device,
                                                  (const uint8_t *) &device->states.current, 48);
        device->lastSubmitted = device->states.current;
        device->lastSubmittedSeq++;
        device->states.previous = device->states.current;
        IHS_HIDDeviceUnlock(managed->device);
        queued = true;
    }
    free(snapshot);
    if (!queued) {
        return false;
    }
    return IHS_SessionHIDSendReport(session);
}

bool IHS_HIDSDLGetLastSubmittedReport(IHS_Session *session, IHS_HIDSDLLastSubmitted *out) {
    if (session == NULL || session->hidManager == NULL || out == NULL) {
        return false;
    }
    size_t count;
    IHS_HIDManagedDevice **snapshot = IHS_HIDManagerSnapshotOpenDevices(session->hidManager,
                                                                       &count);
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        IHS_HIDManagedDevice *managed = snapshot[i];
        if (!IHS_HIDDeviceIsSDL(managed->device)) {
            continue;
        }
        IHS_HIDDeviceSDL *device = (IHS_HIDDeviceSDL *) managed->device;
        IHS_HIDDeviceLock(managed->device);
        if (device->lastSubmittedSeq > 0) {
            memcpy(out->axes, device->lastSubmitted.axes, sizeof(out->axes));
            out->buttons = device->lastSubmitted.buttons;
            out->flags = device->lastSubmitted.flags;
            out->seq = device->lastSubmittedSeq;
            found = true;
        }
        IHS_HIDDeviceUnlock(managed->device);
        if (found) {
            break;
        }
    }
    free(snapshot);
    return found;
}

bool IHS_HIDHandleSDLEvent(IHS_Session *session, const SDL_Event *event) {
    switch (event->type) {
        case SDL_EVENT_GAMEPAD_ADDED: {
            IHS_SessionHIDNotifyDeviceChange(session);
            return true;
        }
        case SDL_EVENT_GAMEPAD_REMOVED: {
            bool changed = HandleRemoveEvent(session->hidManager, &event->gdevice);
            IHS_SessionHIDNotifyDeviceChange(session);
            return changed;
        }
        /* Events update only the canonical controller state. The caller snapshots
         * that state once per pump; no on-wire delta chain is accumulated here. */
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return HandleCButtonEvent(session->hidManager, &event->gbutton);
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            return HandleCAxisEvent(session->hidManager, &event->gaxis);
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            return HandleSensorEvent(session->hidManager, &event->gsensor);
    }
    return false;
}

bool IHS_HIDResetSDLGameControllers(IHS_Session *session) {
    IHS_HIDManager *manager = session->hidManager;
    bool changed = false;
    size_t count;
    IHS_HIDManagedDevice **snapshot = IHS_HIDManagerSnapshotOpenDevices(manager, &count);
    for (size_t i = 0; i < count; ++i) {
        IHS_HIDManagedDevice *managed = snapshot[i];
        if (!IHS_HIDDeviceIsSDL(managed->device)) {
            continue;
        }
        IHS_HIDDeviceSDL *device = (IHS_HIDDeviceSDL *) managed->device;
        IHS_HIDDeviceLock(managed->device);
        if (IHS_HIDReportSDLClear(&device->states.current)) {
            changed = true;
            if (managed->reportHolder.reportLength > 0) {
                /* Neutral-state resync also rides a full-mask delta. */
                IHS_HIDDeviceReportAddForcedFullMaskDelta(managed->device,
                                                          (const uint8_t *) &device->states.current, 48);
            }
            device->states.previous = device->states.current;
        }
        IHS_HIDDeviceUnlock(managed->device);
    }
    free(snapshot);
    if (changed) {
        IHS_SessionHIDSendReport(session);
    }
    return true;
}

static bool HandleRemoveEvent(IHS_HIDManager *manager, const SDL_GamepadDeviceEvent *event) {
    IHS_HIDManagedDevice *managed = IHS_HIDManagerDeviceByJoystickID(manager, event->which);
    if (managed == NULL) {
        return false;
    }
    IHS_HIDManagedDeviceClose(managed);
    return true;
}

static bool HandleCButtonEvent(IHS_HIDManager *manager, const SDL_GamepadButtonEvent *event) {
    IHS_HIDManagedDevice *managed = IHS_HIDManagerDeviceByJoystickID(manager, event->which);
    if (managed == NULL) {
        return false;
    }
    IHS_HIDDeviceSDL *device = (IHS_HIDDeviceSDL *) managed->device;
    assert(device != NULL);
    IHS_HIDDeviceLock(managed->device);
    bool changed = IHS_HIDReportSDLSetButton(&device->states.current, event->button,
                                             event->down);
    IHS_HIDDeviceUnlock(managed->device);
    return changed;
}

static bool HandleCAxisEvent(IHS_HIDManager *manager, const SDL_GamepadAxisEvent *event) {
    IHS_HIDManagedDevice *managed = IHS_HIDManagerDeviceByJoystickID(manager, event->which);
    if (managed == NULL) {
        return false;
    }
    IHS_HIDDeviceSDL *device = (IHS_HIDDeviceSDL *) managed->device;
    assert(device != NULL);
    IHS_HIDDeviceLock(managed->device);
    bool changed = IHS_HIDReportSDLSetAxis(&device->states.current, event->axis, event->value);
    IHS_HIDDeviceUnlock(managed->device);
    return changed;
}

static bool HandleSensorEvent(IHS_HIDManager *manager, const SDL_GamepadSensorEvent *event) {
    IHS_HIDManagedDevice *managed = IHS_HIDManagerDeviceByJoystickID(manager, event->which);
    if (managed == NULL) {
        return false;
    }
    IHS_HIDDeviceSDL *device = (IHS_HIDDeviceSDL *) managed->device;
    assert(device != NULL);
    IHS_HIDDeviceLock(managed->device);
    bool changed = false;
    if (event->sensor == SDL_SENSOR_ACCEL) {
        changed = IHS_HIDReportSDLSetAccel(&device->states.current, event->data);
    } else if (event->sensor == SDL_SENSOR_GYRO) {
        changed = IHS_HIDReportSDLSetGyro(&device->states.current, event->data);
    }
    IHS_HIDDeviceUnlock(managed->device);
    return changed;
}

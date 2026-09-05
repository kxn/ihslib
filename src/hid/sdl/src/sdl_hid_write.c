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

#include "sdl_hid_common.h"
#include "session/session_pri.h"

typedef enum WriteCommandType {
    COMMAND_RUMBLE = 0x01,
    COMMAND_SET_LED = 0x05,
    COMMAND_RUMBLE_TRIGGERS = 0x06,
    COMMAND_SET_SENSOR_ENABLED = 0x08,
    COMMAND_SET_REQUESTED_REPORT_VERSION = 0x09,
    COMMAND_SET_PS5_RUMBLE = 0x0a,
    COMMAND_SET_PLAYER_INDEX = 0x0b,
} WriteCommandType;

typedef struct __attribute__((__packed__)) {
    WriteCommandType type: 8;
    uint16_t lowFreq: 16;
    uint16_t highFreq: 16;
    uint32_t durationMs: 32;
} RumbleCommand;

_Static_assert(sizeof(RumbleCommand) * 8 == 72, "");

typedef struct __attribute__((__packed__)) {
    WriteCommandType type: 8;
    uint8_t r: 8;
    uint8_t g: 8;
    uint8_t b: 8;
} LEDCommand;

_Static_assert(sizeof(LEDCommand) * 8 == 32, "");

typedef struct __attribute__((__packed__)) {
    WriteCommandType type: 8;
    uint8_t value: 8;
} ByteCommand;

_Static_assert(sizeof(ByteCommand) * 8 == 16, "");

typedef union __attribute__((__packed__))  WriteCommand {
    WriteCommandType type: 8;
    RumbleCommand rumble;
    LEDCommand led;
    ByteCommand byte;
} WriteCommand;

static void HandleRumble(IHS_HIDDeviceSDL *sdl, const RumbleCommand *rumble);

static void HandleRumbleTriggers(IHS_HIDDeviceSDL *sdl, const RumbleCommand *rumble);

static void HandleSetLED(IHS_HIDDeviceSDL *sdl, const LEDCommand *led);

static void HandleSetSensorEnabled(IHS_HIDDeviceSDL *sdl, const ByteCommand *byte);

static void HandleSetPS5Rumble(IHS_HIDDeviceSDL *sdl, const ByteCommand *byte);

static void HandleSetPlayerIndex(IHS_HIDDeviceSDL *sdl, const ByteCommand *byte);

static size_t WriteCommandLength(uint8_t type) {
    switch (type) {
        case COMMAND_RUMBLE:
        case COMMAND_RUMBLE_TRIGGERS:
            return sizeof(RumbleCommand);
        case COMMAND_SET_LED:
            return sizeof(LEDCommand);
        case COMMAND_SET_SENSOR_ENABLED:
        case COMMAND_SET_REQUESTED_REPORT_VERSION:
        case COMMAND_SET_PS5_RUMBLE:
        case COMMAND_SET_PLAYER_INDEX:
            return sizeof(ByteCommand);
        default:
            return 0;
    }
}

/* Called on the session receive thread: queue ONLY. SDL gamepad calls here
 * deadlock against SDL_PollEvent on the media thread; the commands are
 * applied by IHS_HIDDeviceSDLApplyPendingWrites on the flush thread, which
 * already calls SDL safely at 125 Hz. */
int IHS_HIDDeviceSDLWrite(IHS_HIDDevice *device, const uint8_t *data, size_t dataLen) {
    IHS_HIDDeviceSDL *sdl = (IHS_HIDDeviceSDL *) device;
    if (dataLen < sizeof(uint8_t)) {
        return -1;
    }
    size_t commandLen = WriteCommandLength(data[0]);
    if (commandLen == 0 || dataLen < commandLen) {
        return -1;
    }
    IHS_HIDDeviceLock(device);
    if (sdl->pendingWrites.size + commandLen > 512) {
        IHS_HIDDeviceUnlock(device);
        IHS_HIDDeviceLog(device, IHS_LogLevelWarn, "HID.SDL",
                         "Write queue overflow, dropping command %u", data[0]);
        return 0; /* accepted-and-dropped: rumble is best-effort */
    }
    IHS_BufferAppendMem(&sdl->pendingWrites, data, commandLen);
    IHS_HIDDeviceUnlock(device);
    return 0;
}

/* Called with the device lock HELD (flush thread): apply queued commands. */
void IHS_HIDDeviceSDLApplyPendingWrites(IHS_HIDDeviceSDL *sdl) {
    while (sdl->pendingWrites.size >= sizeof(uint8_t)) {
        const uint8_t *data = IHS_BufferPointer(&sdl->pendingWrites);
        size_t commandLen = WriteCommandLength(data[0]);
        if (commandLen == 0 || sdl->pendingWrites.size < commandLen) {
            sdl->pendingWrites.size = 0; /* corrupt tail: drop everything */
            return;
        }
        const WriteCommand *command = (const WriteCommand *) data;
        switch (command->type) {
            case COMMAND_RUMBLE: {
                HandleRumble(sdl, &command->rumble);
                break;
            }
            case COMMAND_SET_LED: {
                HandleSetLED(sdl, &command->led);
                break;
            }
            case COMMAND_RUMBLE_TRIGGERS: {
                HandleRumbleTriggers(sdl, &command->rumble);
                break;
            }
            case COMMAND_SET_SENSOR_ENABLED: {
                HandleSetSensorEnabled(sdl, &command->byte);
                break;
            }
            case COMMAND_SET_REQUESTED_REPORT_VERSION: {
                IHS_HIDReportSDLSetRequestedReportVersion(&sdl->states.current, command->byte.value);
                break;
            }
            case COMMAND_SET_PS5_RUMBLE: {
                HandleSetPS5Rumble(sdl, &command->byte);
                break;
            }
            case COMMAND_SET_PLAYER_INDEX: {
                HandleSetPlayerIndex(sdl, &command->byte);
                break;
            }
            default:
                break;
        }
        IHS_BufferOffsetBy(&sdl->pendingWrites, (int) commandLen);
    }
}

void HandleRumble(IHS_HIDDeviceSDL *sdl, const RumbleCommand *rumble) {
    IHS_HIDDeviceLog(&sdl->base, IHS_LogLevelVerbose, "HID.SDL", "Rumble(dur=%u, lo=%u, hi=%u)", rumble->durationMs,
                     rumble->lowFreq, rumble->highFreq);
    SDL_RumbleGamepad(sdl->controller, rumble->lowFreq, rumble->highFreq, rumble->durationMs);
}

static void HandleRumbleTriggers(IHS_HIDDeviceSDL *sdl, const RumbleCommand *rumble) {
    IHS_HIDDeviceLog(&sdl->base, IHS_LogLevelInfo, "HID.SDL", "RumbleTriggers(dur=%u, lo=%u, hi=%u)",
                     rumble->durationMs, rumble->lowFreq, rumble->highFreq);
    SDL_RumbleGamepadTriggers(sdl->controller, rumble->lowFreq, rumble->highFreq, rumble->durationMs);
}

static void HandleSetLED(IHS_HIDDeviceSDL *sdl, const LEDCommand *led) {
    SDL_SetGamepadLED(sdl->controller, led->r, led->g, led->b);
}

static void HandleSetSensorEnabled(IHS_HIDDeviceSDL *sdl, const ByteCommand *byte) {
    SDL_SetGamepadSensorEnabled(sdl->controller, SDL_SENSOR_ACCEL, byte->value);
    SDL_SetGamepadSensorEnabled(sdl->controller, SDL_SENSOR_GYRO, byte->value);
}

static void HandleSetPS5Rumble(IHS_HIDDeviceSDL *sdl, const ByteCommand *byte) {
    (void) sdl;
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, byte->value ? "1" : "0");
}

static void HandleSetPlayerIndex(IHS_HIDDeviceSDL *sdl, const ByteCommand *byte) {
    SDL_SetGamepadPlayerIndex(sdl->controller, byte->value);
    sdl->playerIndex = byte->value;
}
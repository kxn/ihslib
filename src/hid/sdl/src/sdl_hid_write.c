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
    COMMAND_SEND_EFFECT = 0x0d,
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

/* Official CHIDDeviceSDLGamepad::Write accepts a 69-byte effect envelope:
 * 0x0d, little-endian u32 length, 64 payload bytes. See OFFICIAL_INPUT_RE §22.
 * Validate before queueing; never trust the embedded length at drain time. */
typedef struct __attribute__((__packed__)) {
    uint8_t type;
    uint8_t length[4];
    uint8_t data[64];
} EffectCommand;
_Static_assert(sizeof(EffectCommand) == 69, "effect wire size");

static uint32_t EffectLength(const EffectCommand *effect) {
    return (uint32_t)effect->length[0] | (uint32_t)effect->length[1] << 8 |
           (uint32_t)effect->length[2] << 16 | (uint32_t)effect->length[3] << 24;
}

typedef union __attribute__((__packed__))  WriteCommand {
    WriteCommandType type: 8;
    RumbleCommand rumble;
    LEDCommand led;
    ByteCommand byte;
    EffectCommand effect;
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
        case COMMAND_SEND_EFFECT:
            return sizeof(EffectCommand);
        default:
            return 0;
    }
}

/* Called on the session receive thread: queue ONLY. SDL gamepad calls here
 * deadlock against SDL_PollEvent on the media thread; the commands are
 * applied by IHS_HIDDeviceSDLApplyPendingWrites on the media thread, alongside SDL event processing. */
int IHS_HIDDeviceSDLWrite(IHS_HIDDevice *device, const uint8_t *data, size_t dataLen,
                         uint64_t received) {
    IHS_HIDDeviceSDL *sdl = (IHS_HIDDeviceSDL *) device;
    if (!data || dataLen < sizeof(uint8_t)) {
        return -1;
    }
    size_t commandLen = WriteCommandLength(data[0]);
    if (commandLen == 0 || dataLen < commandLen) {
        return -1;
    }
    if (data[0] == COMMAND_SEND_EFFECT &&
        (dataLen != sizeof(EffectCommand) ||
         EffectLength((const EffectCommand *)data) > 64)) {
        return -1;
    }
    /* The generic entry captures received before DeviceBegin waits for the
     * owner-held lock, retaining drain/activation latency in the evidence. */
    IHS_HIDDeviceLock(device);
    if (sdl->pendingWrites.size + sizeof(uint64_t) + commandLen > 512) {
        ++sdl->outputQueueDropped;
        IHS_HIDDeviceUnlock(device);
        return -1; /* No command was accepted, including configuration writes. */
    }
    IHS_BufferAppendMem(&sdl->pendingWrites, (const uint8_t *)&received, sizeof(received));
    IHS_BufferAppendMem(&sdl->pendingWrites, data, commandLen);
    ++sdl->outputQueued;
    IHS_HIDDeviceUnlock(device);
    return 0;
}

/* Called with the device lock HELD (media thread): apply queued commands. */
void IHS_HIDDeviceSDLApplyPendingWrites(IHS_HIDDeviceSDL *sdl) {
    while (sdl->pendingWrites.size >= sizeof(uint64_t) + 1) {
        const uint8_t *record = IHS_BufferPointer(&sdl->pendingWrites);
        uint64_t received;
        memcpy(&received, record, sizeof(received));
        const uint8_t *data = record + sizeof(received);
        size_t commandLen = WriteCommandLength(data[0]);
        if (commandLen == 0 || sdl->pendingWrites.size < sizeof(received) + commandLen) {
            IHS_BufferClear(&sdl->pendingWrites, false); /* corrupt tail */
            return;
        }
        const WriteCommand *command = (const WriteCommand *) data;
        ++sdl->outputApplied;
        if (sdl->output && (data[0] == COMMAND_RUMBLE || data[0] == COMMAND_SEND_EFFECT)) {
            if (sdl->output(sdl->instanceId, data, commandLen, received,
                            sdl->outputContext) < 0)
                ++sdl->outputRejected;
            IHS_BufferOffsetBy(&sdl->pendingWrites, (int)(sizeof(received) + commandLen));
            continue;
        }
        switch (command->type) {
            case COMMAND_SEND_EFFECT: {
                uint32_t length = EffectLength(&command->effect);
                if (length <= sizeof(command->effect.data)) {
                    if (!SDL_SendGamepadEffect(sdl->controller, command->effect.data, (int)length))
                        ++sdl->outputRejected;
                }
                break;
            }
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
        IHS_BufferOffsetBy(&sdl->pendingWrites, (int)(sizeof(received) + commandLen));
    }
    IHS_BufferClear(&sdl->pendingWrites, false);
}

void HandleRumble(IHS_HIDDeviceSDL *sdl, const RumbleCommand *rumble) {
    /* Applied with the device lock held. Per-command logging would acquire
     * the session lock (device -> session -> timer -> device inversion). */
    SDL_RumbleGamepad(sdl->controller, rumble->lowFreq, rumble->highFreq, rumble->durationMs);
}

static void HandleRumbleTriggers(IHS_HIDDeviceSDL *sdl, const RumbleCommand *rumble) {
    SDL_RumbleGamepadTriggers(sdl->controller, rumble->lowFreq, rumble->highFreq, rumble->durationMs);
}

static void HandleSetLED(IHS_HIDDeviceSDL *sdl, const LEDCommand *led) {
    SDL_SetGamepadLED(sdl->controller, led->r, led->g, led->b);
}

static void HandleSetSensorEnabled(IHS_HIDDeviceSDL *sdl, const ByteCommand *byte) {
    /* The SDL Switch backend stubs this call; the native sensor source is
     * started by the platform. Record the host request for diagnostics. */
    sdl->sensorEnableRequested = byte->value != 0;
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

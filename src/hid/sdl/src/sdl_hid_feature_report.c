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
#include "sdl_hid_utils.h"
#include "session/session_pri.h"

typedef enum GetFeatureReportCommand {
    GetFeatureReportGetPowerLevel = 0x02,
    GetFeatureReportGetCaps = 0x04,
    GetFeatureReportGetSerial = 0x07,
    GetFeatureReportGetFirmware = 0x0c,
    GetFeatureReportGetSerialSuffix = 0x0e,
    GetFeatureReportGetPowerState = 0x0f,
    GetFeatureReportGetConnection = 0x10,
} GetFeatureReportCommand;

/**
 * @see https://github.com/libsdl-org/SDL/blob/53dea9830964eee8b5c2a7ee0a65d6e268dc78a1/src/joystick/controller_type.h
 */
typedef enum EControllerType {
    k_ControllerTypeNone = -1,
    k_ControllerTypeUnknown = 0,

    // Steam Controllers
    k_ControllerTypeUnknownSteamController = 1,
    k_ControllerTypeSteamController = 2,
    k_ControllerTypeSteamControllerV2 = 3,

    // Other Controllers
    k_ControllerTypeUnknownNonSteamController = 30,
    k_ControllerTypeXBox360Controller = 31,
    k_ControllerTypeXBoxOneController = 32,
    k_ControllerTypePS3Controller = 33,
    k_ControllerTypePS4Controller = 34,
    k_ControllerTypeWiiController = 35,
    k_ControllerTypeAppleController = 36,
    k_ControllerTypeAndroidController = 37,
    k_ControllerTypeSwitchProController = 38,
    k_ControllerTypeSwitchJoyConLeft = 39,
    k_ControllerTypeSwitchJoyConRight = 40,
    k_ControllerTypeSwitchJoyConPair = 41,
    k_ControllerTypeSwitchInputOnlyController = 42,
    k_ControllerTypeMobileTouch = 43,
    k_ControllerTypeXInputSwitchController = 44,  // Client-side only, used to mark Switch-compatible controllers as not supporting Switch controller protocol
    k_ControllerTypePS5Controller = 45,
    k_ControllerTypeXboxEliteController = 46,
    k_ControllerTypeLastController, // Don't add game controllers below this enumeration - this enumeration can change value
} EControllerType;

typedef struct __attribute__((__packed__)) {
    bool valid;
    bool xinput;
    EControllerType controllerType: 32;
    uint32_t playerFlags: 32;
    bool hid;
    uint32_t deviceClass: 32;
    uint32_t capabilities: 32;
    uint8_t driverData;
} DeviceFeatureReport;

_Static_assert(sizeof(DeviceFeatureReport) == 20, "");

bool IsXinputDevice(const SDL_GUID *guid);

bool IsHIDAPIDevice(const SDL_GUID *guid);

/**
 * Map SDL3's SDL_PowerState + battery percentage back onto the SDL2
 * SDL_JoystickPowerLevel byte the Steam host expects on the wire.
 * UNKNOWN/ERROR -> SDL_JOYSTICK_POWER_UNKNOWN (-1 as a byte, i.e. 0xFF).
 */
static uint8_t JoystickPowerLevelByte(SDL_Joystick *joystick) {
    int percent = -1;
    switch (SDL_GetJoystickPowerInfo(joystick, &percent)) {
        case SDL_POWERSTATE_NO_BATTERY:
            return 4; /* SDL_JOYSTICK_POWER_WIRED */
        case SDL_POWERSTATE_ON_BATTERY:
        case SDL_POWERSTATE_CHARGING:
        case SDL_POWERSTATE_CHARGED:
            if (percent < 0) return (uint8_t) -1; /* UNKNOWN */
            if (percent <= 5) return 0;  /* EMPTY */
            if (percent <= 20) return 1; /* LOW */
            if (percent <= 70) return 2; /* MEDIUM */
            return 3;                    /* FULL */
        default:
            return (uint8_t) -1; /* SDL_JOYSTICK_POWER_UNKNOWN */
    }
}

static bool GamepadWireless(SDL_Gamepad *controller) {
#if SDL_MAJOR_VERSION >= 3
    return SDL_GetGamepadConnectionState(controller) == SDL_JOYSTICK_CONNECTION_WIRELESS;
#else
    const SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller));
    return guid.data[0] == 0x05 && guid.data[1] == 0;
#endif
}

int IHS_HIDDeviceSDLGetFeatureReport(IHS_HIDDevice *device, const uint8_t *reportNumber, size_t reportNumberLen,
                                     IHS_Buffer *dest, size_t length) {
    if (reportNumberLen < 1 || length < 1) return -1;
    IHS_HIDDeviceSDL *sdl = (IHS_HIDDeviceSDL *) device;
    uint8_t payload[21] = {reportNumber[0]};
    switch (reportNumber[0]) {
        case GetFeatureReportGetSerialSuffix:
        case GetFeatureReportGetSerial: {
            const bool synthetic = sdl->identity == IHS_HIDSdlIdentityDualSense &&
                SDL_GetGamepadType(sdl->controller) != SDL_GAMEPAD_TYPE_PS5;
            const char *serial = synthetic ? NULL : SDL_GetGamepadSerial(sdl->controller);
            if (serial) {
                const char *comma = strchr(serial, ',');
                if (reportNumber[0] == GetFeatureReportGetSerialSuffix)
                    serial = comma ? comma + 1 : NULL;
                if (serial) {
                    size_t size = strcspn(serial, ",");
                    if (size > sizeof(payload) - 2) size = sizeof(payload) - 2;
                    memcpy(payload + 1, serial, size);
                }
            }
            break;
        }
        case GetFeatureReportGetFirmware: {
            /* Official Generic report: ID, uint16 LE version, zero padding.
             * A Switch-backed Sony profile has no Sony firmware version. */
            uint16_t version = 0;
            if (sdl->identity != IHS_HIDSdlIdentityDualSense ||
                SDL_GetGamepadType(sdl->controller) == SDL_GAMEPAD_TYPE_PS5) {
                version = SDL_GetGamepadFirmwareVersion(sdl->controller);
            }
            payload[1] = (uint8_t)version;
            payload[2] = (uint8_t)(version >> 8);
            break;
        }
        case GetFeatureReportGetPowerLevel: {
            IHS_BufferWriteMem(dest, 0, reportNumber, 1);
            SDL_Joystick *joystick = SDL_GetGamepadJoystick(sdl->controller);
            uint8_t level = JoystickPowerLevelByte(joystick);
            if (length >= 2) IHS_BufferWriteMem(dest, 1, &level, 1);
            return 0;
        }
        case GetFeatureReportGetPowerState: {
            int percent = -1;
            SDL_PowerState state = SDL_GetJoystickPowerInfo(SDL_GetGamepadJoystick(sdl->controller), &percent);
            payload[1] = GamepadWireless(sdl->controller);
            payload[2] = (uint8_t)state;
            payload[3] = (uint8_t)(percent < 0 ? 100 : percent);
            break;
        }
        case GetFeatureReportGetConnection: {
            SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(sdl->controller));
            if (!IsXinputDevice(&guid)) {
                /* Official 0x10 uses the GUID bus first, then SDL's state. */
                payload[1] = guid.data[0] == 0x05 ? 2 : GamepadWireless(sdl->controller) ? 3 : 1;
            }
            break;
        }
        case GetFeatureReportGetCaps: {
            int playerIndex = -1;
            SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(sdl->controller));

            bool xinput = IsXinputDevice(&guid);
            // Initialize so the xinput-true branch doesn't leak uninitialized stack
            // contents through the wire report (DeviceFeatureReport.controllerType below).
            EControllerType controllerType = k_ControllerTypeUnknown;
            if (!xinput) {
                playerIndex = sdl->playerIndex >= 0 ? sdl->playerIndex :
                              SDL_GetGamepadPlayerIndex(sdl->controller);
                switch (SDL_GetGamepadType(sdl->controller)) {
                    case SDL_GAMEPAD_TYPE_XBOX360:
                        controllerType = k_ControllerTypeXBox360Controller;
                        break;
                    case SDL_GAMEPAD_TYPE_XBOXONE:
                        controllerType = k_ControllerTypeXBoxOneController;
                        break;
                    case SDL_GAMEPAD_TYPE_PS3:
                        controllerType = k_ControllerTypePS3Controller;
                        break;
                    case SDL_GAMEPAD_TYPE_PS4:
                        controllerType = k_ControllerTypePS4Controller;
                        break;
                    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
                        controllerType = k_ControllerTypeSwitchProController;
                        break;
                    /* SDL3 dropped SDL_GAMEPAD_TYPE_VIRTUAL; virtual pads now report
                     * as whatever type they emulate, so there is nothing to map. */
                    case SDL_GAMEPAD_TYPE_PS5:
                        controllerType = k_ControllerTypePS5Controller;
                        break;
                    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                        controllerType = k_ControllerTypeSwitchJoyConLeft;
                        break;
                    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
                        controllerType = k_ControllerTypeSwitchJoyConRight;
                        break;
                    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                        controllerType = k_ControllerTypeSwitchJoyConPair;
                        break;
                    default:
#if defined(__SWITCH__)
                        /* devkitPro SDL2 does not expose the built-in pad as
                         * SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO (type=0), but the
                         * host identity pair is 057e:2009. Report the matching
                         * Steam controller type instead of UnknownNonSteam. */
                        controllerType = k_ControllerTypeSwitchProController;
#else
                        controllerType = k_ControllerTypeUnknownNonSteamController;
#endif
                        break;
                }
            }
            if (sdl->identity == IHS_HIDSdlIdentityDualSense) {
                controllerType = k_ControllerTypePS5Controller;
                xinput = false;
            }
            DeviceFeatureReport report = {
                    .valid = true,
                    .xinput = xinput,
                    /* Official Generic 0x04 packs the index in the low three
                     * bits. A missing index (-1) encodes as 7, not all flags. */
                    .playerFlags = 0x08u | ((uint32_t)playerIndex & 0x07u),
                    .controllerType = controllerType,
                    .hid = IsHIDAPIDevice(&guid),
            };
            if (controllerType == k_ControllerTypePS5Controller) {
                /* The Windows Generic driver replaces its enumeration caps
                 * with this nonzero word when consuming the 0x04 response. */
                report.deviceClass = 1; /* standard gamepad */
                report.capabilities = IHS_HID_CAP_DUALSENSE_PROFILE;
                report.driverData = guid.data[15];
            }
            IHS_HIDDeviceLog(device, IHS_LogLevelDebug, "HID.SDL",
                             "FeatureCaps(xinput=%u, hid=%u, type=%d, player=%d, guid14=0x%02x)",
                             report.xinput ? 1U : 0U, report.hid ? 1U : 0U,
                             (int)report.controllerType, playerIndex,
                             (unsigned)guid.data[14]);
            memcpy(payload + 1, &report, sizeof(report));
            break;
        }
        default: {
            // Official Generic fallback returns one zero byte.
            IHS_BufferFillMem(dest, 0, 0, 1);
            return 0;
        }
    }
    IHS_BufferWriteMem(dest, 0, payload, length < sizeof(payload) ? length : sizeof(payload));
    return 0;
}

bool IsXinputDevice(const SDL_GUID *guid) {
    return guid->data[14] == 'x';
}

bool IsHIDAPIDevice(const SDL_GUID *guid) {
    return guid->data[14] == 'h';
}

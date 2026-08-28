#pragma once

#include <SDL.h>
#include <SDL_gamecontroller.h>
#include <SDL_joystick.h>
#include <SDL_power.h>
#include <SDL_sensor.h>

typedef SDL_GameController SDL_Gamepad;
typedef SDL_GameControllerAxis SDL_GamepadAxis;
typedef SDL_GameControllerButton SDL_GamepadButton;
typedef SDL_ControllerDeviceEvent SDL_GamepadDeviceEvent;
typedef SDL_ControllerButtonEvent SDL_GamepadButtonEvent;
typedef SDL_ControllerAxisEvent SDL_GamepadAxisEvent;
typedef SDL_ControllerSensorEvent SDL_GamepadSensorEvent;

#define SDL_INIT_GAMEPAD SDL_INIT_GAMECONTROLLER

#define SDL_EVENT_GAMEPAD_ADDED SDL_CONTROLLERDEVICEADDED
#define SDL_EVENT_GAMEPAD_REMOVED SDL_CONTROLLERDEVICEREMOVED
#define SDL_EVENT_GAMEPAD_BUTTON_DOWN SDL_CONTROLLERBUTTONDOWN
#define SDL_EVENT_GAMEPAD_BUTTON_UP SDL_CONTROLLERBUTTONUP
#define SDL_EVENT_GAMEPAD_AXIS_MOTION SDL_CONTROLLERAXISMOTION
#define SDL_EVENT_GAMEPAD_SENSOR_UPDATE SDL_CONTROLLERSENSORUPDATE

#define gdevice cdevice
#define gbutton cbutton
#define gaxis caxis
#define gsensor csensor
#define down state

#define SDL_GAMEPAD_TYPE_XBOX360 SDL_CONTROLLER_TYPE_XBOX360
#define SDL_GAMEPAD_TYPE_XBOXONE SDL_CONTROLLER_TYPE_XBOXONE
#define SDL_GAMEPAD_TYPE_PS3 SDL_CONTROLLER_TYPE_PS3
#define SDL_GAMEPAD_TYPE_PS4 SDL_CONTROLLER_TYPE_PS4
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO
#define SDL_GAMEPAD_TYPE_PS5 SDL_CONTROLLER_TYPE_PS5
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR

#define SDL_GetGamepadJoystick SDL_GameControllerGetJoystick
#define SDL_GetGamepadSerial SDL_GameControllerGetSerial
#define SDL_GetGamepadPlayerIndex SDL_GameControllerGetPlayerIndex
#define SDL_SetGamepadPlayerIndex SDL_GameControllerSetPlayerIndex
#define SDL_GetGamepadType SDL_GameControllerGetType
#define SDL_CloseGamepad SDL_GameControllerClose
#define SDL_RumbleGamepad SDL_GameControllerRumble
#define SDL_RumbleGamepadTriggers SDL_GameControllerRumbleTriggers
#define SDL_SetGamepadLED SDL_GameControllerSetLED
#define SDL_SetGamepadSensorEnabled SDL_GameControllerSetSensorEnabled
#define SDL_GetJoystickID SDL_JoystickInstanceID
#define SDL_GetJoystickName SDL_JoystickName
#define SDL_GetJoystickGUID SDL_JoystickGetGUID
#define SDL_GetGamepadFromID SDL_GameControllerFromInstanceID
#define SDL_Swap16LE SDL_SwapLE16

#ifndef SDL_HINT_JOYSTICK_ENHANCED_REPORTS
#define SDL_HINT_JOYSTICK_ENHANCED_REPORTS "SDL_JOYSTICK_ENHANCED_REPORTS"
#endif

static inline int IHS_SDL2DeviceIndexFromInstance(SDL_JoystickID instance_id) {
    int count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++) {
        if (SDL_JoystickGetDeviceInstanceID(i) == instance_id) {
            return i;
        }
    }
    return -1;
}

static inline SDL_JoystickID *IHS_SDL2GetJoysticks(int *count) {
    int n = SDL_NumJoysticks();
    if (count != NULL) {
        *count = n;
    }
    if (n <= 0) {
        return NULL;
    }
    SDL_JoystickID *ids = (SDL_JoystickID *)SDL_malloc(sizeof(SDL_JoystickID) * (size_t)n);
    if (ids == NULL) {
        if (count != NULL) {
            *count = 0;
        }
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        ids[i] = SDL_JoystickGetDeviceInstanceID(i);
    }
    return ids;
}

static inline SDL_bool IHS_SDL2IsGamepad(SDL_JoystickID instance_id) {
    int device_index = IHS_SDL2DeviceIndexFromInstance(instance_id);
    return device_index >= 0 ? SDL_IsGameController(device_index) : SDL_FALSE;
}

static inline SDL_Gamepad *IHS_SDL2OpenGamepad(SDL_JoystickID instance_id) {
    int device_index = IHS_SDL2DeviceIndexFromInstance(instance_id);
    return device_index >= 0 ? SDL_GameControllerOpen(device_index) : NULL;
}

static inline const char *IHS_SDL2GetJoystickNameForID(SDL_JoystickID instance_id) {
    int device_index = IHS_SDL2DeviceIndexFromInstance(instance_id);
    return device_index >= 0 ? SDL_JoystickNameForIndex(device_index) : NULL;
}

static inline Uint16 IHS_SDL2GetJoystickVendorForID(SDL_JoystickID instance_id) {
    int device_index = IHS_SDL2DeviceIndexFromInstance(instance_id);
    return device_index >= 0 ? SDL_JoystickGetDeviceVendor(device_index) : 0;
}

static inline Uint16 IHS_SDL2GetJoystickProductForID(SDL_JoystickID instance_id) {
    int device_index = IHS_SDL2DeviceIndexFromInstance(instance_id);
    return device_index >= 0 ? SDL_JoystickGetDeviceProduct(device_index) : 0;
}

static inline Uint16 IHS_SDL2GetJoystickProductVersionForID(SDL_JoystickID instance_id) {
    int device_index = IHS_SDL2DeviceIndexFromInstance(instance_id);
    return device_index >= 0 ? SDL_JoystickGetDeviceProductVersion(device_index) : 0;
}

static inline SDL_PowerState IHS_SDL2GetJoystickPowerInfo(SDL_Joystick *joystick, int *percent) {
    if (percent != NULL) {
        *percent = -1;
    }
    switch (SDL_JoystickCurrentPowerLevel(joystick)) {
        case SDL_JOYSTICK_POWER_EMPTY:
        case SDL_JOYSTICK_POWER_LOW:
        case SDL_JOYSTICK_POWER_MEDIUM:
        case SDL_JOYSTICK_POWER_FULL:
            return SDL_POWERSTATE_ON_BATTERY;
        case SDL_JOYSTICK_POWER_WIRED:
            return SDL_POWERSTATE_NO_BATTERY;
        default:
            return SDL_POWERSTATE_UNKNOWN;
    }
}

#define SDL_GetJoysticks IHS_SDL2GetJoysticks
#define SDL_IsGamepad IHS_SDL2IsGamepad
#define SDL_OpenGamepad IHS_SDL2OpenGamepad
#define SDL_GetJoystickNameForID IHS_SDL2GetJoystickNameForID
#define SDL_GetJoystickVendorForID IHS_SDL2GetJoystickVendorForID
#define SDL_GetJoystickProductForID IHS_SDL2GetJoystickProductForID
#define SDL_GetJoystickProductVersionForID IHS_SDL2GetJoystickProductVersionForID
#define SDL_GetJoystickPowerInfo IHS_SDL2GetJoystickPowerInfo

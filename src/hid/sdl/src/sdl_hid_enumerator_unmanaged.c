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
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h> /* snprintf; SDL2 pulled it in transitively, SDL3 does not */

#include "sdl_hid_common.h"
#include "sdl_hid_utils.h"
#include "sdl_hid_enumerators.h"

#include "ihslib/enumeration.h"
#include "ihslib/hid.h"
#include "ihslib/hid/sdl.h"

typedef struct UnmanagedEnumerationAllocArg {
    const IHS_HIDProviderSDLDeviceList *list;
    void *listContext;
    IHS_HIDSdlIdentity identity;
} UnmanagedEnumerationAllocArg;

typedef struct UnmanagedEnumeration {
    GameControllerEnumeration base;
    const IHS_HIDProviderSDLDeviceList *list;
    void *listContext;
    IHS_HIDSdlIdentity identity;
} UnmanagedEnumeration;

static IHS_Enumeration *EnumerationAlloc(const IHS_EnumerationClass *cls, void *arg);

static size_t EnumerationCount(const IHS_Enumeration *enumeration);

static void EnumerationReset(IHS_Enumeration *enumeration);

static bool EnumerationEnded(const IHS_Enumeration *enumeration);

static void *EnumerationGet(const IHS_Enumeration *enumeration);

static void *EnumerationNext(IHS_Enumeration *enumeration);

static bool EnumerationGetInfo(IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info);

const static IHS_HIDDeviceSDLEnumerationClass UnmanagedEnumerationClass = {
        .base = {
                .alloc = EnumerationAlloc,
                .free = (void (*)(IHS_Enumeration *)) free,
                .count = EnumerationCount,
                .reset = EnumerationReset,
                .ended = EnumerationEnded,
                .get = EnumerationGet,
                .next = EnumerationNext,
        },
        .getInfo = EnumerationGetInfo,
};

IHS_Enumeration *IHS_HIDDeviceSDLEnumerateUnmanaged(const IHS_HIDProviderSDLDeviceList *list,
                                                void *listContext, IHS_HIDSdlIdentity identity) {
    assert(list != NULL);
    assert(list->count != NULL);
    assert(list->index != NULL);
    assert(list->instanceId != NULL);
    assert(list->controller != NULL);
    UnmanagedEnumerationAllocArg arg = {.list = list, .listContext = listContext, .identity = identity};
    return IHS_EnumerationCreate((const IHS_EnumerationClass *) &UnmanagedEnumerationClass, &arg);
}

static IHS_Enumeration *EnumerationAlloc(const IHS_EnumerationClass *cls, void *arg) {
    UnmanagedEnumeration *enumeration = calloc(1, sizeof(UnmanagedEnumeration));
    enumeration->base.base.cls = cls;
    UnmanagedEnumerationAllocArg *ua = arg;
    enumeration->list = ua->list;
    enumeration->listContext = ua->listContext;
    enumeration->identity = ua->identity;
    EnumerationReset((IHS_Enumeration *) enumeration);
    return (IHS_Enumeration *) enumeration;
}

static size_t EnumerationCount(const IHS_Enumeration *enumeration) {
    const GameControllerEnumeration *gce = (const GameControllerEnumeration *) enumeration;
    return gce->joystickCount;
}

static void EnumerationReset(IHS_Enumeration *enumeration) {
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    UnmanagedEnumeration *ue = (UnmanagedEnumeration *) enumeration;
    gce->joystickIndex = 0;
    gce->joystickCount = ue->list->count(ue->listContext);
}

static bool EnumerationEnded(const IHS_Enumeration *enumeration) {
    const GameControllerEnumeration *gce = (const GameControllerEnumeration *) enumeration;
    return gce->joystickIndex >= gce->joystickCount;
}

static void *EnumerationGet(const IHS_Enumeration *enumeration) {
    const GameControllerEnumeration *gce = (const GameControllerEnumeration *) enumeration;
    if (gce->joystickIndex < gce->joystickCount) {
        return (void *) &gce->joystickIndex;
    }
    return NULL;
}

static void *EnumerationNext(IHS_Enumeration *enumeration) {
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    gce->joystickIndex++;
    return EnumerationGet(enumeration);
}

static bool EnumerationGetInfo(IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info) {
    UnmanagedEnumeration *ue = (UnmanagedEnumeration *) enumeration;
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    SDL_Gamepad *controller = ue->list->controller(gce->joystickIndex, ue->listContext);
    if (controller == NULL) {
        return false;
    }
    SDL_Joystick *joystick = SDL_GetGamepadJoystick(controller);
    SDL_JoystickID instanceId = SDL_GetJoystickID(joystick);
    snprintf(gce->temp.path, 16, "sdl://%u", instanceId);
    const char *name = SDL_GetJoystickName(joystick);
    if (name != NULL) {
        strncpy(gce->temp.product_string, name, 63);
        gce->temp.product_string[63] = '\0';
    } else {
        strcpy(gce->temp.product_string, "Generic Gamepad");
    }
    SDL_GUID guid = SDL_GetJoystickGUID(joystick);
    if (!IHS_HIDDeviceSDLGetJoystickGUIDInfo(&guid, &info->vendor_id, &info->product_id, &info->product_version,
                                             NULL)) {
        info->vendor_id = 0;
        info->product_id = 0;
        info->product_version = 0;
    }
#if defined(__SWITCH__)
    /* devkitPro SDL's built-in Switch pad uses a name-based GUID
     * (000038f8"Switch Cont"), so the standard VID/PID parser above always
     * fails and the host would see 0000:0000. Official clients report a real
     * controller identity together with is_generic_gamepad=true; restore the
     * Nintendo Switch Pro Controller IDs so the host can type the pad while
     * the wire format stays Generic RAW V3. */
    info->vendor_id = 0x057e;
    info->product_id = 0x2009;
    info->product_version = 0x0001;
    strncpy(gce->temp.product_string, "Nintendo Switch Pro Controller", 63);
    gce->temp.product_string[63] = '\0';
#endif
    if (ue->identity == IHS_HIDSdlIdentityDualSense) {
        info->vendor_id = 0x054c;
        info->product_id = 0x0ce6;
        info->product_version = 0x0001;
        strcpy(gce->temp.product_string, "DualSense Wireless Controller");
    }
    info->path = gce->temp.path;
    info->product_string = gce->temp.product_string;
    if (ue->identity != IHS_HIDSdlIdentityDualSense || SDL_GetGamepadType(controller) == SDL_GAMEPAD_TYPE_PS5)
        info->serial_number = SDL_GetGamepadSerial(controller);
    info->native_haptic_pulse = ue->identity != IHS_HIDSdlIdentityDualSense &&
        ue->list->output && ue->list->supportsNativeHaptics &&
        ue->list->supportsNativeHaptics(instanceId, ue->listContext);
    return true;
}

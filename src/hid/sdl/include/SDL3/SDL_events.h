#pragma once

#ifdef IHSLIB_HID_SDL2
#include <SDL.h>
#include "ihslib/hid/sdl/sdl2_compat.h"
#else
#include_next <SDL3/SDL_events.h>
#endif

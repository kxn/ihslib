#pragma once

#ifdef IHSLIB_HID_SDL2
#include <SDL_version.h>
#else
#include_next <SDL3/SDL_version.h>
#endif

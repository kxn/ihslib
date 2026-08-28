target_sources(ihslib PRIVATE
        device.c
        provider.c
        manager.c
        report.c)

if (IHSLIB_HID_SDL)
    add_subdirectory(sdl)
endif ()

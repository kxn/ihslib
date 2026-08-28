cmake_minimum_required(VERSION 3.13)

project(ihslib C)

cmake_policy(SET CMP0076 NEW)

set(CMAKE_C_STANDARD 11)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/scripts)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(IHSLIB_SAMPLES "Build Samples" ON)
    option(IHSLIB_CODEGEN "Code Generator" ON)
else ()
    option(IHSLIB_SAMPLES "Build Samples" OFF)
    option(IHSLIB_CODEGEN "Code Generator" OFF)
endif ()

option(IHSLIB_SANITIZE_ADDRESS "Link Address Sanitizer" OFF)
option(IHSLIB_SANITIZE_UNDEFINED "Link Undefined Behavior Sanitizer" OFF)
option(IHSLIB_SANITIZE_THREAD "Link Thread Sanitizer (mutually exclusive with address)" OFF)

find_package(PkgConfig REQUIRED)

option(IHSLIB_HID_SDL_USE_SDL2 "Build SDL HID provider against SDL2 instead of SDL3" OFF)

# Default is SDL3 upstream. Switch portlibs currently provide SDL2 but not SDL3,
# so the project can opt into an SDL2 compatibility build of the same provider.
if (IHSLIB_HID_SDL_USE_SDL2)
    if (NOT SDL2_FOUND)
        pkg_check_modules(SDL2 sdl2)
    endif ()
    if (NOT DEFINED IHSLIB_HID_SDL)
        set(IHSLIB_HID_SDL ${SDL2_FOUND} CACHE BOOL "Build SDL HID provider")
    endif ()
    if (IHSLIB_HID_SDL AND NOT SDL2_FOUND)
        message(FATAL_ERROR "IHSLIB_HID_SDL_USE_SDL2 requires SDL2")
    endif ()
else ()
    if (NOT SDL3_FOUND)
        pkg_check_modules(SDL3 sdl3)
    endif ()
    if (NOT DEFINED IHSLIB_HID_SDL)
        set(IHSLIB_HID_SDL ${SDL3_FOUND} CACHE BOOL "Build SDL HID provider")
    endif ()
    if (IHSLIB_HID_SDL AND NOT SDL3_FOUND)
        message(FATAL_ERROR "IHSLIB_HID_SDL requires SDL3")
    endif ()
endif ()

if (NOT PROTOBUF_C_FOUND)
    pkg_check_modules(PROTOBUF_C libprotobuf-c REQUIRED)
endif ()

if (NOT MBEDTLS_FOUND)
    find_package(MbedTLS)
endif ()

if (NOT MBEDTLS_FOUND)
    find_package(OpenSSL)
endif ()

add_library(ihslib)
add_subdirectory(src)

target_include_directories(ihslib PUBLIC include PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})

add_subdirectory(src/protobuf)
target_link_libraries(ihslib PRIVATE ihs-protobuf m)

add_subdirectory(src/platforms)
target_include_directories(ihs-platforms PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(ihslib PRIVATE ihs-platforms)

if (MBEDTLS_FOUND)
    target_sources(ihslib PRIVATE src/crypto/impl_mbedtls.c)
    target_include_directories(ihslib PRIVATE SYSTEM ${MBEDTLS_INCLUDE_DIRS})
    target_link_libraries(ihslib PUBLIC ${MBEDCRYPTO_LIBRARY})
elseif (OPENSSL_FOUND)
    target_sources(ihslib PRIVATE src/crypto/impl_openssl.c)
endif ()

add_subdirectory(src/hid)

if (IHSLIB_SANITIZE_ADDRESS AND IHSLIB_SANITIZE_THREAD)
    message(FATAL_ERROR "IHSLIB_SANITIZE_ADDRESS and IHSLIB_SANITIZE_THREAD are mutually exclusive")
endif ()

if (IHSLIB_SANITIZE_ADDRESS)
    add_compile_definitions(IHSLIB_SANITIZE_ADDRESS)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address -static-libasan)
endif ()

if (IHSLIB_SANITIZE_THREAD)
    add_compile_definitions(IHSLIB_SANITIZE_THREAD)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif ()

if (IHSLIB_SANITIZE_UNDEFINED)
    add_compile_definitions(IHSLIB_SANITIZE_UNDEFINED)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif ()

if (IHSLIB_SAMPLES)
    add_subdirectory(samples)
endif ()

# Don't include tools and samples if used as library
if (IHSLIB_CODEGEN)
    find_program(PROTOC_C protoc-c REQUIRED)
    find_program(PYTHON python3 REQUIRED)

    set(PROTOBUF_FILES
            protobuf/discovery.proto
            protobuf/remoteplay.proto
            protobuf/hiddevices.proto)
    add_custom_target(protoc-gen
            COMMAND ${PROTOC_C} --c_out=src ${PROTOBUF_FILES}
            COMMAND ${PYTHON} scripts/protobuf-src-fixup.py src/protobuf
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif ()

# Don't include tests by default if used as library
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif ()

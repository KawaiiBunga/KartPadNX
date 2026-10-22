# KartPad-NX — Switch (NRO) build.
#
# The full game is configured by builder/build-fullapp.sh (stages patch, configure, transform,
# build); see docs/BUILDING-SWITCH.md. It needs the staged runtime/Aurora, Dawn and SDL builds and
# the private translated game (KARTPAD_SHARDS, from the translate stage).
#
# Without KARTPAD_ENABLE_FULL_APP it builds only the Switch platform shim library.

cmake_minimum_required(VERSION 3.25)
project(kartpad_nx LANGUAGES C CXX ASM)

# Clang compiles the guest ISA, but does not interpret GCC specs. Use the
# devkitA64 driver for executable links so Switch crt, PIE and switch.ld apply.
find_program(KARTPAD_LINK_DRIVER aarch64-none-elf-g++
  HINTS "$ENV{DEVKITPRO}/devkitA64/bin" REQUIRED)
set(_kartpad_link_command "\"${KARTPAD_LINK_DRIVER}\"")
if(CMAKE_HOST_UNIX)
  set(_kartpad_link_command "bash \"${CMAKE_CURRENT_SOURCE_DIR}/../builder/link-switch.sh\" \"${KARTPAD_LINK_DRIVER}\"")
endif()
set(CMAKE_CXX_LINK_EXECUTABLE
  "${_kartpad_link_command} <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE
  "${_kartpad_link_command} <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO not set; configure with switch/cmake/SwitchClang.cmake")
endif()

include("${DEVKITPRO}/cmake/Switch.cmake" OPTIONAL) # picked up as toolchain

# CMake's Switch platform setup can collapse CMAKE_EXE_LINKER_FLAGS to only the
# libnx search path when Clang is used for compilation. Our executable link rule
# deliberately switches back to devkitA64 g++, so explicitly restore the flags
# that select libnx's CRT/linker script. Without switch.specs the generic
# aarch64-none-elf CRT is linked and libnx TLS symbols remain undefined.
set(CMAKE_EXE_LINKER_FLAGS
    "-L$ENV{DEVKITPRO}/libnx/lib -L$ENV{DEVKITPRO}/portlibs/switch/lib -fPIE -specs=$ENV{DEVKITPRO}/libnx/switch.specs"
    CACHE STRING "Switch executable linker flags" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KARTPAD_SHARDS "" CACHE FILEPATH "Path to the translated game's shards.cmake")
option(KARTPAD_BUILD_GRAPHICS "Compile Aurora against local Switch Dawn and SDL3" OFF)
option(KARTPAD_ENABLE_FULL_APP "Build the private translated game" OFF)
if(KARTPAD_ENABLE_FULL_APP)
  set(KARTPAD_BUILD_GRAPHICS ON)
endif()
if(KARTPAD_BUILD_GRAPHICS)
  include(cmake/Graphics.cmake)
endif()

# --- Platform shim (this port) -------------------------------------------------
set(SWITCH_SHIM_SOURCES
    src/host_services_switch.cpp
    src/input_switch.cpp
    src/runtime_host_switch.cpp
    src/clang_tls_switch.c   # tpidr_el0 sync for clang thread_local (see file)
    src/main_switch.cpp
    src/log_switch.cpp       # async RAM-buffered stdout/stderr sink
    src/perf_switch.cpp      # perf.cfg knobs + per-role thread placement
    src/prof_switch.cpp      # perf.cfg prof_hz: guest-thread sampling profiler
    src/runtime/disc_extractor.cpp
    src/runtime/disc_reader.cpp
    src/runtime/wii_cluster.cpp
    src/runtime/wii_crypto.cpp
    src/runtime/wii_crypto_switch.cpp
    src/runtime/wii_common_key.cpp
    src/runtime/wii_fst.cpp)

add_library(kartpad_switch_shim STATIC ${SWITCH_SHIM_SOURCES})
target_include_directories(kartpad_switch_shim PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include/kartpad")
# libnx comes from the toolchain's search paths.
target_compile_definitions(kartpad_switch_shim PUBLIC MKW_HOST_SWITCH=1 __SWITCH__=1)
target_link_options(kartpad_switch_shim INTERFACE "LINKER:--wrap=pthread_create" "LINKER:--wrap=exit")

# --- Full game: unresolved runtime port, never silently fall back to a shim ---
set(KARTPAD_RUNTIME_SRC "${CMAKE_SOURCE_DIR}/../build/wiicompiled-fpscr/runtime"
    CACHE PATH "Staged WiiCompiled runtime")
if(KARTPAD_ENABLE_FULL_APP)
  if(NOT KARTPAD_SHARDS OR NOT EXISTS "${KARTPAD_SHARDS}")
    message(FATAL_ERROR "Full app requested, but KARTPAD_SHARDS is missing; refusing a shim-only success.")
  endif()
  if(NOT EXISTS "${KARTPAD_RUNTIME_SRC}/src/main.cpp")
    message(FATAL_ERROR "Staged runtime missing; run builder/build-switch.sh --stage translate first.")
  endif()
  # Build the real translated runtime as an NRO.
  # Requires the staged runtime to carry wiicompiled-switch-runtime.patch
  # (KartPadRuntimeMain seam, __SWITCH__ fiber + guest-memory arms); the build
  # driver applies it before configuring.
  include(cmake/FullApp.cmake)
else()
  message(STATUS "Building platform shim/static libraries only; no game NRO is produced.")
endif()

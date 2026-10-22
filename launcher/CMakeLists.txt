cmake_minimum_required(VERSION 3.20)
project(mkw_launcher LANGUAGES C CXX ASM)

if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO environment variable not set; build within kartpad-dawn container")
endif()

# Clang compiles sources, but devkitA64 g++ is used as the link driver
# so Switch crt, PIE, and switch.specs linker scripts apply correctly.
find_program(KARTPAD_LINK_DRIVER aarch64-none-elf-g++
  HINTS "$ENV{DEVKITPRO}/devkitA64/bin" REQUIRED)
set(CMAKE_CXX_LINK_EXECUTABLE
  "\"${KARTPAD_LINK_DRIVER}\" <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE
  "\"${KARTPAD_LINK_DRIVER}\" <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

include("${DEVKITPRO}/cmake/Switch.cmake" OPTIONAL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Locate Switch cross pkg-config
find_program(PKG_CONFIG_EXECUTABLE NAMES aarch64-none-elf-pkg-config HINTS "$ENV{DEVKITPRO}/portlibs/switch/bin")
find_package(PkgConfig REQUIRED)

# Query portlibs dependencies required for UI, audio, fonts, networking, and compression
pkg_check_modules(DEPS REQUIRED
  sdl2
  SDL2_image
  SDL2_ttf
  libcurl
  libsodium
  jansson
  libzstd
  zlib
  libpng
  libjpeg
  harfbuzz
  freetype2
)

# Application metadata
set(LAUNCHER_TITLE "MKW Launcher")
set(LAUNCHER_AUTHOR "kawaii")
set(LAUNCHER_VERSION "1.0.0")
set(LAUNCHER_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")

if(NOT EXISTS "${LAUNCHER_ICON}")
  message(FATAL_ERROR "MKW Launcher icon missing: ${LAUNCHER_ICON}")
endif()

# Source files
file(GLOB_RECURSE LAUNCHER_SOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/source/**/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/source/**/*.c"
)

# Runtime disc extractor files from switch/src/runtime
set(EXTRACTOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../switch/src/runtime")
set(EXTRACTOR_SOURCES
  "${EXTRACTOR_DIR}/disc_extractor.cpp"
  "${EXTRACTOR_DIR}/disc_reader.cpp"
  "${EXTRACTOR_DIR}/wii_cluster.cpp"
  "${EXTRACTOR_DIR}/wii_crypto.cpp"
  "${EXTRACTOR_DIR}/wii_crypto_switch.cpp"
  "${EXTRACTOR_DIR}/wii_common_key.cpp"
  "${EXTRACTOR_DIR}/wii_fst.cpp"
)

add_executable(mkw_launcher
  ${LAUNCHER_SOURCES}
  ${EXTRACTOR_SOURCES}
)

# Clean up pkg-config flags that clash with Clang / SwitchClang
set(FILTERED_CFLAGS "")
foreach(flag ${DEPS_CFLAGS_OTHER})
  if(flag MATCHES "^-mtp=" OR flag MATCHES "^-march=" OR flag MATCHES "^-mtune=" OR flag MATCHES "^-ftls-model=" OR flag MATCHES "^-D")
    # handled by SwitchClang / definitions
  else()
    list(APPEND FILTERED_CFLAGS "${flag}")
  endif()
endforeach()

target_include_directories(mkw_launcher PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/source"
  "${EXTRACTOR_DIR}"
  ${DEPS_INCLUDE_DIRS}
  "$ENV{DEVKITPRO}/libnx/include"
  "$ENV{DEVKITPRO}/portlibs/switch/include"
)

target_compile_definitions(mkw_launcher PRIVATE
  __SWITCH__=1
  CURL_STATICLIB=1
)

target_compile_options(mkw_launcher PRIVATE
  -Wall
  -Wextra
  -ffunction-sections
  -fdata-sections
  ${FILTERED_CFLAGS}
)

target_link_directories(mkw_launcher PRIVATE
  ${DEPS_LIBRARY_DIRS}
  "$ENV{DEVKITPRO}/libnx/lib"
  "$ENV{DEVKITPRO}/portlibs/switch/lib"
)

target_link_libraries(mkw_launcher PRIVATE
  -Wl,--start-group
  ${DEPS_LIBRARIES}
  -lnx
  -lm
  -Wl,--end-group
)

target_link_options(mkw_launcher PRIVATE
  -Wl,--gc-sections
)

# Generate control.nacp
nx_generate_nacp(mkw-launcher.nacp
  NAME "${LAUNCHER_TITLE}"
  AUTHOR "${LAUNCHER_AUTHOR}"
  VERSION "${LAUNCHER_VERSION}"
)

# Staged RomFS directory
set(LAUNCHER_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs" CACHE PATH "RomFS staging directory")
file(MAKE_DIRECTORY "${LAUNCHER_ROMFS_DIR}")

# Create final NRO with embedded NACP, icon, and RomFS
nx_create_nro(mkw_launcher
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mkw-launcher.nro"
  NACP mkw-launcher.nacp
  ICON "${LAUNCHER_ICON}"
  ROMFS "${LAUNCHER_ROMFS_DIR}"
)

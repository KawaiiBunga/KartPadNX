# SwitchClang.cmake — devkitPro Switch platform, compiled with aarch64 Clang.
#
# Why: the translated MKW runtime's ISA layer uses Clang-only extensions
# (`ext_vector_type`, `__builtin_rotateleft32`) and x86 SSE intrinsics mapped to
# NEON via sse2neon. devkitA64 GCC 16 rejects these, so the whole translated
# graph must be built with Clang. This toolchain keeps everything else devkitPro
# provides — libnx include/lib paths, `switch.specs`, and the elf2nro / nacptool
# packaging helpers — and swaps ONLY the compiler frontend to Clang, using
# devkitA64's binutils and libstdc++ found through the sysroot.
#
# Usage (run from an environment where DEVKITPRO is a NATIVE path so Clang.exe
# gets a real sysroot, e.g. PowerShell or the devkitPro msys2 shell):
#   cmake -B <build> -S switch -G "Unix Makefiles" \
#         -DCMAKE_MAKE_PROGRAM=<devkitPro>/msys2/usr/bin/make.exe \
#         -DCMAKE_TOOLCHAIN_FILE=switch/cmake/SwitchClang.cmake
# See the top-level README for the Windows path-convention notes.
#
# Verified 2026-09-12: translated shards + the platform shim compile to aarch64
# objects with exactly the flags below.

if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO not set in the environment")
endif()
set(DEVKITPRO "$ENV{DEVKITPRO}")

find_program(KARTPAD_CLANG   NAMES clang   clang.exe   REQUIRED)
find_program(KARTPAD_CLANGXX NAMES clang++ clang++.exe REQUIRED)

# Force Clang for every language so devkitA64.cmake's find_program(<triplet>-gcc)
# calls become no-ops (an unseeded ASM slot was also mis-split on the ':' of a
# Windows drive letter under the msys2 CMake driver).
set(CMAKE_C_COMPILER   "${KARTPAD_CLANG}"   CACHE FILEPATH "aarch64 clang"     FORCE)
set(CMAKE_CXX_COMPILER "${KARTPAD_CLANGXX}" CACHE FILEPATH "aarch64 clang++"   FORCE)
set(CMAKE_ASM_COMPILER "${KARTPAD_CLANG}"   CACHE FILEPATH "aarch64 clang asm" FORCE)

# --target / --sysroot via the CMake-native knobs: unlike CMAKE_<LANG>_FLAGS_INIT,
# these survive the devkitPro NintendoSwitch platform file, which re-initializes
# the flag variables at enable_language() (after this toolchain runs). They are
# applied to both compile and link driver invocations.
set(CMAKE_C_COMPILER_TARGET   aarch64-none-elf)
set(CMAKE_CXX_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_SYSROOT "${DEVKITPRO}/devkitA64/aarch64-none-elf")

# The compiler check compiles only (no link): a bare aarch64-none-elf link needs
# switch.specs/crt that CMake's trivial test does not pull in.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Inherit the devkitPro Switch platform: libnx include/lib dirs, switch.specs
# link flags, elf2nro/nacptool + nx_create_nro / nx_generate_nacp helpers.
include("${DEVKITPRO}/cmake/Switch.cmake")

# Own the compile-flag caches so the platform's flag string (which carries the
# GCC-only `-mtp=soft` clang rejects) does NOT seed them. Re-add the devkitPro
# arch/common flags we want, minus `-mtp=soft`; clang picks correct aarch64 TLS
# for local-exec on its own. libnx `-isystem` and the link `-specs`/`-L` come
# from the platform as include dirs / linker flags and are unaffected by these.
# --gcc-toolchain is required so clang's libstdc++ search reaches the
# target-specific bits/c++config.h under aarch64-none-elf/include/c++/<ver>/aarch64-none-elf.
# devkitA64 keeps libstdc++'s target-specific `bits/c++config.h` in a
# triple-named multilib subdir (`include/c++/<ver>/aarch64-none-elf/bits/`).
# Newer clang (22, Windows) descends into it via --gcc-toolchain on its own, but
# older clang (14, the devkitpro-mesa Docker image) does not, so add the dir
# explicitly. Globbed so the gcc version bump doesn't break it; guarded so a
# missing dir is simply omitted.
file(GLOB _KARTPAD_CXXCFG_DIRS
     "${DEVKITPRO}/devkitA64/aarch64-none-elf/include/c++/*/aarch64-none-elf")
set(_KARTPAD_CXX_MULTILIB_INC "")
foreach(_d IN LISTS _KARTPAD_CXXCFG_DIRS)
  if(EXISTS "${_d}/bits/c++config.h")
    string(APPEND _KARTPAD_CXX_MULTILIB_INC " -I${_d}")
  endif()
endforeach()

# _GNU_SOURCE/_DEFAULT_SOURCE un-hide newlib's POSIX/BSD extensions (isascii,
# fdopen, sigfillset/SIG_SETMASK, …) that strict `-std=c++20` (which defines
# __STRICT_ANSI__) otherwise conceals — Dawn/abseil rely on several of them.
set(_KARTPAD_CFLAGS "--target=aarch64-none-elf -stdlib=libstdc++ --gcc-toolchain=${DEVKITPRO}/devkitA64 -mcpu=cortex-a57+crc+crypto -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -Wno-multichar -Wno-multilib-not-found")
set(CMAKE_C_FLAGS   "${_KARTPAD_CFLAGS}"                            CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${_KARTPAD_CFLAGS}${_KARTPAD_CXX_MULTILIB_INC}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "-mcpu=cortex-a57+crc+crypto"   CACHE STRING "" FORCE)

# Link with libstdc++ too (compile-only check above never exercised the linker).
# --target/--sysroot are added automatically from the knobs set above.
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -stdlib=libstdc++")



set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_LINK 1)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_LINK 1)

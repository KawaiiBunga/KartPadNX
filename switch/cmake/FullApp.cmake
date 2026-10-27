# Full translated game runtime, Switch-targeted.
#
# Drives the real WiiCompiled runtime (build/wiicompiled-fpscr/runtime, already
# carrying switch-runtime-portability.patch + wiicompiled-switch-runtime.patch)
# through the fixture's proven Switch graphics stack instead of the runtime's own
# Windows-from-source CMakeLists. Mirrors cmake/PublicProducts.cmake but:
#   * aarch64 flags from SwitchClang (no -march=x86-64-v3);
#   * platform sources swapped to the switch/src/runtime twins;
#   * vendored deps built here (the runtime CMakeLists that defines them is not
#     included);
#   * NO game-asset POST_BUILD copies — assets are staged to SD at runtime, never
#     embedded in the NRO;
#   * no precompiled headers / unity build (mkw_pch.h is not Switch-ABI clean).
#
# Requires KARTPAD_BUILD_GRAPHICS (Graphics.cmake: prebuilt Dawn/SDL3 + Aurora)
# and GraphicsLink.cmake (kartpad_link_switch_graphics), both included already by
# switch/CMakeLists.txt before this file.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/GraphicsLink.cmake")

set(RT "${KARTPAD_RUNTIME_SRC}")
if(NOT EXISTS "${RT}/src/main.cpp")
  message(FATAL_ERROR "Staged runtime missing at ${RT}; run builder/build-switch.sh --stage translate.")
endif()

# KartPad runtime headers this port compiles against (switch/include, copied from the KartPad
# runtime): header-only PowerPC semantics the translated FP adapters call, the Classic Controller
# host bridge and the host-services interface.
set(KARTPAD_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/include")
if(NOT EXISTS "${KARTPAD_RUNTIME_INCLUDE}/kartpad/semantics/ppc_semantics.h")
  message(FATAL_ERROR "Missing ${KARTPAD_RUNTIME_INCLUDE}/kartpad/semantics/ppc_semantics.h")
endif()

set(KARTPAD_PROFILE "base" CACHE STRING "Target profile: base or retro-rewind")

set(_PROFILE_DEFS)
if(KARTPAD_PROFILE STREQUAL "retro-rewind")
  list(APPEND _PROFILE_DEFS MKW_PROFILE_RETRO_REWIND=1 MKW_ENABLE_RETRO_WFC=1)
  target_compile_definitions(kartpad_switch_shim PUBLIC ${_PROFILE_DEFS})
  set(_DEFAULT_TRANSLATION_DIR "${CMAKE_SOURCE_DIR}/../private/self-build/retro-rewind/translation")
else()
  set(_DEFAULT_TRANSLATION_DIR "${CMAKE_SOURCE_DIR}/../private/self-build/translation")
endif()

# Generated data-init + crash-symbol table for the selected profile.
set(KARTPAD_TRANSLATION_DIR "${_DEFAULT_TRANSLATION_DIR}"
    CACHE PATH "Translator output dir holding data_sections_init.cpp + guest_symbol_table.cpp")

set(_SWITCH_RUNTIME_SRC "${CMAKE_SOURCE_DIR}/src/runtime")

# The translator-owned shard graph (base profile: MKW_BASE_COMMON_SHARDS,
# MKW_BASE_REGISTRATION_SOURCES; RR profile adds sensitive and mod shards).
# shards.cmake embeds the absolute paths from the machine that ran the translator
# (a Windows checkout); rewrite that root to this shards file's actual directory
# so the graph resolves under any mount (e.g. the Docker /project bind).
get_filename_component(_shard_dir "${KARTPAD_SHARDS}" DIRECTORY)
file(READ "${KARTPAD_SHARDS}" _shards_text)
if(_shards_text MATCHES "set\\(MKW_TRANSLATED_SHARD_ROOT \"([^\"]+)\"")
  set(_embedded_root "${CMAKE_MATCH_1}")
  if(NOT _embedded_root STREQUAL _shard_dir)
    string(REPLACE "${_embedded_root}" "${_shard_dir}" _shards_text "${_shards_text}")
  endif()
endif()
if(_shards_text MATCHES "\"([^\"]+/private/self-build/retro-rewind/mod/cpp)/")
  set(_embedded_mod_root "${CMAKE_MATCH_1}")
  get_filename_component(_rr_root "${_shard_dir}/../.." ABSOLUTE)
  set(_real_mod_root "${_rr_root}/mod/cpp")
  string(REPLACE "\\" "/" _embedded_mod_root "${_embedded_mod_root}")
  string(REPLACE "\\" "/" _real_mod_root "${_real_mod_root}")
  if(NOT _embedded_mod_root STREQUAL _real_mod_root)
    string(REPLACE "${_embedded_mod_root}" "${_real_mod_root}" _shards_text "${_shards_text}")
  endif()
endif()
set(_shards_local "${CMAKE_BINARY_DIR}/shards_local.cmake")
file(WRITE "${_shards_local}" "${_shards_text}")
include("${_shards_local}")
if(NOT MKW_BASE_COMMON_SHARDS)
  message(FATAL_ERROR "Shard graph ${KARTPAD_SHARDS} defines no MKW_BASE_COMMON_SHARDS")
endif()
message(STATUS "FullApp: ${MKW_BASE_FUNCTION_COUNT} base functions, "
        "${MKW_RETRO_REWIND_FUNCTION_COUNT} RR (${KARTPAD_PROFILE} profile)")

# -fno-math-errno: with the default errno semantics clang may not treat fma() as its builtin, so the
# runtime helpers' std::fma (fmadd/fmsub/fnmadd/fnmsub, ps_madd) was a call to newlib's
# `fmadd d0, d0, d1, d2; ret`, clobbering every caller-saved FP register around it. Same result:
# no translated code reads errno, and the instruction is what that call ran.
set(_FP_OPTS -fno-fast-math -ffp-contract=off -fno-math-errno)

# Lean codegen for the statically-recompiled guest code. The confirmed on-hardware
# bottleneck is single-core instruction bloat + memory latency (GPU is idle,
# endFrame~=0), so shrinking .text (Tegra X1 A57 has only 48 KiB L1I) and freeing
# a register across all ~29k translated functions attacks the root cause directly:
#   -fomit-frame-pointer            : reclaim x29 as a GPR, drop prologue/epilogue.
#   -fno-(asynchronous-)unwind-tables: translated code never throws/unwinds
#                                      (control flow is fibers, not C++ EH), so the
#                                      per-function .eh_frame/.ARM.exidx is dead
#                                      weight -- removing it shrinks the image and
#                                      the hot I-cache footprint.
# Guest FP bit-exactness is unaffected (that is governed by _FP_OPTS above).
# Tradeoff: FP/unwind-based crash backtraces through translated frames get worse;
# acceptable since diagnosis is via perf.txt + address maps, not stack unwinding.
set(_LEAN_CODEGEN -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables)

# ---------------------------------------------------------------- includes ---
# The runtime spells translator output as "generated/<x>.h". Only RuntimeConfig.h
# is referenced that way; the translator wrote it into KARTPAD_TRANSLATION_DIR
# (not build/wiicompiled-fpscr/generated), so alias it into the build tree.
set(_GEN_DIR "${CMAKE_BINARY_DIR}/gen/generated")
file(MAKE_DIRECTORY "${_GEN_DIR}")
if(EXISTS "${KARTPAD_TRANSLATION_DIR}/RuntimeConfig.h")
  configure_file("${KARTPAD_TRANSLATION_DIR}/RuntimeConfig.h" "${_GEN_DIR}/RuntimeConfig.h" COPYONLY)
else()
  message(FATAL_ERROR "Missing ${KARTPAD_TRANSLATION_DIR}/RuntimeConfig.h (run translate)")
endif()

set(_RT_INCLUDES
    "${RT}/include"
    "${RT}/src"
    "${CMAKE_SOURCE_DIR}/include"  # Switch KPAD host-input bridge
    "${CMAKE_SOURCE_DIR}/src"      # aarch64_fiber.h, clang_tls_switch.h (this port)
    "${CMAKE_BINARY_DIR}/gen"      # generated/RuntimeConfig.h
    "${KARTPAD_AURORA_SOURCE}/include")
set(_LIBNX_INCLUDES
    "$ENV{DEVKITPRO}/libnx/include"
    "$ENV{DEVKITPRO}/portlibs/switch/include")

# ------------------------------------------------------- vendored libraries ---
add_library(mkw_pugixml STATIC "${RT}/third_party/pugixml/pugixml.cpp")
target_include_directories(mkw_pugixml PUBLIC "${RT}/third_party/pugixml")
target_compile_features(mkw_pugixml PUBLIC cxx_std_17)
target_compile_options(mkw_pugixml PRIVATE -w)

add_library(mkw_toml11 INTERFACE)
target_include_directories(mkw_toml11 SYSTEM INTERFACE "${RT}/third_party/toml11")
target_compile_features(mkw_toml11 INTERFACE cxx_std_17)

file(GLOB _CRYPTOPP_SOURCES CONFIGURE_DEPENDS "${RT}/third_party/cryptopp/*.cpp")
list(FILTER _CRYPTOPP_SOURCES EXCLUDE REGEX
    "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\\.cpp$")
list(FILTER _CRYPTOPP_SOURCES EXCLUDE REGEX "(_avx|_ppc|_simd|_sse)\\.cpp$")
add_library(mkw_cryptopp STATIC ${_CRYPTOPP_SOURCES})
target_include_directories(mkw_cryptopp SYSTEM PUBLIC "${RT}/third_party")
target_compile_features(mkw_cryptopp PUBLIC cxx_std_17)
target_compile_definitions(mkw_cryptopp PUBLIC CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE)
target_compile_options(mkw_cryptopp PRIVATE -w)

# ------------------------------------------------------ translated shard lib ---
add_library(mkw_base_shared STATIC ${MKW_BASE_COMMON_SHARDS})
target_include_directories(mkw_base_shared PRIVATE ${_RT_INCLUDES})
target_include_directories(mkw_base_shared SYSTEM PRIVATE "${KARTPAD_RUNTIME_INCLUDE}")
target_compile_definitions(mkw_base_shared PRIVATE TARGET_PC __SWITCH__=1 NOMINMAX ${_PROFILE_DEFS})
# -O3 over -O2: on-hardware data showed the hot path is bound by executed
# instruction/call count, not raw code size (the -moutline size-shrink REGRESSED
# it +29% median). Each shard bundles ~400 guest functions, so -O3's higher
# inline threshold folds intra-shard guest->guest calls that -O2 leaves as real
# calls, cutting call overhead. -fno-slp-vectorize stays (SLP bloats generated
# code); _FP_OPTS keeps guest FP bit-exactness.
# -fno-strict-aliasing: the translated PPC code type-puns guest memory heavily
# (same address read as u32, float, double through the flat base); strict aliasing
# assumptions under -O3's aggressive optimization can misoptimize these patterns.
target_compile_options(mkw_base_shared PRIVATE -O3 -fno-strict-aliasing ${_FP_OPTS} ${_LEAN_CODEGEN} -fno-slp-vectorize -w -pipe)
# Sample PGO for the translated shards (docs/PERF-60FPS-ROADMAP.md, "Sample PGO"): a profile made by
# builder/pgo-gen.py from on-hardware samples. Build-level, so it ships as a separate NRO; the
# [build] log line carries KARTPAD_PGO_TAG so a log always says which variant ran.
set(KARTPAD_PGO_PROFILE "" CACHE FILEPATH "LLVM sample profile for the translated shards (empty = off)")
option(KARTPAD_SPLIT_FUNCTIONS "Split never-sampled blocks out of every target the profile optimizes (needs KARTPAD_PGO_PROFILE)" OFF)
if(KARTPAD_PGO_PROFILE)
  if(NOT EXISTS "${KARTPAD_PGO_PROFILE}")
    message(FATAL_ERROR "KARTPAD_PGO_PROFILE does not exist: ${KARTPAD_PGO_PROFILE}")
  endif()
  message(STATUS "FullApp: sample PGO for translated shards from ${KARTPAD_PGO_PROFILE}")
  target_compile_options(mkw_base_shared PRIVATE "-fprofile-sample-use=${KARTPAD_PGO_PROFILE}")
  get_filename_component(_PGO_NAME "${KARTPAD_PGO_PROFILE}" NAME)
  # Machine function splitting (docs/RR-PERFORMANCE-2026-10.md): blocks the profile never saw
  # move to .text.split.<fn>, which the link order leaves behind the hot functions. Layout only:
  # the shards' instructions are unchanged apart from the branches between the two parts. The
  # Retro Rewind shards (RetroRewind.cmake), KARTPAD_PGO_ALL's targets and ThinLTO's backends
  # split the same way.
  if(KARTPAD_SPLIT_FUNCTIONS)
    target_compile_options(mkw_base_shared PRIVATE -fsplit-machine-functions)
    string(APPEND _PGO_NAME "+split")
  endif()
  target_compile_definitions(kartpad_switch_shim PRIVATE "KARTPAD_PGO_TAG=\"${_PGO_NAME}\"")
endif()
# Hot-function link order (same doc section): a GNU ld --section-ordering-file from
# builder/section-order.py. Pure layout; tagged as order=<file> on the [build] log line.
set(KARTPAD_SECTION_ORDER "" CACHE FILEPATH "GNU ld section ordering file for hot functions (empty = off)")
if(KARTPAD_SECTION_ORDER)
  if(NOT EXISTS "${KARTPAD_SECTION_ORDER}")
    message(FATAL_ERROR "KARTPAD_SECTION_ORDER does not exist: ${KARTPAD_SECTION_ORDER}")
  endif()
  message(STATUS "FullApp: hot-function link order from ${KARTPAD_SECTION_ORDER}")
  get_filename_component(_ORDER_NAME "${KARTPAD_SECTION_ORDER}" NAME)
  target_compile_definitions(kartpad_switch_shim PRIVATE "KARTPAD_ORDER_TAG=\"${_ORDER_NAME}\"")
endif()
set_target_properties(mkw_base_shared PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------- runtime object lib ---
file(GLOB_RECURSE _RT_SOURCES CONFIGURE_DEPENDS "${RT}/src/*.cpp")
list(REMOVE_ITEM _RT_SOURCES
    "${RT}/src/product/base_product.cpp"
    "${RT}/src/product/retro_rewind_product.cpp"
    "${RT}/src/host_cpu_baseline.cpp"
    "${RT}/src/guest_flat_memory.cpp"
    "${RT}/src/wup028_adapter.cpp")
list(APPEND _RT_SOURCES
    "${_SWITCH_RUNTIME_SRC}/guest_flat_memory_switch.cpp"
    "${_SWITCH_RUNTIME_SRC}/wup028_adapter_stub.cpp"
    "${_SWITCH_RUNTIME_SRC}/rkg_fixture_stub.cpp")

add_library(mkw_runtime_common OBJECT ${_RT_SOURCES})
target_include_directories(mkw_runtime_common PRIVATE ${_RT_INCLUDES} ${_LIBNX_INCLUDES})
target_include_directories(mkw_runtime_common SYSTEM PRIVATE "${KARTPAD_RUNTIME_INCLUDE}")
target_compile_definitions(mkw_runtime_common PRIVATE
    TARGET_PC __SWITCH__=1 MKW_HOST_SWITCH=1 NOMINMAX
    SDL_MAIN_HANDLED _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION ${_PROFILE_DEFS})
target_compile_options(mkw_runtime_common PRIVATE -O3 -ffast-math -w -pipe)
set_target_properties(mkw_runtime_common PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(mkw_runtime_common PRIVATE
    aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx aurora::core aurora::os
    mkw_pugixml mkw_toml11 mkw_cryptopp)

# The runtime relies on unity-build GROUP concatenation for cross-TU visibility
# of file-scope definitions (e.g. gx_texture.cpp uses g_TlutObjMeta defined in
# gx_objects.cpp). Replicate PublicProducts.cmake's grouping exactly; without it
# those references are undeclared. PCH is intentionally NOT used (mkw_pch.h is
# not Switch-ABI clean); unity build and PCH are independent.
foreach(source IN LISTS _RT_SOURCES)
  get_filename_component(_sname "${source}" NAME_WE)
  string(REPLACE "\\" "/" _snorm "${source}")
  if(_snorm MATCHES "/hle/gx/")
    set(_group "gx_bridge")
  elseif(_sname MATCHES "network|socket|dns|dwc|ios")
    set(_group "network_ios")
  elseif(_sname MATCHES "os_|system|memory|fiber|scheduler")
    set(_group "guest_system")
  elseif(_sname MATCHES "debug|trace|prof")
    set(_group "diagnostics")
  else()
    string(SHA256 _shash "${_sname}")
    string(SUBSTRING "${_shash}" 0 4 _sprefix)
    math(EXPR _bucket "0x${_sprefix} % 8")
    set(_group "runtime_${_bucket}")
  endif()
  set_source_files_properties("${source}" PROPERTIES UNITY_GROUP "${_group}")
endforeach()
set_target_properties(mkw_runtime_common PROPERTIES UNITY_BUILD OFF UNITY_BUILD_MODE GROUP)

# Guest-visible FP bit semantics: same contraction/rounding as translated shards,
# never fast-math (would erase PPC single-precision rounding points). Kept out of
# the fast-math unity groups. The two switch platform arms carry anonymous-
# namespace symbols that must not collide under concatenation, so they too stay
# standalone.
set_source_files_properties(
    "${RT}/src/ppc_helpers.cpp" "${RT}/src/fpu_helpers.cpp"
    "${_SWITCH_RUNTIME_SRC}/guest_flat_memory_switch.cpp"
    "${_SWITCH_RUNTIME_SRC}/wup028_adapter_stub.cpp"
    "${_SWITCH_RUNTIME_SRC}/rkg_fixture_stub.cpp"
    # Includes <switch.h> for libnx light events, whose typedefs collide with the Dolphin SDK
    # headers of the gx_bridge group.
    "${RT}/src/hle/gx/gx_hle_thread.cpp"
    TARGET_DIRECTORY mkw_runtime_common
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
set_source_files_properties(
    "${RT}/src/ppc_helpers.cpp" "${RT}/src/fpu_helpers.cpp"
    TARGET_DIRECTORY mkw_runtime_common
    PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")

# Host ISA guard: on Switch it is a no-op stub (aarch64 has no CPUID gate).
add_library(mkw_cpu_baseline OBJECT "${_SWITCH_RUNTIME_SRC}/host_cpu_baseline_stub.cpp")
target_compile_features(mkw_cpu_baseline PRIVATE cxx_std_17)
target_compile_options(mkw_cpu_baseline PRIVATE -w)

# ----------------------------------------------------- data-init / symbols ---
set(_DATA_INIT_SOURCES)
if(EXISTS "${KARTPAD_TRANSLATION_DIR}/data_sections_init.cpp")
  list(APPEND _DATA_INIT_SOURCES "${KARTPAD_TRANSLATION_DIR}/data_sections_init.cpp")
else()
  message(WARNING "No data_sections_init.cpp under ${KARTPAD_TRANSLATION_DIR}; "
          "guest static data will be uninitialized.")
endif()
if(EXISTS "${KARTPAD_TRANSLATION_DIR}/data_sections_init_blobs.S")
  enable_language(ASM)
  # The translator emits this for COFF (.rdata,"dr") with absolute .incbin paths
  # from the build host. Rewrite to ELF/aarch64 (.rodata) and repoint the .incbin
  # root to this checkout so it assembles under any mount (Docker /project).
  file(READ "${KARTPAD_TRANSLATION_DIR}/data_sections_init_blobs.S" _blobs_text)
  string(REPLACE ".rdata,\"dr\"" ".rodata,\"a\",@progbits" _blobs_text "${_blobs_text}")
  if(_blobs_text MATCHES "\\.incbin \"([^\"]*/data_sections_init_blobs)/")
    set(_embedded_blob_dir "${CMAKE_MATCH_1}")
    set(_real_blob_dir "${KARTPAD_TRANSLATION_DIR}/data_sections_init_blobs")
    if(NOT _embedded_blob_dir STREQUAL _real_blob_dir)
      string(REPLACE "${_embedded_blob_dir}" "${_real_blob_dir}" _blobs_text "${_blobs_text}")
    endif()
  endif()
  set(_blobs_local "${CMAKE_BINARY_DIR}/gen/data_sections_init_blobs.S")
  string(REPLACE "/e/" "E:/" _blobs_text "${_blobs_text}")
    file(WRITE "${_blobs_local}" "${_blobs_text}")
  set_source_files_properties("${_blobs_local}" PROPERTIES LANGUAGE ASM)
  list(APPEND _DATA_INIT_SOURCES "${_blobs_local}")
endif()
if(EXISTS "${KARTPAD_TRANSLATION_DIR}/guest_symbol_table.cpp")
  list(APPEND _DATA_INIT_SOURCES "${KARTPAD_TRANSLATION_DIR}/guest_symbol_table.cpp")
else()
  list(APPEND _DATA_INIT_SOURCES "${RT}/cmake/guest_symbol_table_stub.cpp")
endif()
if(_DATA_INIT_SOURCES)
  add_library(mkw_data_init OBJECT ${_DATA_INIT_SOURCES})
  target_include_directories(mkw_data_init PRIVATE ${_RT_INCLUDES})
  target_include_directories(mkw_data_init SYSTEM PRIVATE "${KARTPAD_RUNTIME_INCLUDE}")
  target_compile_definitions(mkw_data_init PRIVATE TARGET_PC __SWITCH__=1 NOMINMAX ${_PROFILE_DEFS})
  target_compile_options(mkw_data_init PRIVATE -O2 ${_FP_OPTS} ${_LEAN_CODEGEN} -w -pipe)
  set_target_properties(mkw_data_init PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()

# --------------------------------------------------------------- product ---
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_NATIVE_MODEL_REGION "${CMAKE_BINARY_DIR}/gen/native_model_region.cpp")
set(_NATIVE_MODEL_INPUTS)
foreach(_address IN ITEMS 800605C0 800606E0 80060980 800609E0 80060A60
    800619C0 80061B80 80062050 800620C0 80062330 80062480 80062650
    80062860 80062910 800629C0 80062A80 80063380 80063410 800636C0
    80063870 80063EE0 80063FE0 80064450 80064B30 80064FD0)
  list(APPEND _NATIVE_MODEL_INPUTS "${KARTPAD_TRANSLATION_DIR}/functions/func_${_address}.cpp")
endforeach()
file(GLOB _NATIVE_MODEL_SHARDS CONFIGURE_DEPENDS
    "${KARTPAD_TRANSLATION_DIR}/build_shards/base_common/*.cpp"
    "${KARTPAD_TRANSLATION_DIR}/build_shards/base_common/*_traits.h")
add_custom_command(OUTPUT "${_NATIVE_MODEL_REGION}" "${CMAKE_BINARY_DIR}/gen/native_model_region.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../builder/generate-native-model.py"
        "${KARTPAD_TRANSLATION_DIR}" "${_NATIVE_MODEL_REGION}"
    DEPENDS "${CMAKE_SOURCE_DIR}/../builder/generate-native-model.py"
        "${CMAKE_SOURCE_DIR}/../builder/capture-flat-base.py"
        "${CMAKE_SOURCE_DIR}/src/native_model_region_footer.inc"
        ${_NATIVE_MODEL_INPUTS} ${_NATIVE_MODEL_SHARDS}
    VERBATIM)
set(_NATIVE_SCENE_REGION "${CMAKE_BINARY_DIR}/gen/native_scene_regions.cpp")
set(_NATIVE_SCENE_INPUTS)
foreach(_address IN ITEMS 80055540 80055130 80055150 800551E0 80055250
    80055290 80055340 800553E0 80055480 80053D00 80054460 80054ED0
    80053FB0 800540F0 80054270)
  list(APPEND _NATIVE_SCENE_INPUTS "${KARTPAD_TRANSLATION_DIR}/functions/func_${_address}.cpp")
endforeach()
add_custom_command(OUTPUT "${_NATIVE_SCENE_REGION}" "${CMAKE_BINARY_DIR}/gen/native_scene_regions.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../builder/generate-native-scene.py"
        "${KARTPAD_TRANSLATION_DIR}" "${_NATIVE_SCENE_REGION}"
    DEPENDS "${CMAKE_SOURCE_DIR}/../builder/generate-native-scene.py"
        "${CMAKE_SOURCE_DIR}/../builder/generate-native-model.py"
        "${CMAKE_SOURCE_DIR}/../builder/capture-flat-base.py"
        ${_NATIVE_SCENE_INPUTS} ${_NATIVE_MODEL_SHARDS}
    VERBATIM)
if(KARTPAD_PROFILE STREQUAL "retro-rewind")
  include("${CMAKE_CURRENT_LIST_DIR}/RetroRewind.cmake")
  set(_PRODUCT_SOURCE "${RT}/src/product/retro_rewind_product.cpp")
  set(_REGISTRATION_SOURCES ${MKW_RETRO_REGISTRATION_SOURCES})
else()
  set(_PRODUCT_SOURCE "${RT}/src/product/base_product.cpp")
  set(_REGISTRATION_SOURCES ${MKW_BASE_REGISTRATION_SOURCES})
endif()
add_executable(kartpad_full
    "${_PRODUCT_SOURCE}"
    ${_REGISTRATION_SOURCES})
target_sources(kartpad_full PRIVATE
    $<TARGET_OBJECTS:mkw_runtime_common>
    $<TARGET_OBJECTS:mkw_cpu_baseline>
    # AArch64 cooperative fiber primitive backing the __SWITCH__ scheduler arm in
    # fiber_manager.cpp (kartpad_fiber_*). .c + the register-save .S trampoline.
    "${CMAKE_SOURCE_DIR}/src/aarch64_fiber.c"
    "${CMAKE_SOURCE_DIR}/src/aarch64_fiber_switch.S"
    # Hardware NEON IDCT THP video decoder and NEON matrix kernel
    "${CMAKE_SOURCE_DIR}/src/thp_video_decoder.cpp"
    "${CMAKE_SOURCE_DIR}/src/thp_video_hle.cpp"
    "${CMAKE_SOURCE_DIR}/src/native_matrix.cpp"
    "${CMAKE_SOURCE_DIR}/src/native_vector.cpp"
    "${CMAKE_SOURCE_DIR}/src/scalar_fp_cold.cpp"
    "${CMAKE_SOURCE_DIR}/src/native_scene.cpp"
    "${CMAKE_SOURCE_DIR}/src/native_subsystems.cpp"
    "${CMAKE_SOURCE_DIR}/src/transition_guard.cpp"
    "${_NATIVE_MODEL_REGION}"
    "${_NATIVE_SCENE_REGION}")
if(KARTPAD_PROFILE STREQUAL "retro-rewind")
  if(TARGET mkw_retro_sensitive)
    target_sources(kartpad_full PRIVATE $<TARGET_OBJECTS:mkw_retro_sensitive>)
  endif()
  target_sources(kartpad_full PRIVATE $<TARGET_OBJECTS:mkw_retro_rewind_functions>)
endif()
# The guarded vector kernels preserve the translated FP rounding/NI behavior.
# Override the product's broader fast-math flags for this compilation unit.
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/native_vector.cpp"
    "${CMAKE_SOURCE_DIR}/src/native_subsystems.cpp"
    "${CMAKE_SOURCE_DIR}/src/scalar_fp_cold.cpp"
    "${_NATIVE_SCENE_REGION}"
    PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off;-fno-math-errno")
set_property(SOURCE "${_NATIVE_SCENE_REGION}" APPEND PROPERTY COMPILE_OPTIONS
    -fno-strict-aliasing -fno-slp-vectorize)
option(KARTPAD_OUTLINE_SCALAR_FP "Share exceptional scalar FP paths in cold helpers" OFF)
set(_CPU_OPT_TAG "baseline")
if(KARTPAD_OUTLINE_SCALAR_FP)
  foreach(_target IN ITEMS mkw_base_shared mkw_runtime_common kartpad_full)
    target_compile_definitions(${_target} PRIVATE KARTPAD_OUTLINE_SCALAR_FP=1)
  endforeach()
  if(TARGET mkw_retro_sensitive)
    target_compile_definitions(mkw_retro_sensitive PRIVATE KARTPAD_OUTLINE_SCALAR_FP=1)
  endif()
  if(TARGET mkw_retro_rewind_functions)
    target_compile_definitions(mkw_retro_rewind_functions PRIVATE KARTPAD_OUTLINE_SCALAR_FP=1)
  endif()
  set(_CPU_OPT_TAG "cold-fp")
endif()
option(KARTPAD_THINLTO_SHARDS "Optimize translated guest functions across shard boundaries" OFF)
set(KARTPAD_THINLTO_JOBS "1" CACHE STRING "Maximum parallel ThinLTO backend jobs")
# Import only small functions across shards (LLVM IR instructions; empty = LLVM's 100 with its 10x
# hot and 100x critical multipliers). With the sample profile marking most translated call sites
# hot, the default imported multi-thousand-instruction bodies, which turned saved frames into spills
# (candidates F/G); a small limit with multipliers of 1 inlines the small callees whose call frame
# is most of their cost.
set(KARTPAD_THINLTO_IMPORT_LIMIT "" CACHE STRING "ThinLTO import-instr-limit (empty: LLVM default)")
if(KARTPAD_THINLTO_SHARDS)
  if(NOT KARTPAD_THINLTO_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "KARTPAD_THINLTO_JOBS must be a positive integer")
  endif()
  # The AIO toolchain links through aio/pipeline/nxtools.py with LLVM lld, which has ThinLTO
  # built in and archives with llvm-ar already; the Docker toolchain links with GNU ld and
  # needs the gold plugin.
  get_filename_component(_THINLTO_LINKER_NAME "${CMAKE_LINKER}" NAME)
  if(_THINLTO_LINKER_NAME MATCHES "lld")
    set(_THINLTO_LLD ON)
  else()
    set(_THINLTO_LLD OFF)
    string(REGEX MATCH "^[0-9]+" _LLVM_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")
    find_file(KARTPAD_LLVM_GOLD NAMES LLVMgold.so
        HINTS "/usr/lib/llvm-${_LLVM_MAJOR}/lib" REQUIRED)
    find_program(_THINLTO_AR NAMES "llvm-ar-${_LLVM_MAJOR}" REQUIRED)
    find_program(_THINLTO_RANLIB NAMES "llvm-ranlib-${_LLVM_MAJOR}" REQUIRED)
    # GNU devkitPro specs retain the proven crt/linker layout. LLVM archives must
    # index bitcode correctly; plain devkitA64 ranlib has no LLVM bitcode reader.
    set(CMAKE_AR "${_THINLTO_AR}")
    set(CMAKE_RANLIB "${_THINLTO_RANLIB}")
  endif()
  target_compile_options(mkw_base_shared PRIVATE -flto=thin)
  # The Switch BFD/plugin combination does not tell LLVM about --wrap symbols.
  # Keep their defining shards native so ThinLTO cannot import/integrate the
  # original bodies ahead of the linker's existing native dispatch seam.
  # One pass over each shard (they are large) for the definitions of all wrapped symbols.
  list(JOIN KARTPAD_GUEST_WRAP_SYMBOLS "|" _wrap_alternatives)
  set(_NATIVE_WRAP_SHARDS "")
  foreach(_symbol IN LISTS KARTPAD_GUEST_WRAP_SYMBOLS)
    set(_wrap_found_${_symbol} "")
  endforeach()
  foreach(_shard IN LISTS MKW_BASE_COMMON_SHARDS)
    file(STRINGS "${_shard}" _definitions
        REGEX "^extern \"C\" void (${_wrap_alternatives})\\(CpuContext\\* MKW_RESTRICT ctx\\)$")
    foreach(_definition IN LISTS _definitions)
      string(REGEX REPLACE "^extern \"C\" void ([A-Za-z0-9_]+)\\(.*$" "\\1" _symbol "${_definition}")
      list(APPEND _wrap_found_${_symbol} "${_shard}")
    endforeach()
  endforeach()
  foreach(_symbol IN LISTS KARTPAD_GUEST_WRAP_SYMBOLS)
    list(LENGTH _wrap_found_${_symbol} _count)
    if(NOT _count EQUAL 1)
      message(FATAL_ERROR "ThinLTO needs exactly one defining shard for ${_symbol}; found ${_count}")
    endif()
    list(APPEND _NATIVE_WRAP_SHARDS ${_wrap_found_${_symbol}})
  endforeach()
  list(REMOVE_DUPLICATES _NATIVE_WRAP_SHARDS)
  foreach(_shard IN LISTS _NATIVE_WRAP_SHARDS)
    set_property(SOURCE "${_shard}" APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
  endforeach()
  list(LENGTH _NATIVE_WRAP_SHARDS _native_count)
  message(STATUS "FullApp: ThinLTO retains ${_native_count} native wrapper-definition shards")
  if(_THINLTO_LLD)
    # Same backend settings as the gold branch below. lld always emits function and data
    # sections for LTO code. Machine function splitting is a code-generation option, so the
    # shards' -fsplit-machine-functions has to be repeated for the LTO backends.
    target_link_options(kartpad_full PRIVATE
        "LINKER:--thinlto-jobs=${KARTPAD_THINLTO_JOBS}"
        "LINKER:--lto-O3"
        "LINKER:--plugin-opt=-vectorize-slp=false"
        "LINKER:--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
    if(KARTPAD_THINLTO_IMPORT_LIMIT)
      target_link_options(kartpad_full PRIVATE
          "LINKER:--plugin-opt=-import-instr-limit=${KARTPAD_THINLTO_IMPORT_LIMIT}"
          "LINKER:--plugin-opt=-import-hot-multiplier=1"
          "LINKER:--plugin-opt=-import-critical-multiplier=1")
    endif()
    if(KARTPAD_PGO_PROFILE)
      target_link_options(kartpad_full PRIVATE "LINKER:--lto-sample-profile=${KARTPAD_PGO_PROFILE}")
      if(KARTPAD_SPLIT_FUNCTIONS)
        target_link_options(kartpad_full PRIVATE "LINKER:--plugin-opt=-split-machine-functions")
      endif()
    endif()
  else()
    target_link_options(kartpad_full PRIVATE
        "LINKER:-plugin,${KARTPAD_LLVM_GOLD}"
        "LINKER:-plugin-opt,thinlto"
        "LINKER:-plugin-opt,jobs=${KARTPAD_THINLTO_JOBS}"
        "LINKER:-plugin-opt,-vectorize-slp=false"
        "LINKER:-plugin-opt,cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
    # The backends otherwise run at the plugin's default O2 and emit one .text section, which
    # also defeats KARTPAD_SECTION_ORDER. The sample profile re-annotates imported bodies.
    target_link_options(kartpad_full PRIVATE
        "LINKER:-plugin-opt,O3"
        "LINKER:-plugin-opt,-function-sections"
        "LINKER:-plugin-opt,-data-sections")
    if(KARTPAD_PGO_PROFILE)
      target_link_options(kartpad_full PRIVATE "LINKER:-plugin-opt,sample-profile=${KARTPAD_PGO_PROFILE}")
      if(KARTPAD_SPLIT_FUNCTIONS)
        target_link_options(kartpad_full PRIVATE "LINKER:-plugin-opt,-split-machine-functions")
      endif()
    endif()
  endif()
  if(_CPU_OPT_TAG STREQUAL "baseline")
    set(_CPU_OPT_TAG "thin-shards")
  else()
    string(APPEND _CPU_OPT_TAG "+thin-shards")
  endif()
endif()
target_compile_definitions(kartpad_switch_shim PRIVATE KARTPAD_CPU_OPT_TAG="${_CPU_OPT_TAG}")
if(TARGET mkw_data_init)
  target_sources(kartpad_full PRIVATE $<TARGET_OBJECTS:mkw_data_init>)
endif()
# Per-link build stamp, logged first by main_switch.cpp (LogBuildIdentity).
set(_KARTPAD_STAMP_C "${CMAKE_CURRENT_BINARY_DIR}/kartpad_build_stamp.c")
add_custom_target(kartpad_build_stamp_gen
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${_KARTPAD_STAMP_C}" -P "${CMAKE_SOURCE_DIR}/cmake/BuildStamp.cmake"
  BYPRODUCTS "${_KARTPAD_STAMP_C}" VERBATIM)
add_dependencies(kartpad_full kartpad_build_stamp_gen)
target_sources(kartpad_full PRIVATE "${_KARTPAD_STAMP_C}")
target_include_directories(kartpad_full PRIVATE ${_RT_INCLUDES} ${_LIBNX_INCLUDES}
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/include/kartpad")
target_include_directories(kartpad_full SYSTEM PRIVATE "${KARTPAD_RUNTIME_INCLUDE}")
target_compile_definitions(kartpad_full PRIVATE
    TARGET_PC __SWITCH__=1 MKW_HOST_SWITCH=1 NOMINMAX
    SDL_MAIN_HANDLED _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION ${_PROFILE_DEFS})
target_compile_options(kartpad_full PRIVATE -O3 -ffast-math -w -pipe)
if(KARTPAD_SECTION_ORDER)
  target_link_options(kartpad_full PRIVATE "LINKER:--section-ordering-file=${KARTPAD_SECTION_ORDER}")
  set_property(TARGET kartpad_full APPEND PROPERTY LINK_DEPENDS "${KARTPAD_SECTION_ORDER}")
endif()
set_target_properties(kartpad_full PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(kartpad_full PRIVATE
    kartpad_switch_shim
    mkw_base_shared mkw_pugixml mkw_toml11 mkw_cryptopp
    aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx aurora::core aurora::os)

# libnx entry (kartpad_switch_shim: main_switch.cpp) + Dawn Null/deko3d/libnx link group.
kartpad_link_switch_graphics(kartpad_full)

# Profile-aware hbmenu title and NRO filename.
if(KARTPAD_PROFILE STREQUAL "retro-rewind")
  set(_DEFAULT_APP_NAME "KartPad Retro Rewind")
  set(_DEFAULT_NRO_NAME "kartpad-retro-rewind.nro")
else()
  set(_DEFAULT_APP_NAME "KartPad-NX")
  set(_DEFAULT_NRO_NAME "kartpad.nro")
endif()
set(KARTPAD_APP_NAME "${_DEFAULT_APP_NAME}" CACHE STRING "hbmenu title of this NRO")
set(KARTPAD_NRO_NAME "${_DEFAULT_NRO_NAME}" CACHE STRING "Output NRO filename")
set(_KARTPAD_APP_NAME "${KARTPAD_APP_NAME}")
nx_generate_nacp(kartpad-full.nacp NAME "${_KARTPAD_APP_NAME}" AUTHOR "KartPad-NX" VERSION "0.1.0")
# The runtime's first-run WC24 seed is public runtime support data, not disc
# content. Keep it in ROMFS so a fresh managed NAND can boot on Switch.
set(_KARTPAD_ROMFS "${CMAKE_CURRENT_BINARY_DIR}/kartpad-romfs")
file(COPY "${RT}/assets/wii" DESTINATION "${_KARTPAD_ROMFS}")
# The Wii DSP coefficient table (public audio-DSP constants, 4 KiB) is required
# by the AX HLE mixer during OS::Init. The desktop build copies it next to the
# executable; Horizon has no executable dir, so pack it into ROMFS and let
# FindDspCoefficientRom pick it up from romfs:/dsp/dsp_coef.bin.
file(COPY "${RT}/assets/dsp" DESTINATION "${_KARTPAD_ROMFS}")
# Native code and binding metadata collected from earlier console sessions.
# Use an explicit build input so updating the pack re-runs packaging.
set(KARTPAD_DEKO_SEED "${_repo}/private/deko-training/initial_dksh_cache.bin" CACHE FILEPATH "Validated native shader training pack")
if(EXISTS "${KARTPAD_DEKO_SEED}")
  file(MAKE_DIRECTORY "${_KARTPAD_ROMFS}/deko")
  configure_file("${KARTPAD_DEKO_SEED}" "${_KARTPAD_ROMFS}/deko/initial_dksh_cache.bin" COPYONLY)
else()
  file(REMOVE "${_KARTPAD_ROMFS}/deko/initial_dksh_cache.bin")
endif()
# Pipeline recipes recorded on earlier console sessions (builder/deko-training.py pack-pipelines).
# Aurora merges them into the console's own recipes once per seed id and prewarms them at boot.
set(KARTPAD_PIPELINE_SEED "${_repo}/private/deko-training/initial_pipeline_cache.db" CACHE FILEPATH "Pipeline recipe seed")
get_filename_component(_pipeline_seed_dir "${KARTPAD_PIPELINE_SEED}" DIRECTORY)
get_filename_component(_pipeline_seed_stem "${KARTPAD_PIPELINE_SEED}" NAME_WE)
set(_pipeline_seed_id "${_pipeline_seed_dir}/${_pipeline_seed_stem}.id")
if(EXISTS "${KARTPAD_PIPELINE_SEED}" AND EXISTS "${_pipeline_seed_id}")
  file(MAKE_DIRECTORY "${_KARTPAD_ROMFS}/deko")
  configure_file("${KARTPAD_PIPELINE_SEED}" "${_KARTPAD_ROMFS}/deko/initial_pipeline_cache.db" COPYONLY)
  configure_file("${_pipeline_seed_id}" "${_KARTPAD_ROMFS}/deko/initial_pipeline_cache.id" COPYONLY)
else()
  file(REMOVE "${_KARTPAD_ROMFS}/deko/initial_pipeline_cache.db" "${_KARTPAD_ROMFS}/deko/initial_pipeline_cache.id")
endif()
nx_create_nro(kartpad_full OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${KARTPAD_NRO_NAME}"
  NACP kartpad-full.nacp ROMFS "${_KARTPAD_ROMFS}")

# Whole-program sample PGO (docs/PERF-60FPS-ROADMAP.md, "Sample PGO"): with KARTPAD_PGO_ALL the same
# profile also optimizes the runtime (HLE), the Switch shim, the renderer (kartpad_deko) and Aurora.
# Use a profile taken from a build with the current runtime/renderer/Aurora sources, or their line
# offsets will not match.
option(KARTPAD_PGO_ALL "Apply KARTPAD_PGO_PROFILE to runtime, shim and Aurora targets too" OFF)
# The renderer changes often (the v9 GPU policy, the transition guard): only a profile recorded with
# its current sources may optimize it, or its newest code has no samples and is compiled as cold.
option(KARTPAD_PGO_RENDERER "With KARTPAD_PGO_ALL, also apply the profile to the renderer (kartpad_deko)" OFF)
if(KARTPAD_PGO_PROFILE AND KARTPAD_PGO_ALL)
  message(STATUS "FullApp: sample PGO also for runtime, shim and Aurora")
  set(_pgo_renderer "")
  if(KARTPAD_PGO_RENDERER)
    set(_pgo_renderer kartpad_deko)
  endif()
  foreach(_pgo_target kartpad_full mkw_runtime_common kartpad_switch_shim ${_pgo_renderer} aurora_core aurora_gx aurora_gd
                      aurora_main aurora_os aurora_pad aurora_dvd aurora_mtx aurora_card aurora_ms aurora_vi aurora_si)
    if(TARGET ${_pgo_target})
      target_compile_options(${_pgo_target} PRIVATE "-fprofile-sample-use=${KARTPAD_PGO_PROFILE}")
      if(KARTPAD_SPLIT_FUNCTIONS)
        target_compile_options(${_pgo_target} PRIVATE -fsplit-machine-functions)
      endif()
    endif()
  endforeach()
endif()

# Profiling builds (builder/pgo-gen.py): line tables with profiling discriminators for every target
# the sample profile optimizes, so on-console PC samples (prof.bin) map back to source lines. Only
# the ELF carries them (elf2nro keeps the loaded segments) and Clang emits the same code either way.
# KARTPAD_PROF_RACE_SECONDS makes such a build record its first race seconds by itself
# (prof_switch.cpp), with the threads that run game, GX and renderer code.
option(KARTPAD_PROFILE_LINES "Line tables for sample-PGO profiling (ELF only)" OFF)
set(KARTPAD_PROF_RACE_SECONDS "0" CACHE STRING "Race seconds a build profiles by itself (0 = perf.cfg decides)")
if(KARTPAD_PROFILE_LINES)
  message(STATUS "FullApp: line tables for sample-PGO profiling")
  # And the boot-time TLB probe of the flat guest views (guest_flat_memory_switch.cpp).
  target_compile_definitions(mkw_runtime_common PRIVATE KARTPAD_TLB_PROBE_DEFAULT=1)
  foreach(_lines_target kartpad_full mkw_base_shared mkw_runtime_common kartpad_switch_shim kartpad_deko
                        mkw_retro_sensitive mkw_retro_rewind_functions aurora_core aurora_gx aurora_gd
                        aurora_main aurora_os aurora_pad aurora_dvd aurora_mtx aurora_card aurora_ms aurora_vi
                        aurora_si)
    if(TARGET ${_lines_target})
      target_compile_options(${_lines_target} PRIVATE -gline-tables-only -fdebug-info-for-profiling)
    endif()
  endforeach()
endif()
set(KARTPAD_PROF_RACE_START "0" CACHE STRING "Race seconds (across races) before that capture starts")
option(KARTPAD_PROF_RACE_EVERY "Capture every race to its own prof-race-NN.bin (start counted per race)" OFF)
if(NOT KARTPAD_PROF_RACE_SECONDS STREQUAL "0")
  target_compile_definitions(kartpad_switch_shim PRIVATE "KARTPAD_PROF_RACE_SECONDS=${KARTPAD_PROF_RACE_SECONDS}"
                             "KARTPAD_PROF_RACE_START=${KARTPAD_PROF_RACE_START}")
  if(KARTPAD_PROF_RACE_EVERY)
    target_compile_definitions(kartpad_switch_shim PRIVATE KARTPAD_PROF_RACE_EVERY=1)
  endif()
endif()
# The menus too (prof-menu.bin, prof_switch.cpp): from 20 s after start until a race runs, at most this long.
set(KARTPAD_PROF_MENU_SECONDS "0" CACHE STRING "Menu seconds a profiling build records by itself (0 = perf.cfg decides)")
if(NOT KARTPAD_PROF_MENU_SECONDS STREQUAL "0")
  target_compile_definitions(kartpad_switch_shim PRIVATE "KARTPAD_PROF_MENU_SECONDS=${KARTPAD_PROF_MENU_SECONDS}")
endif()

# AIO builder only (off by default): take the game-independent targets from prebuilt objects.
# Last, so every target's settings are final when they are compared.
include("${CMAKE_CURRENT_LIST_DIR}/Prebuilt.cmake")

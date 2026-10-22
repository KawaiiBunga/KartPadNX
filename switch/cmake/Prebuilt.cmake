# Prebuilt game-independent libraries for the AIO builder (aio/toolkit/make-prebuilt.py).
#
# Off by default: a normal build compiles every target from source and this file does nothing.
#
#   KARTPAD_PREBUILT_EXPORT=ON  writes ${CMAKE_BINARY_DIR}/prebuilt-export.cmake, a fingerprint of
#       each target in _KARTPAD_PREBUILT_TARGETS. make-prebuilt.py configures that way against a
#       stub translation (no game code), builds those targets and packs their objects with it.
#   KARTPAD_PREBUILT_LIBS_DIR=<dir>  (the prebuilt.cmake make-prebuilt.py wrote) replaces each
#       target's sources by its prebuilt objects when its fingerprint matches, so only the translated
#       game is compiled. The targets keep their names, usage requirements and link order; a target
#       whose fingerprint differs (other sources, flags or PGO profile) is compiled as usual.
#
# A fingerprint covers the target's sources and their per-source settings, its compile settings,
# those of every target it links (transitively), the directory's compile flags and the compiler
# version, with the source tree, build tree and devkitPro roots written as placeholders.
# Sources that include translator output (generated/RuntimeConfig.h) are never prebuilt:
# make-prebuilt.py finds them from the compiler's dependency files and lists them as
# _LOCAL_SOURCES, which are compiled here as part of the same target.
include_guard(GLOBAL)

set(_KARTPAD_PREBUILT_TARGETS
    mkw_runtime_common kartpad_switch_shim mkw_cryptopp mkw_pugixml
    aurora_core aurora_gx aurora_mtx aurora_os aurora_pad aurora_si aurora_vi
    kartpad_deko uamlib xxhash fmt imgui imgui_backends sqlite3 libzstd_static TracyClient)
# Targets whose objects (TARGET_OBJECTS) are linked straight into the executable.
set(_KARTPAD_PREBUILT_CONSUMERS kartpad_full)

option(KARTPAD_PREBUILT_EXPORT "Write prebuilt-export.cmake for aio/toolkit/make-prebuilt.py" OFF)
set(KARTPAD_PREBUILT_LIBS_DIR "" CACHE PATH "Prebuilt game-independent objects (AIO builder; empty = build all)")

get_filename_component(_kp_tree "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)

function(_kartpad_prebuilt_normalize text out)
  string(REPLACE "${CMAKE_BINARY_DIR}" "@BIN@" text "${text}")
  string(REPLACE "${_kp_tree}" "@TREE@" text "${text}")
  string(REPLACE "$ENV{DEVKITPRO}" "@DKP@" text "${text}")
  # Cross-directory link items carry a pointer value, ::@(0x...), different in every run.
  string(REGEX REPLACE "::@\\([0-9A-Fa-fx]+\\)" "::@()" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Compile-relevant usage requirements of a target and everything it links, depth first.
function(_kartpad_prebuilt_usage target out_text visited_var)
  set(_visited ${${visited_var}})
  set(_text "")
  get_target_property(_aliased ${target} ALIASED_TARGET)
  if(_aliased)
    set(target ${_aliased})
  endif()
  if(target IN_LIST _visited)
    set(${out_text} "" PARENT_SCOPE)
    return()
  endif()
  list(APPEND _visited ${target})
  string(APPEND _text "[${target}]\n")
  foreach(_prop INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_DEFINITIONS INTERFACE_INCLUDE_DIRECTORIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES INTERFACE_COMPILE_FEATURES INTERFACE_LINK_LIBRARIES)
    get_target_property(_v ${target} ${_prop})
    if(_v)
      string(APPEND _text "${_prop}=${_v}\n")
    endif()
  endforeach()
  get_target_property(_links ${target} INTERFACE_LINK_LIBRARIES)
  if(_links)
    foreach(_l IN LISTS _links)
      string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _l "${_l}")
      if(TARGET "${_l}")
        _kartpad_prebuilt_usage("${_l}" _sub _visited)
        string(APPEND _text "${_sub}")
      endif()
    endforeach()
  endif()
  set(${visited_var} ${_visited} PARENT_SCOPE)
  set(${out_text} "${_text}" PARENT_SCOPE)
endfunction()

function(_kartpad_prebuilt_fingerprint target out)
  get_target_property(_dir ${target} SOURCE_DIR)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _config)
  set(_text "")
  foreach(_lang C CXX ASM)
    get_directory_property(_flags DIRECTORY "${_dir}" DEFINITION CMAKE_${_lang}_FLAGS)
    get_directory_property(_cflags DIRECTORY "${_dir}" DEFINITION CMAKE_${_lang}_FLAGS_${_config})
    string(APPEND _text "${_lang}: ${CMAKE_${_lang}_COMPILER_ID} ${CMAKE_${_lang}_COMPILER_VERSION} "
           "${CMAKE_${_lang}_COMPILER_TARGET} | ${_flags} | ${_cflags}\n")
  endforeach()
  foreach(_prop TYPE SOURCES COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_FEATURES
                LINK_LIBRARIES C_STANDARD CXX_STANDARD C_EXTENSIONS CXX_EXTENSIONS
                POSITION_INDEPENDENT_CODE UNITY_BUILD)
    get_target_property(_v ${target} ${_prop})
    string(APPEND _text "${_prop}=${_v}\n")
  endforeach()
  get_target_property(_sources ${target} SOURCES)
  foreach(_src IN LISTS _sources)
    if(_src MATCHES "^\\$<")
      continue()
    endif()
    get_filename_component(_abs "${_src}" ABSOLUTE BASE_DIR "${_dir}")
    foreach(_prop LANGUAGE COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES)
      get_source_file_property(_v "${_abs}" TARGET_DIRECTORY ${target} ${_prop})
      if(_v)
        string(APPEND _text "${_src}:${_prop}=${_v}\n")
      endif()
    endforeach()
  endforeach()
  set(_visited ${target})
  get_target_property(_links ${target} LINK_LIBRARIES)
  if(_links)
    foreach(_l IN LISTS _links)
      if(TARGET "${_l}")
        _kartpad_prebuilt_usage("${_l}" _sub _visited)
        string(APPEND _text "${_sub}")
      endif()
    endforeach()
  endif()
  _kartpad_prebuilt_normalize("${_text}" _text)
  string(SHA256 _hash "${_text}")
  set(${out} ${_hash} PARENT_SCOPE)
  if(KARTPAD_PREBUILT_EXPORT OR KARTPAD_PREBUILT_LIBS_DIR)
    file(WRITE "${CMAKE_BINARY_DIR}/prebuilt-fingerprints/${target}.txt" "${_text}")
  endif()
endfunction()

if(KARTPAD_PREBUILT_EXPORT)
  set(_out "# Written by switch/cmake/Prebuilt.cmake (KARTPAD_PREBUILT_EXPORT).\n")
  foreach(_t IN LISTS _KARTPAD_PREBUILT_TARGETS)
    if(TARGET ${_t})
      _kartpad_prebuilt_fingerprint(${_t} _fp)
      get_target_property(_type ${_t} TYPE)
      string(APPEND _out "set(KARTPAD_PREBUILT_${_t}_TYPE ${_type})\n"
                         "set(KARTPAD_PREBUILT_${_t}_FINGERPRINT ${_fp})\n")
    endif()
  endforeach()
  file(WRITE "${CMAKE_BINARY_DIR}/prebuilt-export.cmake" "${_out}")
  message(STATUS "Prebuilt: fingerprints written to ${CMAKE_BINARY_DIR}/prebuilt-export.cmake")
endif()

if(KARTPAD_PREBUILT_LIBS_DIR)
  if(NOT EXISTS "${KARTPAD_PREBUILT_LIBS_DIR}/prebuilt.cmake")
    message(FATAL_ERROR "KARTPAD_PREBUILT_LIBS_DIR has no prebuilt.cmake: ${KARTPAD_PREBUILT_LIBS_DIR}")
  endif()
  include("${KARTPAD_PREBUILT_LIBS_DIR}/prebuilt.cmake")
  # Fingerprint everything before changing anything: targets read each other's settings.
  set(_kp_use)
  foreach(_t IN LISTS KARTPAD_PREBUILT_TARGETS)
    if(NOT TARGET ${_t})
      message(WARNING "Prebuilt: no target ${_t} in this build; its objects are not used")
      continue()
    endif()
    _kartpad_prebuilt_fingerprint(${_t} _fp)
    if(_fp STREQUAL KARTPAD_PREBUILT_${_t}_FINGERPRINT)
      list(APPEND _kp_use ${_t})
    else()
      message(WARNING "Prebuilt: ${_t} differs from the prebuilt objects; compiling it from source "
              "(settings in ${CMAKE_BINARY_DIR}/prebuilt-fingerprints/${_t}.txt)")
    endif()
  endforeach()
  foreach(_t IN LISTS _kp_use)
    set(_objects ${KARTPAD_PREBUILT_${_t}_OBJECTS})
    get_target_property(_type ${_t} TYPE)
    if(_type STREQUAL "STATIC_LIBRARY")
      set_property(TARGET ${_t} PROPERTY SOURCES ${_objects})
      set_property(TARGET ${_t} PROPERTY LINKER_LANGUAGE ${KARTPAD_PREBUILT_${_t}_LANGUAGE})
      # Object libraries whose objects the archive held are in the prebuilt objects already.
      foreach(_folded IN LISTS KARTPAD_PREBUILT_${_t}_FOLDED)
        foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
          get_target_property(_links ${_t} ${_prop})
          if(_links)
            list(REMOVE_ITEM _links ${_folded} "$<LINK_ONLY:${_folded}>")
            set_property(TARGET ${_t} PROPERTY ${_prop} "${_links}")
          endif()
        endforeach()
      endforeach()
    elseif(_type STREQUAL "OBJECT_LIBRARY")
      # The prebuilt objects go where the consumer took $<TARGET_OBJECTS>, followed by the
      # objects of the local sources (if any), which this target still compiles.
      set(_local)
      foreach(_src IN LISTS KARTPAD_PREBUILT_${_t}_LOCAL_SOURCES)
        string(REPLACE "@TREE@" "${_kp_tree}" _src "${_src}")
        list(APPEND _local "${_src}")
      endforeach()
      set(_replacement ${_objects})
      if(_local)
        set_property(TARGET ${_t} PROPERTY SOURCES ${_local})
        list(APPEND _replacement "$<TARGET_OBJECTS:${_t}>")
      endif()
      foreach(_c IN LISTS _KARTPAD_PREBUILT_CONSUMERS)
        get_target_property(_csources ${_c} SOURCES)
        list(FIND _csources "$<TARGET_OBJECTS:${_t}>" _at)
        if(_at LESS 0)
          message(FATAL_ERROR "Prebuilt: ${_c} does not take $<TARGET_OBJECTS:${_t}>")
        endif()
        list(REMOVE_AT _csources ${_at})
        list(INSERT _csources ${_at} ${_replacement})
        set_property(TARGET ${_c} PROPERTY SOURCES "${_csources}")
      endforeach()
    endif()
  endforeach()
  list(LENGTH _kp_use _n_use)
  list(LENGTH KARTPAD_PREBUILT_TARGETS _n_all)
  message(STATUS "Prebuilt: ${_n_use} of ${_n_all} game-independent targets from ${KARTPAD_PREBUILT_LIBS_DIR}")
endif()

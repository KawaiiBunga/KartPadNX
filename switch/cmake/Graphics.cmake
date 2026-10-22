# Use the exact headers and ABI of the locally built Switch dependencies.
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(KARTPAD_DAWN_SOURCE "${_repo}/ref/dawn" CACHE PATH "Patched Dawn source")
set(KARTPAD_DAWN_BUILD "${_repo}/build/dawn-switch-deko" CACHE PATH "Switch Dawn Null/Tint build")
set(KARTPAD_SDL_SOURCE "${_repo}/ref/SDL" CACHE PATH "Patched SDL3 source")
set(KARTPAD_SDL_BUILD "${_repo}/build/sdl-switch-vulkan" CACHE PATH "External-graphics Switch SDL3 build")
set(KARTPAD_AURORA_SOURCE "${_repo}/build/wiicompiled-fpscr/aurora-main" CACHE PATH "Aurora source")
foreach(_archive IN ITEMS
    "${KARTPAD_SDL_BUILD}/libSDL3.a")
  if(NOT EXISTS "${_archive}")
    message(FATAL_ERROR "Build the Switch graphics dependency first: ${_archive}")
  endif()
endforeach()
add_library(dawn::webgpu_dawn STATIC IMPORTED GLOBAL)
set_target_properties(dawn::webgpu_dawn PROPERTIES
  IMPORTED_LOCATION "${KARTPAD_DAWN_BUILD}/src/dawn/native/libwebgpu_dawn.a"
  INTERFACE_INCLUDE_DIRECTORIES "${KARTPAD_DAWN_BUILD}/gen/include;${KARTPAD_DAWN_SOURCE}/include")
add_library(dawn::dawncpp_headers INTERFACE IMPORTED GLOBAL)
set_target_properties(dawn::dawncpp_headers PROPERTIES
  INTERFACE_LINK_LIBRARIES dawn::webgpu_dawn)
# Abseil is contained in monolithic Dawn; use its matching headers too.
foreach(_name flat_hash_map btree)
  add_library(absl::${_name} INTERFACE IMPORTED GLOBAL)
  set_target_properties(absl::${_name} PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${KARTPAD_DAWN_SOURCE}/third_party/abseil-cpp"
    INTERFACE_LINK_LIBRARIES dawn::webgpu_dawn)
endforeach()
add_library(SDL3::SDL3-static STATIC IMPORTED GLOBAL)
set_target_properties(SDL3::SDL3-static PROPERTIES
  IMPORTED_LOCATION "${KARTPAD_SDL_BUILD}/libSDL3.a"
  INTERFACE_INCLUDE_DIRECTORIES "${KARTPAD_SDL_SOURCE}/include")
set(Dawn_FOUND TRUE)
set(SDL3_FOUND TRUE)
set(AURORA_DAWN_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_SDL3_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL ON)
set(DAWN_ENABLE_VULKAN OFF)
set(BUILD_SHARED_LIBS OFF)
set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)
add_subdirectory("${KARTPAD_AURORA_SOURCE}" aurora)
include("${CMAKE_CURRENT_LIST_DIR}/Deko3D.cmake")
set_property(TARGET TracyClient PROPERTY SOURCES "${_repo}/switch/src/tracy_disabled_switch.cpp")
# The disabled Switch implementation has no dynamic-loader calls. Tracy's
# upstream target still inherits CMake's generic Unix libdl dependency, which
# does not exist on libnx. Preserve its other usage requirements.
foreach(_link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
  get_target_property(_tracy_links TracyClient ${_link_property})
  if(_tracy_links)
    list(REMOVE_ITEM _tracy_links dl)
    set_property(TARGET TracyClient PROPERTY ${_link_property} "${_tracy_links}")
  endif()
endforeach()
if(TARGET sqlite3)
  include("${KARTPAD_AURORA_SOURCE}/cmake/AuroraSwitchSQLite.cmake")
  aurora_configure_switch_sqlite(sqlite3)
endif()

# Native Maxwell renderer, adapted from SwitchWaker 04372277d03f.
set(_dk "${_repo}/switch/deko")
set(NX_ROOT "$ENV{DEVKITPRO}/libnx")
add_subdirectory("${_repo}/switch/uam" "${CMAKE_BINARY_DIR}/uam" EXCLUDE_FROM_ALL)
file(GLOB _glsl "${_dk}/shaders/*.glsl")
set(_dksh)
foreach(_shader IN LISTS _glsl)
  get_filename_component(_name "${_shader}" NAME_WE)
  if(_name MATCHES "_vsh$")
    set(_stage vert)
  else()
    set(_stage frag)
  endif()
  nx_add_shader_program(kartpad_${_name} "${_shader}" ${_stage} OUTPUT "${CMAKE_BINARY_DIR}/dksh/${_name}.dksh")
  list(APPEND _dksh kartpad_${_name})
endforeach()
dkp_add_embedded_binary_library(kartpad_deko_builtin ${_dksh})
add_library(kartpad_deko STATIC
  "${_dk}/device.cpp" "${_dk}/memory.cpp" "${_dk}/descriptors.cpp"
  "${_dk}/pattern.cpp" "${_dk}/shader_cache.cpp"
  "${_dk}/shaders.cpp" "${_dk}/shader_translate.cpp" "${_dk}/dksh_file.cpp"
  "${_dk}/aurora/dk_objects.cpp"
  "${_dk}/backend_objects.cpp" "${_dk}/backend_pipelines.cpp" "${_dk}/backend_commands.cpp"
  "${_dk}/backend_present.cpp")
target_include_directories(kartpad_deko PUBLIC "${_dk}" PRIVATE
  "${KARTPAD_DAWN_SOURCE}" "${KARTPAD_DAWN_BUILD}/gen" "${_dk}/aurora"
  "$ENV{DEVKITPRO}/portlibs/switch/include")
target_compile_definitions(kartpad_deko PRIVATE
  COS_SWITCH_NRO_DIR="/switch/kartpad" COS_SWITCH_DATA_DIR="sdmc:/switch/kartpad")
option(KARTPAD_DEKO_DEBUG "Validate native deko3d API calls" OFF)
if(KARTPAD_DEKO_DEBUG)
  set(_deko_library deko3dd)
  target_compile_definitions(kartpad_deko PRIVATE COS_DK_DEBUG_LIB=1)
else()
  set(_deko_library deko3d)
endif()
target_link_libraries(kartpad_deko PRIVATE kartpad_deko_builtin xxhash uamlib
  PUBLIC dawn::webgpu_dawn ${_deko_library} z nx)
target_compile_definitions(aurora_core PUBLIC AURORA_GFX_DEKO3D=1)
target_include_directories(aurora_core PRIVATE "${_dk}")
target_include_directories(aurora_gx PRIVATE "${_dk}")
target_link_libraries(aurora_core PUBLIC kartpad_deko)

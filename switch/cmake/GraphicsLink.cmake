# Final-executable link contract of the game NRO: TLS and libc shims,
# pthread/exit wraps, the deko3d wrap list, and the Dawn/Aurora/libnx link group.
include_guard(GLOBAL)
set(KARTPAD_GUEST_WRAP_SYMBOLS
    func_801B3BAC func_80199D64 func_80199E30 func_8019A91C func_8019A970
    func_80062C50 func_80064FD0
    func_80035340 func_800353B0 func_800365D0 func_80041D10
    func_807BE030 func_80199D30 func_8006E6F0 func_80052FD0 func_8006D6E0
    func_80055540)

# NintendoSwitch is a custom platform, but its GCC/binutils linker supports
# archive rescanning. Aurora and the static driver have cyclic references.
set(CMAKE_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
set(CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)

file(GLOB_RECURSE _dawn_archives "${KARTPAD_DAWN_BUILD}/*.a")
list(REMOVE_ITEM _dawn_archives "${KARTPAD_DAWN_BUILD}/src/dawn/native/libwebgpu_dawn.a")
list(SORT _dawn_archives)

function(kartpad_link_switch_graphics target)
  # Objects, not a static shim archive: retain the early TLS constructor even
  # when no executable code explicitly references it. The libc shims come first in the link
  # order so they win over older copies in later archives.
  target_sources(${target} PRIVATE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/clang_tls_switch.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/libc_switch.c")
  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src"
    "$ENV{DEVKITPRO}/libnx/include" "$ENV{DEVKITPRO}/portlibs/switch/include")
  target_link_directories(${target} PRIVATE
    "$ENV{DEVKITPRO}/libnx/lib" "$ENV{DEVKITPRO}/portlibs/switch/lib")
  # PIE and switch.specs are already supplied by the NintendoSwitch platform's
  # executable flags. Repeating the specs file can repeat its linker script.
  # pthread_join frees the 8 MiB stack __wrap_pthread_create allocated, after
  # libnx's join has unmapped the stack mirror (clang_tls_switch.c).
  target_link_options(${target} PRIVATE
    "LINKER:--wrap=pthread_create" "LINKER:--wrap=pthread_join" "LINKER:--wrap=exit"
    "LINKER:--allow-multiple-definition"
    "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  # Diagnostic quarantine for the Retro WFC host heap corruption
  # (heap_guard_switch.c, docs/DEKO3D-TINT-CRASH-2026-10-17.md).
  option(KARTPAD_HEAP_GUARD "Quarantine and check freed small host heap blocks (diagnostic)" OFF)
  if(KARTPAD_HEAP_GUARD)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/heap_guard_switch.c")
    foreach(_symbol malloc free calloc realloc)
      target_link_options(${target} PRIVATE "LINKER:--wrap=${_symbol}")
    endforeach()
  endif()
  foreach(_symbol IN LISTS KARTPAD_GUEST_WRAP_SYMBOLS)
    target_link_options(${target} PRIVATE "LINKER:--wrap=${_symbol}")
  endforeach()
  file(STRINGS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../deko/wrap-symbols.txt" _deko_wraps)
  foreach(_symbol IN LISTS _deko_wraps)
    target_link_options(${target} PRIVATE "LINKER:--wrap=${_symbol}")
  endforeach()
  set(_libraries dawn::webgpu_dawn ${_dawn_archives}
    jpeg expat zstd z stdc++ nx sysbase c m gcc)
  list(JOIN _libraries "," _group)
  target_link_libraries(${target} PRIVATE "$<LINK_GROUP:RESCAN,${_group}>")
endfunction()

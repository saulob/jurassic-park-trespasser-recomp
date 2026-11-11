cmake_minimum_required(VERSION 3.20)
project(trespasser_recomp C)

if(NOT MSVC)
  message(FATAL_ERROR "pcrecomp's native32 runtime is MSVC x86 inline assembly: build with cl (see tools/build.ps1)")
endif()

# x64 (tools/build.ps1 -Arch x64): only the 64-bit host groundwork, which cannot run the game yet
# (docs/X64_MIGRATION.md). The playable host below stays 32-bit.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(PCRECOMP "${CMAKE_SOURCE_DIR}/external/pcrecomp" CACHE PATH "pcrecomp checkout (tools/fetch_pcrecomp.py)")
  add_library(host64 STATIC src/host64/guest_memory.c src/host64/guest_ptr.c src/host64/guest_thunks.c)
  target_include_directories(host64 PUBLIC src/host64 ${PCRECOMP}/runtime/win32hle)
  target_compile_options(host64 PRIVATE /W3)
  target_compile_definitions(host64 PRIVATE _CRT_SECURE_NO_WARNINGS)
  add_executable(trespasser_x64_probe src/host64/probe_main.c)
  target_link_libraries(trespasser_x64_probe PRIVATE host64)
  target_compile_options(trespasser_x64_probe PRIVATE /W3)
  target_compile_definitions(trespasser_x64_probe PRIVATE _CRT_SECURE_NO_WARNINGS
    TRESPASSER_DEFAULT_GAME_DIR="${CMAKE_SOURCE_DIR}/game")
  add_executable(x64_tests EXCLUDE_FROM_ALL tests/x64_tests.c)
  target_link_libraries(x64_tests PRIVATE host64)
  target_compile_options(x64_tests PRIVATE /W3)
  target_compile_definitions(x64_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  return()
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the playable host must be a 32-bit process: use the amd64_x86 toolchain or -A Win32 (see docs/BUILD.md)")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(FATAL_ERROR "use RelWithDebInfo or Release: Debug's /RTC1 cannot be combined with the lifted code's optimisation")
endif()

set(PCRECOMP "${CMAKE_SOURCE_DIR}/external/pcrecomp" CACHE PATH "pcrecomp checkout (tools/fetch_pcrecomp.py)")
set(TRESPASSER_GEN_OPT "/O2" CACHE STRING "optimisation for the lifted C (/Od compiles much faster)")
option(TRESPASSER_TRACE "build the lifted code with RECOMP_TRACE (--calltrace, --firsthit, ...)" OFF)

if(NOT EXISTS "${PCRECOMP}/runtime/native32/native32.c")
  message(FATAL_ERROR "pcrecomp not found at ${PCRECOMP}: run `py -3 tools/fetch_pcrecomp.py`")
endif()
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/recomp_*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty: run `py -3 tools/run_lift.py` first")
endif()

set(HOST_SOURCES
  src/host/main.c
  src/host/log.c
  src/host/fingerprint.c
  src/host/guest_env.c
  src/host/registry.c
  src/host/io_trace.c
  src/host/window_host.c
  src/host/guest_ui.c
  src/host/host_policy.c
  src/host/exit_policy.c
  src/host/milestones.c
  src/host/selfmod.c
  src/host/terrain_probe.c
  src/host/diagnostics.c
  src/host/ddraw_trace.c
  src/host/ddraw/dd_common.c
  src/host/ddraw/dd_device.c
  src/host/ddraw/dd_surface.c
  src/host/ddraw/dd_objects.c
  src/host/ddraw/dd_dispatch.c
  src/host/ddraw/d3d11_present.c
  src/host/ddraw/d3d11_renderer.c
  src/host/ddraw/d3d11_shaders.c
  src/host/ddraw/d3d_objects.c
  src/host/ddraw/d3d_translate.c
  src/host/ddraw/target_sync.c
  src/host/ddraw/terrain_diag.c
)
set(PCRECOMP_SOURCES
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
)

add_executable(trespasser ${HOST_SOURCES} ${PCRECOMP_SOURCES} ${GEN_SOURCES})
target_include_directories(trespasser PRIVATE
  src/host
  src/recomp/gen
  ${PCRECOMP}/runtime/native32
  ${PCRECOMP}/runtime/recomp32
)
target_compile_definitions(trespasser PRIVATE
  RECOMP_FLAT_MEMORY
  RECOMP_LOCAL_REGS
  TRESPASSER_DEFAULT_GAME_DIR="${CMAKE_SOURCE_DIR}/game"
  TRESPASSER_DEFAULT_LOG_DIR="${CMAKE_SOURCE_DIR}/logs"
)
if(TRESPASSER_TRACE)
  target_compile_definitions(trespasser PRIVATE RECOMP_TRACE=1)
endif()

# The guest image has no relocations and must sit at 0x00400000, so the host links high
# and keeps its own first stack small; lifted code runs on a thread with a large stack.
# Not /LARGEADDRESSAWARE: the original was a 2 GB process.
target_link_options(trespasser PRIVATE /BASE:0x60000000 /DYNAMICBASE:NO /NXCOMPAT /STACK:0x40000)
target_link_libraries(trespasser PRIVATE kernel32 user32 gdi32 advapi32 bcrypt lz32 d3d11 dxgi dxguid)

# TRESPASSER_GEN_OPT alone sets the lifted sources' optimisation: the configuration's /O
# options would only be overridden there (D9025), so they move to the other sources.
set(CONFIG_OPT "")
foreach(config Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${config} upper)
  string(REGEX MATCHALL "/O[^ ]+" flags "${CMAKE_C_FLAGS_${upper}}")
  string(REGEX REPLACE "/O[^ ]+" "" CMAKE_C_FLAGS_${upper} "${CMAKE_C_FLAGS_${upper}}")
  foreach(flag IN LISTS flags)
    list(APPEND CONFIG_OPT "$<$<CONFIG:${config}>:${flag}>")
  endforeach()
endforeach()

set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${TRESPASSER_GEN_OPT}")
set_source_files_properties(${PCRECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "/W1;${CONFIG_OPT}")
# native_bridge and gen_bridge save ebp, let the callee change it and restore it before any
# local is read again, so C4731 does not apply. native32.c defines _CRT_SECURE_NO_WARNINGS itself.
set_source_files_properties(${PCRECOMP}/runtime/native32/native32.c PROPERTIES
  COMPILE_OPTIONS "/W1;/wd4731;${CONFIG_OPT}")
set_source_files_properties(${HOST_SOURCES} PROPERTIES
  COMPILE_OPTIONS "/W3;${CONFIG_OPT}"
  COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS;TRESPASSER_LOG_DIR=\"${CMAKE_SOURCE_DIR}/build/logs\"")

# Headless checks of the host's logic and COM objects (no window, device or game):
#   ninja -C <build dir> host_tests com_tests && host_tests.exe && com_tests.exe
add_executable(host_tests EXCLUDE_FROM_ALL tests/host_tests.c src/host/host_policy.c src/host/ddraw/d3d_translate.c
  src/host/ddraw/target_sync.c src/host/ddraw/d3d11_shaders.c)
target_include_directories(host_tests PRIVATE src/host)
target_compile_options(host_tests PRIVATE /W3)
target_compile_definitions(host_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_executable(com_tests EXCLUDE_FROM_ALL tests/com_tests.c src/host/ddraw/dd_common.c src/host/ddraw/dd_device.c
  src/host/ddraw/dd_surface.c src/host/ddraw/dd_objects.c src/host/ddraw/d3d_objects.c src/host/ddraw/d3d_translate.c)
target_include_directories(com_tests PRIVATE src/host src/host/ddraw src/recomp/gen ${PCRECOMP}/runtime/native32
  ${PCRECOMP}/runtime/recomp32)
target_compile_options(com_tests PRIVATE /W3)
target_compile_definitions(com_tests PRIVATE _CRT_SECURE_NO_WARNINGS RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
target_link_libraries(com_tests PRIVATE user32 gdi32 dxguid)

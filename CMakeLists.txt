cmake_minimum_required(VERSION 3.20)
project(trespasser_recomp VERSION 0.1.0 LANGUAGES C CXX)

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
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(JPT_GEN_OPT_DEFAULT "/O2")
else()
  set(JPT_GEN_OPT_DEFAULT "/Od")
endif()
set(TRESPASSER_GEN_OPT "${JPT_GEN_OPT_DEFAULT}" CACHE STRING "optimisation for the lifted C (/Od: /O2 /Ob1, as the game needs)")
option(TRESPASSER_TRACE "build the lifted code with RECOMP_TRACE (--calltrace, --firsthit, ...)" OFF)

if(NOT EXISTS "${PCRECOMP}/runtime/native32/native32.c")
  message(FATAL_ERROR "pcrecomp not found at ${PCRECOMP}: run `py -3 tools/fetch_pcrecomp.py`")
endif()
file(STRINGS "${PCRECOMP}/runtime/recomp32/recomp_types.h" JPT_PCRECOMP_PATCHED REGEX "RECOMP_X87_RING_DEPTH" LIMIT_COUNT 1)
if(NOT JPT_PCRECOMP_PATCHED)
  message(FATAL_ERROR "pcrecomp at ${PCRECOMP} lacks patches/pcrecomp: run `py -3 tools/fetch_pcrecomp.py`")
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
  src/host/gamepad.c
  src/host/gamepad_menu.c
  src/host/gamepad_policy.c
  src/host/pad_glyphs.c
  src/host/guest_ui.c
  src/host/host_policy.c
  src/host/exit_policy.c
  src/host/milestones.c
  src/host/selfmod.c
  src/host/guest_overrides.c
  src/host/override_registry.c
  src/host/water_kernels.c
  src/host/occlusion_kernel.c
  src/host/polygon_submit.c
  src/host/widescreen.c
  src/host/game_text.c
  src/host/tutorial_prompts.c
  src/host/ui_canvas.c
  src/host/frontend_hd.c
  src/host/frontend_layout.c
  src/host/frontend_assets.c
  src/host/frontend_art.c
  src/host/loading_hd.c
  src/host/loading_layout.c
  src/host/video_hd.c
  src/host/video_layout.c
  src/host/video_test.c
  src/host/video_test_state.c
  src/host/video_test_panel.cpp
  src/host/debug_menu.c
  src/host/debug_menu_policy.c
  src/host/debug_menu_panel.cpp
  src/host/cheats.c
  src/host/cheats_policy.c
  src/host/guest_call.c
  src/host/save_preview.c
  src/host/engine_limits.c
  src/host/engine_heaps.c
  src/host/guest_profile.c
  src/host/texture_streaming.c
  src/host/terrain_quality.c
  src/host/cpu_polygons.c
  src/host/diagnostics.c
  src/host/physics_fixes.c
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
  src/host/ddraw/page_guard.c
  src/host/ddraw/page_arena.c
  src/host/ddraw/video_gpu.c
)
set(PCRECOMP_SOURCES
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
)

# Settings file, display arithmetic, monitors and user data: shared by the host, the launcher and the tests.
add_library(jpt_common STATIC src/common/jpt_config.c src/common/jpt_display.c src/common/jpt_gamepad.c
  src/common/jpt_prompt_text.c src/common/jpt_monitors.c src/common/jpt_user_data.c)
target_include_directories(jpt_common PUBLIC src/common)
target_compile_options(jpt_common PRIVATE /W4)
target_compile_definitions(jpt_common PRIVATE _CRT_SECURE_NO_WARNINGS)

# The launcher: TRG Launcher in an SDL3 window with an SDL renderer and Dear ImGui, as in the other ports
# (config/launcher_deps.txt, fetched and patched by tools/build.ps1); closed before the game creates its window.
# TRG's downloader is not built: no HTTP.
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/external/imgui" CACHE PATH "Dear ImGui checkout (config/launcher_deps.txt)")
set(TRG_DIR "${CMAKE_SOURCE_DIR}/external/trg-launcher" CACHE PATH "patched TRG Launcher checkout (config/launcher_deps.txt)")
set(SDL_DIR "${CMAKE_SOURCE_DIR}/external/SDL" CACHE PATH "SDL 3 checkout (config/launcher_deps.txt)")
if(NOT EXISTS "${TRG_DIR}/include/trg/launcher.h" OR NOT EXISTS "${IMGUI_DIR}/imgui.h" OR
   NOT EXISTS "${SDL_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "launcher sources missing: run tools/build.ps1, which fetches them (config/launcher_deps.txt)")
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)

# TRG Launcher (C++17: in C++20 u8string() is another type) and Dear ImGui with its SDL3 backends.
add_library(trg_core STATIC ${TRG_DIR}/src/settings.cpp ${TRG_DIR}/src/platform.cpp)
target_include_directories(trg_core PUBLIC ${TRG_DIR}/include)
target_link_libraries(trg_core PUBLIC shell32 ole32 comdlg32)
add_library(imgui_sdl STATIC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui_sdl PUBLIC ${IMGUI_DIR})
target_link_libraries(imgui_sdl PUBLIC SDL3::SDL3-static)
add_library(trg_ui STATIC ${TRG_DIR}/src/launcher.cpp ${TRG_DIR}/src/headers.cpp)
target_link_libraries(trg_ui PUBLIC trg_core imgui_sdl)
set_target_properties(trg_core imgui_sdl trg_ui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# The game's Video Test panel (src/host/video_test_panel.cpp): ImGui's D3D11 renderer over the core above.
add_library(imgui_dx11 STATIC ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_link_libraries(imgui_dx11 PUBLIC imgui_sdl d3d11)
set_target_properties(imgui_dx11 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# The sidebar's page icons (assets/launcher/icons/*.svg) as a table of byte arrays, page_icons.cpp.
file(GLOB JPT_PAGE_ICONS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/launcher/icons/*.svg)
list(SORT JPT_PAGE_ICONS)
set(JPT_PAGE_ICON_LIST ${CMAKE_BINARY_DIR}/generated/page_icons.txt)
file(CONFIGURE OUTPUT ${JPT_PAGE_ICON_LIST} CONTENT "${JPT_PAGE_ICONS}")
set(JPT_PAGE_ICON_SOURCE ${CMAKE_BINARY_DIR}/generated/page_icon_data.cpp)
add_custom_command(OUTPUT ${JPT_PAGE_ICON_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${JPT_PAGE_ICON_LIST} -DOUTPUT=${JPT_PAGE_ICON_SOURCE} -DSYMBOL=kPageIconFiles
          -P ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
  DEPENDS ${JPT_PAGE_ICON_LIST} ${JPT_PAGE_ICONS} ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
  COMMENT "Embedding the page icons")

# The port's launcher. The core (game files, disc image reader, installer, settings, page model, icons) needs no window;
# the tests link only it.
add_library(jpt_launcher_core STATIC launcher/iso9660.cpp launcher/game_files.cpp launcher/game_install.cpp
  launcher/settings.cpp launcher/system_info.cpp launcher/options.cpp launcher/header_art.cpp launcher/page_icons.cpp
  launcher/launcher_log.cpp ${JPT_PAGE_ICON_SOURCE})
target_include_directories(jpt_launcher_core PUBLIC launcher)
target_link_libraries(jpt_launcher_core PUBLIC jpt_common trg_core bcrypt d3d11 dxgi)
add_library(jpt_launcher STATIC launcher/launcher.cpp launcher/host.cpp)
target_link_libraries(jpt_launcher PUBLIC jpt_launcher_core trg_ui)
target_compile_definitions(jpt_launcher PRIVATE JPT_PORT_VERSION="${PROJECT_VERSION}")
foreach(lib trg_core imgui_sdl trg_ui jpt_launcher_core jpt_launcher)
  target_compile_definitions(${lib} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  target_compile_options(${lib} PRIVATE /utf-8)
endforeach()
target_compile_options(jpt_launcher_core PRIVATE /W4)
target_compile_options(jpt_launcher PRIVATE /W4)
# Vendored nanosvg: C4702 is reported at code generation, past its include's warning pragma.
set_source_files_properties(launcher/page_icons.cpp PROPERTIES COMPILE_OPTIONS "/wd4702")
set_target_properties(jpt_launcher_core jpt_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The lifted x87 stack is a pointer into a long buffer (pcrecomp's RECOMP_X87_RING; native32's support comes from
# patches/pcrecomp), and g_fp_top keeps the depth native32 hands the stack over by.
set(TRESPASSER_X87_MODEL RECOMP_X87_RING RECOMP_X87_RING_DEPTH)

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
  RECOMP_ICALL_SITE_RANGE
  ${TRESPASSER_X87_MODEL}
  TRESPASSER_DEFAULT_GAME_DIR="${CMAKE_SOURCE_DIR}/game"
  TRESPASSER_DEFAULT_USER_DIR="${CMAKE_SOURCE_DIR}/user"
  TRESPASSER_DEFAULT_LOG_DIR="${CMAKE_SOURCE_DIR}/logs"
)
if(TRESPASSER_TRACE)
  target_compile_definitions(trespasser PRIVATE RECOMP_TRACE=1)
endif()

# The launcher reads its images next to the executable; each copy depends on its source file, so an image-only change syncs.
set(JPT_LAUNCHER_ASSET_DIR "$<TARGET_FILE_DIR:trespasser>/assets/launcher")
set(JPT_LAUNCHER_ASSET_COPIES)
foreach(asset header-art.png sponsor-heart.png)
  set(asset_source "${CMAKE_SOURCE_DIR}/assets/launcher/${asset}")
  set(asset_stamp "${CMAKE_BINARY_DIR}/launcher-assets/${asset}.stamp")
  add_custom_command(OUTPUT "${asset_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JPT_LAUNCHER_ASSET_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${asset_source}" "${JPT_LAUNCHER_ASSET_DIR}/${asset}"
    COMMAND ${CMAKE_COMMAND} -E touch "${asset_stamp}"
    DEPENDS "${asset_source}"
    VERBATIM)
  list(APPEND JPT_LAUNCHER_ASSET_COPIES "${asset_stamp}")
endforeach()
add_custom_target(jpt_launcher_assets DEPENDS ${JPT_LAUNCHER_ASSET_COPIES})
add_dependencies(trespasser jpt_launcher_assets)

# HD loading art drawn for the port (assets/frontend/loading_hd/README.md), read next to the executable.
file(GLOB JPT_LOADING_ART CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/frontend/loading_hd/*.png
  ${CMAKE_SOURCE_DIR}/assets/frontend/loading_hd/*.jpg)
set(JPT_LOADING_ART_COPIES)
foreach(art_source IN LISTS JPT_LOADING_ART)
  get_filename_component(art_name "${art_source}" NAME)
  set(art_stamp "${CMAKE_BINARY_DIR}/loading-art/${art_name}.stamp")
  add_custom_command(OUTPUT "${art_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:trespasser>/assets/frontend/loading_hd"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${art_source}" "$<TARGET_FILE_DIR:trespasser>/assets/frontend/loading_hd/${art_name}"
    COMMAND ${CMAKE_COMMAND} -E touch "${art_stamp}"
    DEPENDS "${art_source}"
    VERBATIM)
  list(APPEND JPT_LOADING_ART_COPIES "${art_stamp}")
endforeach()
if(JPT_LOADING_ART_COPIES)
  add_custom_target(jpt_loading_art DEPENDS ${JPT_LOADING_ART_COPIES})
  add_dependencies(trespasser jpt_loading_art)
endif()

# The guest image has no relocations and must sit at 0x00400000, so the host links high
# and keeps its own first stack small; lifted code runs on a thread with a large stack.
# Not /LARGEADDRESSAWARE: the original was a 2 GB process.
target_link_options(trespasser PRIVATE /BASE:0x60000000 /DYNAMICBASE:NO /NXCOMPAT /STACK:0x40000)
# Release links without incremental-link thunks: overrides go through stable entries (src/host/guest_override_list.h).
# No identical-code folding: distinct functions keep distinct addresses, as in the RelWithDebInfo build.
target_link_options(trespasser PRIVATE $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:NOICF>)
target_link_libraries(trespasser PRIVATE jpt_launcher jpt_common imgui_dx11 SDL3::SDL3-static kernel32 user32 gdi32 msimg32 advapi32 bcrypt lz32 d3d11
  dxgi dxguid ole32 uuid windowscodecs delayimp)
# ImGui's D3D11 renderer imports D3DCompile; the renderer loads d3dcompiler_47.dll itself, so start-up does not need it.
target_link_options(trespasser PRIVATE /DELAYLOAD:d3dcompiler_47.dll)

# TRESPASSER_GEN_OPT alone sets the lifted sources' optimisation: the configuration's /O
# options would only be overridden there (D9025), so they move to the other sources.
# Release compiles them as RelWithDebInfo does (/O2 /Ob1), without its debug information.
set(CONFIG_OPT "")
foreach(config Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${config} upper)
  string(REGEX MATCHALL "/O[^ ]+" flags "${CMAKE_C_FLAGS_${upper}}")
  string(REGEX REPLACE "/O[^ ]+" "" CMAKE_C_FLAGS_${upper} "${CMAKE_C_FLAGS_${upper}}")
  if(config STREQUAL "Release")
    list(TRANSFORM flags REPLACE "^/Ob2$" "/Ob1")
  endif()
  foreach(flag IN LISTS flags)
    list(APPEND CONFIG_OPT "$<$<CONFIG:${config}>:${flag}>")
  endforeach()
endforeach()

# Every lifted source runs during gameplay (the game loop and its windows, scene, terrain, physics, AI, the renderer and
# the alias entries into them), so a /Od build compiles them optimised too. /Ob1 expands only the runtime's inline
# helpers (pops, x87 stack, flags, call resolution); overrides do not depend on it: a replaceable function is reached
# through its stable entry, which reads its target at run time wherever a call to it is expanded.
if(TRESPASSER_GEN_OPT STREQUAL "/Od")
  set(JPT_GEN_COMPILE "/w;/O2;/Ob1")
else()
  set(JPT_GEN_COMPILE "/w;${TRESPASSER_GEN_OPT};/Ob1")
endif()
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${JPT_GEN_COMPILE}")
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
# Launcher and display logic on synthetic disc images in temporary folders (headless):
#   ninja -C <build dir> launcher_tests && launcher_tests.exe
add_executable(launcher_tests EXCLUDE_FROM_ALL tests/launcher_tests.cpp)
target_link_libraries(launcher_tests PRIVATE jpt_launcher_core)
target_compile_definitions(launcher_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX JPT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(launcher_tests PRIVATE /W4 /utf-8)
set_target_properties(launcher_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The override registry and stable entries against a lifted fixture, linked both ways (headless):
#   ninja -C <build dir> override_tests_incremental override_tests_non_incremental, then run both
foreach(model incremental non_incremental)
  add_executable(override_tests_${model} EXCLUDE_FROM_ALL tests/override_tests.c tests/override_fixture.c
    src/host/override_registry.c)
  target_include_directories(override_tests_${model} PRIVATE src/host tests)
  target_compile_definitions(override_tests_${model} PRIVATE _CRT_SECURE_NO_WARNINGS)
endforeach()
target_compile_definitions(override_tests_incremental PRIVATE OVERRIDE_TESTS_INCREMENTAL=1)
target_compile_definitions(override_tests_non_incremental PRIVATE OVERRIDE_TESTS_INCREMENTAL=0)
target_link_options(override_tests_incremental PRIVATE /DEBUG /INCREMENTAL)
target_link_options(override_tests_non_incremental PRIVATE /INCREMENTAL:NO /OPT:REF /OPT:NOICF)
# The fixture as the lifted code is compiled; the test side lets the compiler expand the stable entries inline.
set_source_files_properties(tests/override_fixture.c PROPERTIES COMPILE_OPTIONS "/W3;/O2;/Ob1")
set_source_files_properties(tests/override_tests.c PROPERTIES COMPILE_OPTIONS "/W3;/O2;/Ob2")

add_executable(host_tests EXCLUDE_FROM_ALL tests/host_tests.c src/host/host_policy.c src/host/bump_spans.c src/host/ddraw/d3d_translate.c
  src/host/ddraw/target_sync.c src/host/ddraw/d3d11_shaders.c src/host/ddraw/page_arena.c src/common/jpt_display.c)
target_include_directories(host_tests PRIVATE src/host src/common)
target_compile_options(host_tests PRIVATE /W3)
target_compile_definitions(host_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The HD frontend's and loading screen's layout, pointer mapping and painter (headless: GDI draws into memory only):
#   ninja -C <build dir> frontend_tests && frontend_tests.exe [--dump <dir>]
add_executable(frontend_tests EXCLUDE_FROM_ALL tests/frontend_tests.c tests/frontend_bridge_tests.c
  tests/save_preview_tests.c tests/frontend_art_tests.c tests/frontend_retail_tests.c tests/loading_tests.c src/host/frontend_art.c src/host/frontend_hd.c src/host/frontend_layout.c
  src/host/frontend_assets.c src/host/loading_hd.c src/host/loading_layout.c src/host/save_preview.c
  src/host/host_policy.c src/host/pad_glyphs.c src/common/jpt_display.c src/common/jpt_gamepad.c
  src/common/jpt_prompt_text.c)
target_include_directories(frontend_tests PRIVATE src/host src/common ${PCRECOMP}/runtime/native32
  ${PCRECOMP}/runtime/recomp32)
target_compile_options(frontend_tests PRIVATE /W3)
target_compile_definitions(frontend_tests PRIVATE _CRT_SECURE_NO_WARNINGS RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS
  ${TRESPASSER_X87_MODEL} "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(frontend_tests PRIVATE gdi32 user32 ole32 uuid windowscodecs)
# The gamepad's decisions, bindings, glyphs and prompts (headless, no SDL device):
#   ninja -C <build dir> gamepad_tests && gamepad_tests.exe
add_executable(gamepad_tests EXCLUDE_FROM_ALL tests/gamepad_tests.c src/host/gamepad_policy.c src/host/pad_glyphs.c)
target_include_directories(gamepad_tests PRIVATE src/host)
target_link_libraries(gamepad_tests PRIVATE jpt_common)
target_compile_options(gamepad_tests PRIVATE /W3)
target_compile_definitions(gamepad_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The F5 Debug / Test Menu and F10 Cheats: their policies, the retail sites they use and the panel on a WARP device
# (headless): ninja -C <build dir> debug_menu_tests && debug_menu_tests.exe
add_executable(debug_menu_tests EXCLUDE_FROM_ALL tests/debug_menu_tests.c tests/cheats_tests.c
  src/host/debug_menu_policy.c src/host/cheats_policy.c src/host/debug_menu_panel.cpp)
target_include_directories(debug_menu_tests PRIVATE src/host src/common)
target_compile_options(debug_menu_tests PRIVATE /W3)
target_compile_definitions(debug_menu_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(debug_menu_tests PRIVATE imgui_dx11 d3d11 dxguid)
# The HD videos: the retail inventory, the original decoder (no audio track, no window), the layout and session, and the
# GPU passes on a WARP device (headless): ninja -C <build dir> video_tests && video_tests.exe
add_executable(video_tests EXCLUDE_FROM_ALL tests/video_tests.c src/host/video_layout.c src/host/ddraw/video_gpu.c
  src/host/video_test_state.c src/host/video_test_panel.cpp)
target_include_directories(video_tests PRIVATE src/host)
target_compile_options(video_tests PRIVATE /W3)
target_compile_definitions(video_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(video_tests PRIVATE imgui_dx11 d3d11 dxguid)
add_executable(com_tests EXCLUDE_FROM_ALL tests/com_tests.c src/host/ddraw/dd_common.c src/host/ddraw/dd_device.c
  src/host/ddraw/dd_surface.c src/host/ddraw/dd_objects.c src/host/ddraw/d3d_objects.c src/host/ddraw/d3d_translate.c
  src/host/ddraw/page_guard.c src/host/ddraw/page_arena.c)
target_include_directories(com_tests PRIVATE src/host src/host/ddraw src/recomp/gen ${PCRECOMP}/runtime/native32
  ${PCRECOMP}/runtime/recomp32)
target_compile_options(com_tests PRIVATE /W3)
target_compile_definitions(com_tests PRIVATE _CRT_SECURE_NO_WARNINGS RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS
  ${TRESPASSER_X87_MODEL})
target_link_libraries(com_tests PRIVATE user32 gdi32 dxguid)

# The patched pcrecomp runtime as the hot lifted code is compiled (x87 ring, inline rounding against the C library,
# per-site indirect-call cache), headless: ninja -C <build dir> x87_tests && x87_tests.exe
add_executable(x87_tests EXCLUDE_FROM_ALL tests/x87_tests.c)
target_include_directories(x87_tests PRIVATE ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(x87_tests PRIVATE _CRT_SECURE_NO_WARNINGS RECOMP_FLAT_MEMORY RECOMP_ICALL_SITE_RANGE
  ${TRESPASSER_X87_MODEL})
set_source_files_properties(tests/x87_tests.c PROPERTIES COMPILE_OPTIONS "/W3;/O2;/Ob1")

# Byte-exact comparison of the host water kernels with the lifted functions they replace, both compiled
# as in the game (headless): ninja -C <build dir> water_tests && water_tests.exe
# A second copy of the lifted code at the other optimisation level shows what the C compiler decides.
set(WATER_LIFTED "${CMAKE_BINARY_DIR}/water_lifted.c")
set(WATER_LIFTED_ALT "${CMAKE_BINARY_DIR}/water_lifted_alt.c")
foreach(copy IN ITEMS "lifted_|${WATER_LIFTED}" "lifted_alt_|${WATER_LIFTED_ALT}")
  string(REPLACE "|" ";" copy "${copy}")
  list(GET copy 0 prefix)
  list(GET copy 1 out)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} "-DGEN_DIR=${CMAKE_SOURCE_DIR}/src/recomp/gen" "-DFUNCS=sub_0060E3F0;sub_00638670;sub_00630D90;sub_00559240"
      "-DPREFIX=${prefix}" "-DOUT=${out}" -P "${CMAKE_SOURCE_DIR}/tools/extract_lifted.cmake"
    DEPENDS ${GEN_SOURCES} tools/extract_lifted.cmake
    COMMENT "Copying the lifted water functions for water_tests")
endforeach()
# The lifted callees both copies call by name: _alloca_probe and the _ftol thunk.
set(WATER_CALLEES "${CMAKE_BINARY_DIR}/water_callees.c")
add_custom_command(OUTPUT ${WATER_CALLEES}
  COMMAND ${CMAKE_COMMAND} "-DGEN_DIR=${CMAKE_SOURCE_DIR}/src/recomp/gen" "-DFUNCS=sub_00626BA0;sub_00626B92"
    "-DPREFIX=" "-DOUT=${WATER_CALLEES}" -P "${CMAKE_SOURCE_DIR}/tools/extract_lifted.cmake"
  DEPENDS ${GEN_SOURCES} tools/extract_lifted.cmake
  COMMENT "Copying the lifted callees for water_tests")
if(TRESPASSER_GEN_OPT STREQUAL "/Od")
  set(WATER_ALT_OPT "/O2")
else()
  set(WATER_ALT_OPT "/Od")
endif()
add_executable(water_tests EXCLUDE_FROM_ALL tests/water_tests.c src/host/water_kernels.c src/host/occlusion_kernel.c
  src/host/polygon_submit.c src/host/bump_spans.c
  ${WATER_LIFTED}
  ${WATER_LIFTED_ALT}
  ${WATER_CALLEES})
target_include_directories(water_tests PRIVATE src/host ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(water_tests PRIVATE _CRT_SECURE_NO_WARNINGS RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS
  ${TRESPASSER_X87_MODEL})
set_source_files_properties(tests/water_tests.c PROPERTIES COMPILE_OPTIONS "/W3")
set_source_files_properties(${WATER_LIFTED} PROPERTIES COMPILE_OPTIONS "/w;${TRESPASSER_GEN_OPT}")
set_source_files_properties(${WATER_LIFTED_ALT} PROPERTIES COMPILE_OPTIONS "/w;${WATER_ALT_OPT}")
set_source_files_properties(${WATER_CALLEES} PROPERTIES COMPILE_OPTIONS "/w;${TRESPASSER_GEN_OPT}")
target_compile_definitions(water_tests PRIVATE "WATER_ALT_OPT=\"${WATER_ALT_OPT}\"" "WATER_GEN_OPT=\"${TRESPASSER_GEN_OPT}\""
  "WATER_GAME_EXE=\"${CMAKE_SOURCE_DIR}/game/trespass.exe\"")
# The lifted code addresses guest memory directly: a fixed image puts tests/water_tests.c's window there.
target_link_options(water_tests PRIVATE /BASE:0x00400000 /DYNAMICBASE:NO /FIXED)

# The mouse and the gamepad through the lifted CInput read with the host's per-frame steps around it, compared frame
# by frame (headless): ninja -C <build dir> input_tests && input_tests.exe
set(INPUT_LIFTED "${CMAKE_BINARY_DIR}/input_lifted.c")
add_custom_command(OUTPUT ${INPUT_LIFTED}
  COMMAND ${CMAKE_COMMAND} "-DGEN_DIR=${CMAKE_SOURCE_DIR}/src/recomp/gen" "-DFUNCS=sub_005F80B0"
    "-DPREFIX=lifted_" "-DOUT=${INPUT_LIFTED}" -P "${CMAKE_SOURCE_DIR}/tools/extract_lifted.cmake"
  DEPENDS ${GEN_SOURCES} tools/extract_lifted.cmake
  COMMENT "Copying the lifted CInput read for input_tests")
add_executable(input_tests EXCLUDE_FROM_ALL tests/input_tests.c src/host/gamepad_policy.c src/host/host_policy.c
  ${INPUT_LIFTED})
target_include_directories(input_tests PRIVATE src/host ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_link_libraries(input_tests PRIVATE jpt_common)
target_compile_definitions(input_tests PRIVATE _CRT_SECURE_NO_WARNINGS RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS
  ${TRESPASSER_X87_MODEL} "INPUT_GAME_EXE=\"${CMAKE_SOURCE_DIR}/game/trespass.exe\"")
set_source_files_properties(tests/input_tests.c PROPERTIES COMPILE_OPTIONS "/W3")
set_source_files_properties(${INPUT_LIFTED} PROPERTIES COMPILE_OPTIONS "/w;${TRESPASSER_GEN_OPT}")
target_link_options(input_tests PRIVATE /BASE:0x00400000 /DYNAMICBASE:NO /FIXED)

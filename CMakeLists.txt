cmake_minimum_required(VERSION 3.20)
project(trespasser_recomp VERSION 0.1.0 LANGUAGES C CXX)

if(UNIX AND NOT APPLE)
  include(cmake/JptLinux.cmake)
  return()
endif()

if(NOT MSVC)
  message(FATAL_ERROR "pcrecomp's native32 runtime is MSVC x86 inline assembly: build with cl (see tools/build.ps1)")
endif()

# x64 (tools/build.ps1 -Arch x64): only the 64-bit host groundwork, which cannot run the game yet
# (docs/X64_MIGRATION.md). The playable host below stays 32-bit.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(PCRECOMP "${CMAKE_SOURCE_DIR}/external/pcrecomp" CACHE PATH "pcrecomp checkout (tools/fetch_pcrecomp.py)")
  add_library(host64 STATIC src/host64/guest_memory.c src/host64/guest_ptr.c src/host64/guest_thunks.c src/host64/host_os_win.c)
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
  target_include_directories(x64_tests PRIVATE tests)
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
file(STRINGS "${PCRECOMP}/runtime/native32/native32.h" JPT_PCRECOMP_DISPATCH REGEX "native32_set_dispatch" LIMIT_COUNT 1)
file(STRINGS "${PCRECOMP}/runtime/native32/native32.h" JPT_PCRECOMP_HOLD REGEX "native32_hold_range" LIMIT_COUNT 1)
if(NOT JPT_PCRECOMP_PATCHED OR NOT JPT_PCRECOMP_DISPATCH OR NOT JPT_PCRECOMP_HOLD)
  message(FATAL_ERROR "pcrecomp at ${PCRECOMP} lacks patches/pcrecomp: run `py -3 tools/fetch_pcrecomp.py`")
endif()

# The one program carries every lifted build (English and Russian share lift116); each lift's symbols carry its prefix
# so they link together and the host chooses one at run time (src/host/lift_registry.c). The lifts
# are project artifacts (lifts/, tools/lifts.py): unpacked here, never generated from a game executable.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(JptLifts)
set(JPT_REPO_ROOT "${CMAKE_SOURCE_DIR}")
include(JptAudio)
jpt_unpack_lifts("${CMAKE_SOURCE_DIR}")
set(GEN_SOURCES_116 ${lift116_SOURCES})

set(HOST_SOURCES
  src/host/main.c
  src/host/log.c
  src/host/guest_env.c
  src/host/registry.c
  src/host/io_trace.c
  src/host/menu_audio_policy.c
  src/host/menu_audio.c
  ${JPT_AUDIO_PORTABLE_SOURCES}
  ${JPT_AUDIO_PLATFORM_SOURCES}
  src/host/window_host.c
  src/host/platform.c
  src/host/window_title.c
  src/host/text_layer.c
  src/host/gamepad.c
  src/host/pad_sdl.c
  src/host/gamepad_menu.c
  src/host/gamepad_ui.c
  src/host/gamepad_policy.c
  src/host/pad_glyphs.c
  src/host/guest_ui.c
  src/host/guest_ui_model.c
  src/host/host_policy.c
  src/host/exit_policy.c
  src/host/milestones.c
  src/host/selfmod.c
  src/host/perf_sections.c
  src/host/guest_overrides.c
  src/host/override_registry.c
  src/host/water_kernels.c
  src/host/occlusion_kernel.c
  src/host/polygon_submit.c
  src/host/widescreen.c
  src/host/fov_policy.c
  src/host/game_text.c
  src/host/text_layout.c
  src/host/l10n.c
  src/host/l10n_assets.cpp
  src/host/stream_pack.c
  src/host/stream_audio.c
  src/host/stream_vfile.c
  src/host/tutorial_prompts.c
  src/host/overlay_text.c
  src/host/ui_canvas.c
  src/host/frontend_hd.c
  src/host/frontend_layout.c
  src/host/frontend_assets.c
  src/host/frontend_art.c
  src/host/frontend_plaque.c
  src/host/loading_hd.c
  src/host/loading_layout.c
  src/host/video_hd.c
  src/host/video_layout.c
  src/host/video_subtitles.c
  src/host/video_test.c
  src/host/video_test_state.c
  src/host/video_test_panel.cpp
  src/host/debug_menu.c
  src/host/debug_menu_win.c
  src/host/guest_ui_actions.c
  src/host/guest_features.c
  src/host/skip_intro.c
  src/host/host_input.c
  src/host/debug_menu_policy.c
  src/host/debug_menu_layout.c
  src/host/debug_menu_panel.cpp
  src/host/imgui_software.cpp
  src/host/compass_policy.c
  src/host/minimap.c
  src/host/objective_policy.c
  src/host/objectives.c
  src/host/overlay_policy.c
  src/host/overlay.c
  src/host/physics_watch_policy.c
  src/host/physics_watch.c
  src/host/cheats.c
  src/host/cheats_policy.c
  src/host/guest_call.c
  src/host/save_preview.c
  src/host/engine_limits.c
  src/host/engine_heaps.c
  src/host/guest_profile.c
  src/host/active_profile.c
  src/host/lift_registry.c
  src/host/profiles/guest_profile_data.c
  src/host/profiles/guest_gsub_tables.c
  src/host/texture_streaming.c
  src/host/terrain_quality.c
  src/host/cpu_polygons.c
  src/host/texture_gpu_policy.c
  src/host/diagnostics.c
  src/host/physics_fixes.c
  src/host/ce_policy.c
  src/host/ce_gameplay.c
  src/host/ce_quick.c
  src/host/active_ai_policy.c
  src/host/active_ai.c
  src/host/patch117_policy.c
  src/host/patch117_objects.c
  src/host/patch117.c
  src/host/patch117_lz.c
  src/host/patch117_rules.c
  src/host/patch117_player.c
  src/host/patch117_physics.c
  src/host/ce_effects.c
  src/host/ce_graphics.c
  src/host/ddraw_trace.c
  src/host/ddraw/dd_common.c
  src/host/ddraw/dd_device.c
  src/host/ddraw/dd_surface.c
  src/host/ddraw/dd_objects.c
  src/host/ddraw/dd_dispatch.c
  src/host/ddraw/renderer_backend.c
  src/host/ddraw/vk_win_renderer.c
  src/host/ddraw/vk_win_account.c
  src/host/ddraw/vk_win_policy.c
  src/host/ddraw/d3d11_present.c
  src/host/ddraw/d3d11_renderer.c
  src/host/ddraw/d3d11_shaders.c
  src/host/ddraw/assao_host.cpp
  src/host/ddraw/assao_shader.cpp
  src/host/ao_policy.c
  src/host/ddraw/d3d_objects.c
  src/host/ddraw/d3d_translate.c
  src/host/ddraw/target_sync.c
  src/host/ddraw/page_guard.c
  src/host/ddraw/page_arena.c
  src/host/ddraw/video_gpu.c
  src/common/jpt_sync.c
)
set(PCRECOMP_SOURCES
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
)

# Settings file, display arithmetic, monitors and user data: shared by the host, the launcher and the tests.
add_library(jpt_common STATIC src/common/jpt_hash.c src/common/jpt_game_version.c src/common/jpt_lift_meta.c src/common/jpt_string_table.c src/common/jpt_restored_audio.c src/common/jpt_cheats.c src/common/jpt_config.c src/common/jpt_display.c src/common/jpt_gamepad.c src/common/jpt_gpu.c src/common/jpt_renderer.c src/common/jpt_present_policy.c
  src/common/jpt_prompt_text.c src/common/jpt_monitors.c src/common/jpt_user_data.c src/common/jpt_l10n.c src/common/jpt_tr.c src/common/jpt_fs_win.c
  src/common/jpt_video_subtitles.c)
target_include_directories(jpt_common PUBLIC src/common)
target_link_libraries(jpt_common PUBLIC d3d11 dxgi dxguid)
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

# Native Vulkan 1.2 backend (shared with Linux; dynamic loader via SDL3, pinned Khronos headers).
set(JPT_VULKAN_SOURCES
  src/vulkan/vk_policy.c src/vulkan/vk_loader.c src/vulkan/vk_alloc.c
  src/vulkan/vk_context.c src/vulkan/vk_swapchain.c src/vulkan/vk_shaders.c
  src/vulkan/vk_renderer.c)
add_library(jpt_vulkan STATIC ${JPT_VULKAN_SOURCES} src/host/ao_policy.c)
target_include_directories(jpt_vulkan PUBLIC src/vulkan)
target_include_directories(jpt_vulkan SYSTEM PUBLIC third_party/vulkan)
target_include_directories(jpt_vulkan PRIVATE src/host)
target_link_libraries(jpt_vulkan PUBLIC jpt_common SDL3::SDL3-static)
set_source_files_properties(${JPT_VULKAN_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4")
target_compile_definitions(jpt_vulkan PRIVATE _CRT_SECURE_NO_WARNINGS)

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
add_library(jpt_launcher_core STATIC launcher/iso9660.cpp launcher/inflate.cpp launcher/is_cabinet.cpp launcher/game_files.cpp launcher/game_install.cpp
  launcher/settings.cpp launcher/system_info.cpp launcher/options.cpp launcher/header_art.cpp launcher/page_icons.cpp
  launcher/launcher_log.cpp launcher/user_data.cpp launcher/localization/ui_text.cpp
  launcher/localization/pt_br_ui_text.cpp ${JPT_PAGE_ICON_SOURCE})
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

# Intel ASSAO (third_party/assao/README.md): the upstream DX11 effect, its shader compiled into the executable.
set(JPT_ASSAO_DIR "${CMAKE_SOURCE_DIR}/third_party/assao")
add_library(assao STATIC ${JPT_ASSAO_DIR}/ASSAODX11.cpp)
target_include_directories(assao PUBLIC ${JPT_ASSAO_DIR})
target_compile_definitions(assao PRIVATE NDEBUG ASSAO_SHADER_ERROR_HANDLER=assao_shader_error)
target_compile_options(assao PRIVATE /W1)
target_link_libraries(assao PUBLIC d3d11)
set(JPT_ASSAO_SHADER_LIST ${CMAKE_BINARY_DIR}/generated/assao_shader.txt)
file(CONFIGURE OUTPUT ${JPT_ASSAO_SHADER_LIST} CONTENT "${JPT_ASSAO_DIR}/ASSAO.hlsl")
set(JPT_ASSAO_SHADER_SOURCE ${CMAKE_BINARY_DIR}/generated/assao_shader_data.cpp)
add_custom_command(OUTPUT ${JPT_ASSAO_SHADER_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${JPT_ASSAO_SHADER_LIST} -DOUTPUT=${JPT_ASSAO_SHADER_SOURCE}
          -DSYMBOL=kAssaoShaderFiles -P ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
  DEPENDS ${JPT_ASSAO_SHADER_LIST} ${JPT_ASSAO_DIR}/ASSAO.hlsl ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
  COMMENT "Embedding ASSAO.hlsl")

# The translated text (assets/translations, made by tools/translation_pack.cpp) is part of the executable.
file(GLOB JPT_TRANSLATIONS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/translations/*.txt)
list(SORT JPT_TRANSLATIONS)
set(JPT_TRANSLATION_LIST ${CMAKE_BINARY_DIR}/generated/translations.txt)
file(CONFIGURE OUTPUT ${JPT_TRANSLATION_LIST} CONTENT "${JPT_TRANSLATIONS}")
set(JPT_TRANSLATION_SOURCE ${CMAKE_BINARY_DIR}/generated/translation_data.cpp)
add_custom_command(OUTPUT ${JPT_TRANSLATION_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${JPT_TRANSLATION_LIST} -DOUTPUT=${JPT_TRANSLATION_SOURCE}
          -DSYMBOL=kTranslationFiles -P ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
  DEPENDS ${JPT_TRANSLATION_LIST} ${JPT_TRANSLATIONS} ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
  COMMENT "Embedding the translated text")

# The restored audio (assets/restored_audio, made by tools/stream_fix_pack.cpp) is part of the executable too.
file(GLOB JPT_RESTORED_AUDIO CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/restored_audio/*)
list(SORT JPT_RESTORED_AUDIO)
set(JPT_RESTORED_AUDIO_LIST ${CMAKE_BINARY_DIR}/generated/restored_audio.txt)
file(CONFIGURE OUTPUT ${JPT_RESTORED_AUDIO_LIST} CONTENT "${JPT_RESTORED_AUDIO}")
set(JPT_RESTORED_AUDIO_SOURCE ${CMAKE_BINARY_DIR}/generated/restored_audio_data.cpp)
add_custom_command(OUTPUT ${JPT_RESTORED_AUDIO_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${JPT_RESTORED_AUDIO_LIST} -DOUTPUT=${JPT_RESTORED_AUDIO_SOURCE}
          -DSYMBOL=kRestoredAudioFiles -P ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
  DEPENDS ${JPT_RESTORED_AUDIO_LIST} ${JPT_RESTORED_AUDIO} ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
  COMMENT "Embedding the restored audio")

add_executable(trespasser_recomp ${HOST_SOURCES} ${PCRECOMP_SOURCES} ${GEN_SOURCES} ${JPT_ASSAO_SHADER_SOURCE} ${JPT_TRANSLATION_SOURCE} ${JPT_RESTORED_AUDIO_SOURCE})
target_include_directories(trespasser_recomp PRIVATE
  src/host
  ${PCRECOMP}/runtime/native32
  ${PCRECOMP}/runtime/recomp32
)
target_compile_definitions(trespasser_recomp PRIVATE
  RECOMP_FLAT_MEMORY
  RECOMP_LOCAL_REGS
  RECOMP_ICALL_SITE_RANGE
  ${TRESPASSER_X87_MODEL}
)
# Development builds use the source tree's game, logs and old user folders; a Release names no folder of this machine
# (tools\verify-release.ps1) and keeps game\ and logs\ next to the program.
target_compile_definitions(trespasser_recomp PRIVATE
  $<$<NOT:$<CONFIG:Release>>:TRESPASSER_DEFAULT_GAME_DIR="${CMAKE_SOURCE_DIR}/game">
  $<$<NOT:$<CONFIG:Release>>:TRESPASSER_LEGACY_USER_DIR="${CMAKE_SOURCE_DIR}/user">
  $<$<NOT:$<CONFIG:Release>>:TRESPASSER_LOG_DIR="${CMAKE_SOURCE_DIR}/build/logs">
)
if(TRESPASSER_TRACE)
  target_compile_definitions(trespasser_recomp PRIVATE RECOMP_TRACE=1)
endif()

# The launcher reads its images next to the executable; each copy depends on its source file, so an image-only change syncs.
set(JPT_LAUNCHER_ASSET_DIR "$<TARGET_FILE_DIR:trespasser_recomp>/assets/launcher")
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
add_dependencies(trespasser_recomp jpt_launcher_assets)

# HD loading art drawn for the port (assets/frontend/loading_hd/README.md), read next to the executable.
file(GLOB JPT_LOADING_ART CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/frontend/loading_hd/*.png
  ${CMAKE_SOURCE_DIR}/assets/frontend/loading_hd/*.jpg)
set(JPT_LOADING_ART_COPIES)
foreach(art_source IN LISTS JPT_LOADING_ART)
  get_filename_component(art_name "${art_source}" NAME)
  set(art_stamp "${CMAKE_BINARY_DIR}/loading-art/${art_name}.stamp")
  add_custom_command(OUTPUT "${art_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:trespasser_recomp>/assets/frontend/loading_hd"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${art_source}" "$<TARGET_FILE_DIR:trespasser_recomp>/assets/frontend/loading_hd/${art_name}"
    COMMAND ${CMAKE_COMMAND} -E touch "${art_stamp}"
    DEPENDS "${art_source}"
    VERBATIM)
  list(APPEND JPT_LOADING_ART_COPIES "${art_stamp}")
endforeach()
if(JPT_LOADING_ART_COPIES)
  add_custom_target(jpt_loading_art DEPENDS ${JPT_LOADING_ART_COPIES})
  add_dependencies(trespasser_recomp jpt_loading_art)
endif()

# The guest image has no relocations and must sit at 0x00400000, so the host links high
# and keeps its own first stack small; lifted code runs on a thread with a large stack.
# Not /LARGEADDRESSAWARE: the original was a 2 GB process.
target_link_options(trespasser_recomp PRIVATE /BASE:0x60000000 /DYNAMICBASE:NO /NXCOMPAT /STACK:0x40000)
# Release links without incremental-link thunks: overrides go through stable entries (src/host/guest_override_list.h).
# No identical-code folding: distinct functions keep distinct addresses, as in the RelWithDebInfo build.
target_link_options(trespasser_recomp PRIVATE $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:NOICF>)
target_link_libraries(trespasser_recomp PRIVATE assao jpt_vulkan jpt_launcher jpt_common imgui_dx11 SDL3::SDL3-static kernel32 user32 gdi32 msimg32 advapi32 bcrypt lz32 d3d11
  dxgi dxguid ole32 uuid windowscodecs delayimp)
# ImGui's D3D11 renderer imports D3DCompile; the renderer loads d3dcompiler_47.dll itself, so start-up does not need it.
target_link_options(trespasser_recomp PRIVATE /DELAYLOAD:d3dcompiler_47.dll)

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
# The C++ host sources keep the configuration's own /O and /Ob (CMAKE_CXX_FLAGS_<CONFIG> is not stripped as the C one is);
# adding CONFIG_OPT as well passed /Ob2 and /Ob1 together in Release (cl D9025).
set(HOST_CXX_SOURCES ${HOST_SOURCES})
list(FILTER HOST_CXX_SOURCES INCLUDE REGEX "[.]cpp$")
set(HOST_C_SOURCES ${HOST_SOURCES})
list(FILTER HOST_C_SOURCES EXCLUDE REGEX "[.]cpp$")
set_source_files_properties(${HOST_C_SOURCES} PROPERTIES COMPILE_OPTIONS "/W3;${CONFIG_OPT}")
set_source_files_properties(${HOST_CXX_SOURCES} PROPERTIES COMPILE_OPTIONS "/W3")
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS")

# The profile tables (guest addresses, patch groups, layouts, expected lift metadata) and the choice of the active
# profile, for the tests that read guest addresses (they call test_profile_activate, tests/test_profile.h).
add_library(jpt_profile STATIC src/host/active_profile.c src/host/profiles/guest_profile_data.c)
target_include_directories(jpt_profile PUBLIC src/host)
target_link_libraries(jpt_profile PUBLIC jpt_common)
target_compile_definitions(jpt_profile PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(jpt_profile PRIVATE /W3)

# Which lift each profile runs, its guest addresses, patch groups and layout, and the refusal of a stale lift (headless):
#   ninja -C <build dir> profile_dispatch_tests && profile_dispatch_tests.exe
add_executable(profile_dispatch_tests EXCLUDE_FROM_ALL tests/profile_dispatch_tests.c)
target_link_libraries(profile_dispatch_tests PRIVATE jpt_profile)
target_compile_definitions(profile_dispatch_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_compile_options(profile_dispatch_tests PRIVATE /W4)

# Headless checks of the host's logic and COM objects (no window, device or game):
#   ninja -C <build dir> host_tests com_tests && host_tests.exe && com_tests.exe
# Launcher and display logic on synthetic disc images in temporary folders (headless):
#   ninja -C <build dir> launcher_tests && launcher_tests.exe
add_executable(launcher_tests EXCLUDE_FROM_ALL tests/launcher_tests.cpp)
target_link_libraries(launcher_tests PRIVATE jpt_launcher_core)
target_compile_definitions(launcher_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX JPT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(launcher_tests PRIVATE /W4 /utf-8)
set_target_properties(launcher_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Text translations: the language choice, GameStrings, code pages, captions, DDF labels, the overlay's word wrap and the
# embedded text itself (headless): ninja -C <build dir> l10n_tests && l10n_tests.exe
add_executable(l10n_tests EXCLUDE_FROM_ALL tests/l10n_tests.c tests/l10n_coverage_tests.c src/host/text_layout.c src/host/l10n_assets.cpp
  ${JPT_TRANSLATION_SOURCE} ${JPT_RESTORED_AUDIO_SOURCE})
target_include_directories(l10n_tests PRIVATE src/host launcher)
target_link_libraries(l10n_tests PRIVATE jpt_common)
target_compile_options(l10n_tests PRIVATE /W3)
target_compile_definitions(l10n_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
set_target_properties(l10n_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
# The restored audio and the corrected English captions: the embedded records, the virtual Stream.tpa, the captions' timings in
# every language and the lint that keeps development paths out (headless): ninja -C <build dir> stream_tests && stream_tests.exe
add_executable(stream_tests EXCLUDE_FROM_ALL tests/stream_tests.c src/host/stream_pack.c src/host/stream_vfile.c src/host/l10n_assets.cpp
  ${JPT_TRANSLATION_SOURCE} ${JPT_RESTORED_AUDIO_SOURCE})
target_include_directories(stream_tests PRIVATE src/host launcher)
target_link_libraries(stream_tests PRIVATE jpt_common)
target_compile_options(stream_tests PRIVATE /W3)
target_compile_definitions(stream_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
set_target_properties(stream_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
# Developer tool: converts the extracted TresCom downloads into assets/translations (never run by the build):
#   ninja -C <build dir> translation_pack && translation_pack.exe <downloads folder> assets\translations
add_executable(translation_pack EXCLUDE_FROM_ALL tools/translation_pack.cpp)
target_link_libraries(translation_pack PRIVATE jpt_launcher_core)
target_compile_definitions(translation_pack PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(translation_pack PRIVATE /W4 /utf-8)
set_target_properties(translation_pack PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
# Developer tool: derives the English caption timings and the restored audio from the English Stream.tpa fix (never run by
# the build; the fix is not needed afterwards):
#   ninja -C <build dir> stream_fix_pack && stream_fix_pack.exe generate <retail Stream.tpa> <fixed Stream.tpa> assets
add_executable(stream_fix_pack EXCLUDE_FROM_ALL tools/stream_fix_pack.cpp)
target_link_libraries(stream_fix_pack PRIVATE jpt_common)
target_compile_definitions(stream_fix_pack PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(stream_fix_pack PRIVATE /W4 /utf-8)
set_target_properties(stream_fix_pack PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The portable user data root and its migration, in scratch folders only (headless):
#   ninja -C <build dir> user_data_tests && user_data_tests.exe
add_executable(user_data_tests EXCLUDE_FROM_ALL tests/user_data_tests.cpp)
target_link_libraries(user_data_tests PRIVATE jpt_launcher_core)
target_compile_definitions(user_data_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX JPT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(user_data_tests PRIVATE /W4 /utf-8)
set_target_properties(user_data_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

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

# Official Patch 1.1 (build 117) level fixes over scene images, fingerprints, setting and wiring (headless):
#   ninja -C <build dir> patch117_tests && patch117_tests.exe
add_executable(patch117_tests EXCLUDE_FROM_ALL tests/patch117_tests.c src/host/patch117_policy.c src/host/patch117_objects.c
  src/host/patch117_rules.c)
target_include_directories(patch117_tests PRIVATE src/host)
target_link_libraries(patch117_tests PRIVATE jpt_common lz32)
target_compile_options(patch117_tests PRIVATE /W3)
target_compile_definitions(patch117_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

add_executable(host_tests EXCLUDE_FROM_ALL tests/host_tests.c src/host/host_policy.c src/host/window_title.c src/host/bump_spans.c src/host/texture_gpu_policy.c src/host/ddraw/d3d_translate.c
  src/host/ddraw/target_sync.c src/host/ddraw/d3d11_shaders.c src/host/ddraw/page_arena.c src/common/jpt_display.c)
target_include_directories(host_tests PRIVATE src/host src/common)
target_compile_options(host_tests PRIVATE /W3)
target_compile_definitions(host_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The log's performance policy: statistics only with --perf-log, state and failure lines always (headless, scratch file):
#   ninja -C <build dir> log_tests && log_tests.exe
add_executable(log_tests EXCLUDE_FROM_ALL tests/log_tests.c src/host/log.c)
target_include_directories(log_tests PRIVATE src/host ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_link_libraries(log_tests PRIVATE jpt_common)
target_compile_options(log_tests PRIVATE /W3)
target_compile_definitions(log_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"" RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS
  ${TRESPASSER_X87_MODEL})
# Window presentation policy: the game's screen size changes the window only with its first presented frame (headless):
#   ninja -C <build dir> presentation_tests && presentation_tests.exe
add_executable(presentation_tests EXCLUDE_FROM_ALL tests/presentation_tests.c src/common/jpt_display.c)
target_include_directories(presentation_tests PRIVATE src/common)
target_compile_options(presentation_tests PRIVATE /W3)
target_compile_definitions(presentation_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
# The SDL3 platform layer's decisions (mouse motion after recaptures, message routing past SDL, SDL's lifetime, hotkeys,
# controller hotplug) and where the host starts, pumps and stops SDL (headless, no SDL):
#   ninja -C <build dir> platform_tests && platform_tests.exe
add_executable(platform_tests EXCLUDE_FROM_ALL tests/platform_tests.c src/host/host_policy.c src/host/gamepad_policy.c
  src/host/pad_glyphs.c src/host/overlay_policy.c)
target_include_directories(platform_tests PRIVATE src/host)
target_link_libraries(platform_tests PRIVATE jpt_common)
target_compile_options(platform_tests PRIVATE /W3)
target_compile_definitions(platform_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
# The menus' selection sound read from the retail executable, and the lookup of the file it is loaded from (headless):
#   ninja -C <build dir> menu_audio_tests && menu_audio_tests.exe
add_executable(menu_audio_tests EXCLUDE_FROM_ALL tests/menu_audio_tests.c src/host/menu_audio_policy.c src/common/jpt_hash.c)
target_include_directories(menu_audio_tests PRIVATE src/host src/common)
target_compile_options(menu_audio_tests PRIVATE /W3)
target_compile_definitions(menu_audio_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
# The audio path without a device: DirectSound volume and frequency semantics, the mixer on synthetic buffers, 3D math, the
# PCM formats of the game, and the rules that keep DirectSound out of the backend (headless, nothing is played):
#   ninja -C <build dir> audio_tests && audio_tests.exe
add_executable(audio_tests EXCLUDE_FROM_ALL tests/audio_tests.c src/host/audio/audio_policy.c src/host/audio/audio_mixer.c
  src/host/audio/audio_spatial.c src/host/menu_audio_policy.c)
target_include_directories(audio_tests PRIVATE src/host src/host/audio)
target_compile_options(audio_tests PRIVATE /W3)
target_compile_definitions(audio_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
# tests/linux_hle_tests.c, tests/linux_paths_tests.c, tests/linux_call_spine_tests.c (with tests/linux_fake_guest.c),
# tests/linux_guest_ptr_tests.c, tests/linux_module_tests.c, tests/linux_boot_tests.c, tests/linux_user32_tests.c,
# tests/linux_gdi_tests.c, tests/linux_ddraw_tests.c (these three with tests/linux_fake_guest.c and tests/linux_win32_harness.h) and
# tests/linux_present_tests.c, tests/linux_crash_tests.c, tests/linux_audio_stress_tests.c, tests/linux_shutdown_tests.c,
# tests/linux_stream_tests.c, tests/linux_save_files_tests.c, tests/linux_video_tests.c, tests/linux_session_tests.c,
# tests/linux_mouse_tests.c, tests/linux_video_subtitle_tests.c, tests/linux_gamepad_tests.c, tests/linux_cs_tests.c and tests/linux_sdl_window_tests.c are Linux-only: cmake/JptLinux.cmake builds them.
# Portable runtime pieces of the Linux host that also build here (headless): guest path resolution, the registry store,
# and synchronisation over SDL3: ninja -C <build dir> vpath_tests sync_tests reg_store_tests
add_executable(vpath_tests EXCLUDE_FROM_ALL tests/vpath_tests.c src/common/jpt_vpath.c src/common/jpt_fs_win.c)
target_include_directories(vpath_tests PRIVATE src/common)
add_executable(reg_store_tests EXCLUDE_FROM_ALL tests/reg_store_tests.c src/common/jpt_reg_store.c src/common/jpt_fs_win.c)
target_include_directories(reg_store_tests PRIVATE src/common)
add_executable(sync_tests EXCLUDE_FROM_ALL tests/sync_tests.c src/common/jpt_sync.c)
target_include_directories(sync_tests PRIVATE src/common)
target_link_libraries(sync_tests PRIVATE SDL3::SDL3-static)
foreach(portable_test vpath_tests reg_store_tests sync_tests)
  target_compile_options(${portable_test} PRIVATE /W3)
  target_compile_definitions(${portable_test} PRIVATE _CRT_SECURE_NO_WARNINGS)
endforeach()
# The queue policy between the mixer and the device against a virtual device and clock (steady, bursts, jitter, drift, 100 ms
# and 500 ms stalls, a late or early producer), and SDL's 22050 Hz conversion (headless): ninja -C <build dir> audio_pacer_tests
add_executable(audio_pacer_tests EXCLUDE_FROM_ALL tests/audio_pacer_tests.c src/host/audio/audio_pacer.c)
target_include_directories(audio_pacer_tests PRIVATE src/host/audio)
target_link_libraries(audio_pacer_tests PRIVATE SDL3::SDL3-static)
target_compile_options(audio_pacer_tests PRIVATE /W3)
target_compile_definitions(audio_pacer_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The platform-independent audio decisions (SDL3 driver policy per platform, the start-up log line), PCM streams and voices
# over caller-owned memory (headless, nothing is played): ninja -C <build dir> audio_portable_tests && audio_portable_tests.exe
add_executable(audio_portable_tests EXCLUDE_FROM_ALL tests/audio_portable_tests.c src/host/audio/audio_policy.c src/host/audio/audio_mixer.c)
target_include_directories(audio_portable_tests PRIVATE src/host/audio)
target_compile_options(audio_portable_tests PRIVATE /W3)
target_compile_definitions(audio_portable_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The real SDL3 backend on SDL's dummy driver: explicit and headless driver choice, the device thread pulling the mixer,
# rate change, reopen, an unknown driver (headless, no device, no sound): ninja -C <build dir> audio_backend_tests
add_executable(audio_backend_tests EXCLUDE_FROM_ALL tests/audio_backend_tests.c src/host/audio/sdl_audio.c src/host/audio/audio_mixer.c
  src/host/audio/audio_policy.c src/host/audio/audio_pacer.c)
target_include_directories(audio_backend_tests PRIVATE src/host/audio)
target_link_libraries(audio_backend_tests PRIVATE SDL3::SDL3-static)
target_compile_options(audio_backend_tests PRIVATE /W3)
target_compile_definitions(audio_backend_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The portable DirectSound layer through guest vtables over a mixer with no device, in guest memory that is not the host's
# (the same source runs on Linux): ninja -C <build dir> dsound_compat_tests && dsound_compat_tests.exe
add_executable(dsound_compat_tests EXCLUDE_FROM_ALL tests/dsound_compat_tests.c src/host/audio/dsound_compat.c src/host/audio/audio_policy.c
  src/host/audio/audio_mixer.c src/host/audio/audio_spatial.c src/host/audio/audio_sync.c)
target_include_directories(dsound_compat_tests PRIVATE src/host/audio)
target_link_libraries(dsound_compat_tests PRIVATE SDL3::SDL3-static)
target_compile_options(dsound_compat_tests PRIVATE /W3)
target_compile_definitions(dsound_compat_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# Voice-over playback as the retail game drives it for a Hammond narration: a looping 22050 Hz mono 16-bit stream filled from the
# play cursor at -5 dB, with music and effects, 3D, frequency and one-shot cases (synthetic audio, headless, nothing is played):
#   ninja -C <build dir> hammond_voiceover_tests && hammond_voiceover_tests.exe
add_executable(hammond_voiceover_tests EXCLUDE_FROM_ALL tests/hammond_voiceover_tests.c src/host/audio/dsound_compat.c src/host/audio/audio_policy.c
  src/host/audio/audio_mixer.c src/host/audio/audio_spatial.c src/host/audio/audio_sync.c)
target_include_directories(hammond_voiceover_tests PRIVATE src/host/audio)
target_link_libraries(hammond_voiceover_tests PRIVATE SDL3::SDL3-static)
target_compile_options(hammond_voiceover_tests PRIVATE /W3)
target_compile_definitions(hammond_voiceover_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The 32-bit Windows binding of that layer: the ABI against dsound.h, the stdcall entry points, the DSound.dll import gate
# (headless): ninja -C <build dir> dsound_bridge_tests && dsound_bridge_tests.exe
add_executable(dsound_bridge_tests EXCLUDE_FROM_ALL tests/dsound_bridge_tests.c tests/dsound_windows_abi.c ${JPT_AUDIO_WINDOWS_SOURCES}
  src/host/audio/dsound_compat.c src/host/audio/audio_policy.c src/host/audio/audio_mixer.c src/host/audio/audio_spatial.c src/host/audio/audio_sync.c)
target_include_directories(dsound_bridge_tests PRIVATE src/host/audio)
target_link_libraries(dsound_bridge_tests PRIVATE SDL3::SDL3-static)
target_compile_options(dsound_bridge_tests PRIVATE /W3)
target_compile_definitions(dsound_bridge_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The Smacker HLE: the 14 exports of SMACKW32.DLL the game imports over a synthetic decoder, a fake clock and the real mixer
# (headless, no game file, nothing is played): ninja -C <build dir> smacker_tests && smacker_tests.exe
add_executable(smacker_tests EXCLUDE_FROM_ALL tests/smacker_tests.c ${JPT_SMACKER_PORTABLE_SOURCES} src/host/audio/dsound_compat.c
  src/host/audio/audio_policy.c src/host/audio/audio_mixer.c src/host/audio/audio_spatial.c src/host/audio/audio_sync.c)
target_include_directories(smacker_tests PRIVATE src/host/audio src/host/video)
target_link_libraries(smacker_tests PRIVATE SDL3::SDL3-static)
target_compile_options(smacker_tests PRIVATE /W3)
target_compile_definitions(smacker_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The boundary of the FFmpeg Smacker backend (versions, licences, decoders, refusal of bad files). FFmpeg is found through
# pkg-config or -DFFMPEG_ROOT=; without it the suite reports DATA UNAVAILABLE: ninja -C <build dir> smacker_ffmpeg_tests
add_executable(smacker_ffmpeg_tests EXCLUDE_FROM_ALL tests/smacker_ffmpeg_tests.c ${JPT_SMACKER_PORTABLE_SOURCES} src/host/audio/audio_policy.c
  src/host/audio/audio_mixer.c src/host/audio/audio_spatial.c src/host/audio/audio_sync.c src/host/audio/dsound_compat.c)
target_include_directories(smacker_ffmpeg_tests PRIVATE src/host/audio src/host/video)
target_link_libraries(smacker_ffmpeg_tests PRIVATE SDL3::SDL3-static)
target_compile_options(smacker_ffmpeg_tests PRIVATE /W3)
target_compile_definitions(smacker_ffmpeg_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
jpt_smacker_use_ffmpeg(smacker_ffmpeg_tests)
# The HD frontend's and loading screen's layout, pointer mapping and painter (headless: GDI draws into memory only):
#   ninja -C <build dir> frontend_tests && frontend_tests.exe [--dump <dir>]
add_executable(frontend_tests EXCLUDE_FROM_ALL tests/frontend_tests.c tests/frontend_bridge_tests.c
  tests/save_preview_tests.c tests/frontend_art_tests.c tests/frontend_retail_tests.c tests/frontend_l10n_tests.c tests/loading_tests.c src/host/frontend_art.c src/host/frontend_plaque.c src/host/frontend_hd.c src/host/frontend_layout.c
  src/host/frontend_assets.c src/host/loading_hd.c src/host/loading_layout.c src/host/save_preview.c
  src/host/host_policy.c src/host/pad_glyphs.c src/common/jpt_display.c src/common/jpt_gamepad.c
  src/common/jpt_prompt_text.c src/common/jpt_l10n.c src/common/jpt_tr.c)
target_include_directories(frontend_tests PRIVATE src/host src/common ${PCRECOMP}/runtime/native32
  ${PCRECOMP}/runtime/recomp32)
target_compile_options(frontend_tests PRIVATE /W3)
target_compile_definitions(frontend_tests PRIVATE _CRT_SECURE_NO_WARNINGS RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS
  ${TRESPASSER_X87_MODEL} "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(frontend_tests PRIVATE gdi32 user32 ole32 uuid windowscodecs)
# The gamepad's decisions, bindings, glyphs and prompts (headless, no SDL device):
#   ninja -C <build dir> gamepad_tests && gamepad_tests.exe
add_executable(gamepad_tests EXCLUDE_FROM_ALL tests/gamepad_tests.c src/host/gamepad_policy.c src/host/pad_glyphs.c
  src/host/overlay_policy.c)
target_include_directories(gamepad_tests PRIVATE src/host)
target_link_libraries(gamepad_tests PRIVATE jpt_common)
target_compile_options(gamepad_tests PRIVATE /W3)
target_compile_definitions(gamepad_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# Quick Save / Quick Load, Dual stow, Water refraction and Enhanced sky's arithmetic and switches, their settings and
# keys (headless): ninja -C <build dir> ce_tests && ce_tests.exe
add_executable(ce_tests EXCLUDE_FROM_ALL tests/ce_tests.c src/host/ce_policy.c src/host/ce_effects.c src/host/host_policy.c)
target_include_directories(ce_tests PRIVATE src/host)
target_link_libraries(ce_tests PRIVATE jpt_common)
target_compile_options(ce_tests PRIVATE /W3)
target_compile_definitions(ce_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
# The input context read from the game's window stack over a fake guest memory, and the input state, relative mouse mode and
# pointer decisions that follow (headless, the same source runs on Linux): ninja -C <build dir> input_context_tests
add_executable(input_context_tests EXCLUDE_FROM_ALL tests/input_context_tests.c src/host/guest_ui_model.c src/host/host_policy.c)
target_include_directories(input_context_tests PRIVATE src/host)
target_link_libraries(input_context_tests PRIVATE jpt_common jpt_profile)
target_compile_options(input_context_tests PRIVATE /W3)
target_compile_definitions(input_context_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The port's hotkey table (key_policy): which keys are the port's, repeats, modifiers and the Quick keys (headless, the same
# source runs on Linux): ninja -C <build dir> hotkey_tests
add_executable(hotkey_tests EXCLUDE_FROM_ALL tests/hotkey_tests.c src/host/host_policy.c)
target_include_directories(hotkey_tests PRIVATE src/host)
target_link_libraries(hotkey_tests PRIVATE jpt_common jpt_profile)
target_compile_options(hotkey_tests PRIVATE /W3)
target_compile_definitions(hotkey_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# Active AI: limits, loop orders, the physics box guard, the save block and the retail lint (headless):
#   ninja -C <build dir> active_ai_tests && active_ai_tests.exe
add_executable(active_ai_tests EXCLUDE_FROM_ALL tests/active_ai_tests.c src/host/active_ai_policy.c)
target_include_directories(active_ai_tests PRIVATE src/host)
target_compile_options(active_ai_tests PRIVATE /W3)
target_compile_definitions(active_ai_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
# ps_water and ps_sky drawn offscreen on a WARP device, and the MSAA scene copy (headless, no window):
#   ninja -C <build dir> ce_gpu_tests && ce_gpu_tests.exe
add_executable(ce_gpu_tests EXCLUDE_FROM_ALL tests/ce_gpu_tests.c src/host/ddraw/d3d11_shaders.c src/host/ce_effects.c)
target_include_directories(ce_gpu_tests PRIVATE src/host src/host/ddraw)
target_compile_options(ce_gpu_tests PRIVATE /W3)
target_compile_definitions(ce_gpu_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ce_gpu_tests PRIVATE d3d11 dxguid)
# Field of view and mouse sensitivity: settings, the camera's arithmetic per aspect ratio, the mouse's counts and where
# each value is written (headless):
#   ninja -C <build dir> fov_mouse_tests && fov_mouse_tests.exe
add_executable(fov_mouse_tests EXCLUDE_FROM_ALL tests/fov_mouse_tests.c src/host/fov_policy.c
  src/host/host_policy.c)
target_include_directories(fov_mouse_tests PRIVATE src/host)
target_link_libraries(fov_mouse_tests PRIVATE jpt_common)
target_compile_options(fov_mouse_tests PRIVATE /W3)
target_compile_definitions(fov_mouse_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
# F10 Cheats, F3 Objectives, the compass and the minimap: their policies, the retail sites and level data
# they use and the panel on a WARP device (headless): ninja -C <build dir> debug_menu_tests && debug_menu_tests.exe
add_executable(debug_menu_tests EXCLUDE_FROM_ALL tests/debug_menu_tests.c tests/cheats_tests.c tests/compass_tests.c
  tests/minimap_tests.c tests/objective_tests.c tests/overlay_tests.c tests/menu_l10n_tests.c tests/menu_layout_tests.c src/host/debug_menu_policy.c
  src/host/debug_menu_layout.c src/host/cheats_policy.c src/host/text_layer.c tests/text_layer_tests.c
  src/host/compass_policy.c src/host/objective_policy.c src/host/overlay_policy.c src/host/physics_watch_policy.c
  src/host/minimap.c src/host/host_policy.c tests/physics_watch_tests.c src/host/debug_menu_panel.cpp
  src/host/imgui_software.cpp)
target_include_directories(debug_menu_tests PRIVATE src/host src/common)
target_compile_options(debug_menu_tests PRIVATE /W3)
target_compile_definitions(debug_menu_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(debug_menu_tests PRIVATE jpt_common imgui_dx11 d3d11 dxguid)
# Alt+Tab during a video: the lifted facts that keep a video from playing under a live game window (headless):
#   ninja -C <build dir> video_focus_tests && video_focus_tests.exe
add_executable(video_focus_tests EXCLUDE_FROM_ALL tests/video_focus_tests.c)
target_compile_options(video_focus_tests PRIVATE /W3)
target_compile_definitions(video_focus_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

# Video subtitles: timelines, fingerprints, languages, setting, layout and wiring (headless):
#   ninja -C <build dir> video_subtitle_tests && video_subtitle_tests.exe
add_executable(video_subtitle_tests EXCLUDE_FROM_ALL tests/video_subtitle_tests.c src/host/video_layout.c)
target_include_directories(video_subtitle_tests PRIVATE src/host src/common)
target_link_libraries(video_subtitle_tests PRIVATE jpt_common)
target_compile_options(video_subtitle_tests PRIVATE /W3 /utf-8)
target_compile_definitions(video_subtitle_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

# The HD videos: the retail inventory, the original decoder (no audio track, no window), the layout and session, and the
# GPU passes on a WARP device (headless): ninja -C <build dir> video_tests && video_tests.exe
add_executable(video_tests EXCLUDE_FROM_ALL tests/video_tests.c src/host/video_layout.c src/host/ddraw/video_gpu.c
  src/host/video_test_state.c src/host/video_test_panel.cpp)
target_include_directories(video_tests PRIVATE src/host src/common)
target_compile_options(video_tests PRIVATE /W3)
target_compile_definitions(video_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(video_tests PRIVATE jpt_common imgui_dx11 d3d11 dxguid)
# Ambient occlusion: presets, projection and frame policy, then the ASSAO wrapper's lifecycle and passes on a WARP
# device (headless): ninja -C <build dir> ao_tests && ao_tests.exe
add_executable(ao_tests EXCLUDE_FROM_ALL tests/ao_tests.cpp src/host/ao_policy.c src/host/ddraw/assao_host.cpp)
target_include_directories(ao_tests PRIVATE src/host src/host/ddraw)
target_compile_options(ao_tests PRIVATE /W3)
target_compile_definitions(ao_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(ao_tests PRIVATE assao jpt_common d3d11 dxguid)
add_executable(com_tests EXCLUDE_FROM_ALL tests/com_tests.c src/host/ce_effects.c src/host/ddraw/dd_common.c src/host/ddraw/dd_device.c
  src/host/ddraw/dd_surface.c src/host/ddraw/dd_objects.c src/host/ddraw/d3d_objects.c src/host/ddraw/d3d_translate.c
  src/host/ddraw/page_guard.c src/host/ddraw/page_arena.c src/host/text_layer.c)
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
    COMMAND ${CMAKE_COMMAND} "-DGEN_DIR=${CMAKE_SOURCE_DIR}/src/recomp/gen" "-DLIFT_PREFIX=lift116_" "-DFUNCS=sub_0060E3F0;sub_00638670;sub_00630D90;sub_00559240"
      "-DPREFIX=${prefix}" "-DOUT=${out}" -P "${CMAKE_SOURCE_DIR}/tools/extract_lifted.cmake"
    DEPENDS ${GEN_SOURCES_116} tools/extract_lifted.cmake
    COMMENT "Copying the lifted water functions for water_tests")
endforeach()
# The lifted callees both copies call by name: _alloca_probe and the _ftol thunk.
set(WATER_CALLEES "${CMAKE_BINARY_DIR}/water_callees.c")
add_custom_command(OUTPUT ${WATER_CALLEES}
  COMMAND ${CMAKE_COMMAND} "-DGEN_DIR=${CMAKE_SOURCE_DIR}/src/recomp/gen" "-DLIFT_PREFIX=lift116_" "-DFUNCS=sub_00626BA0;sub_00626B92"
    "-DPREFIX=" "-DOUT=${WATER_CALLEES}" -P "${CMAKE_SOURCE_DIR}/tools/extract_lifted.cmake"
  DEPENDS ${GEN_SOURCES_116} tools/extract_lifted.cmake
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
target_compile_options(water_tests PRIVATE /W3)
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
  COMMAND ${CMAKE_COMMAND} "-DGEN_DIR=${CMAKE_SOURCE_DIR}/src/recomp/gen" "-DLIFT_PREFIX=lift116_" "-DFUNCS=sub_005F80B0"
    "-DPREFIX=lifted_" "-DOUT=${INPUT_LIFTED}" -P "${CMAKE_SOURCE_DIR}/tools/extract_lifted.cmake"
  DEPENDS ${GEN_SOURCES_116} tools/extract_lifted.cmake
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

foreach(host_test host_tests platform_tests ce_tests fov_mouse_tests input_tests frontend_tests gamepad_tests debug_menu_tests
        video_tests active_ai_tests patch117_tests water_tests com_tests ao_tests video_subtitle_tests)
  target_link_libraries(${host_test} PRIVATE jpt_profile)
  target_include_directories(${host_test} PRIVATE tests)
endforeach()

add_executable(vulkan_policy_tests EXCLUDE_FROM_ALL tests/vulkan_policy_tests.c)
target_link_libraries(vulkan_policy_tests PRIVATE jpt_vulkan)
target_compile_options(vulkan_policy_tests PRIVATE /W4)
target_compile_definitions(vulkan_policy_tests PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(vulkan_render_tests EXCLUDE_FROM_ALL tests/vulkan_render_tests.c)
target_link_libraries(vulkan_render_tests PRIVATE jpt_vulkan)
target_compile_options(vulkan_render_tests PRIVATE /W4)
target_compile_definitions(vulkan_render_tests PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(video_vulkan_tests EXCLUDE_FROM_ALL tests/video_vulkan_tests.c src/host/video_layout.c)
target_include_directories(video_vulkan_tests PRIVATE src/host src/common tests)
target_link_libraries(video_vulkan_tests PRIVATE jpt_vulkan)
target_compile_options(video_vulkan_tests PRIVATE /W3)
target_compile_definitions(video_vulkan_tests PRIVATE _CRT_SECURE_NO_WARNINGS "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

add_executable(renderer_benchmark_vulkan_tests EXCLUDE_FROM_ALL tests/renderer_benchmark.c tests/renderer_benchmark_vk_stubs.c
  src/host/ddraw/page_guard.c
  src/host/ddraw/vk_win_renderer.c src/host/ddraw/vk_win_account.c src/host/texture_gpu_policy.c
  src/host/ddraw/d3d_translate.c src/host/ddraw/target_sync.c src/host/ce_effects.c src/host/ao_policy.c src/host/text_layer.c)
target_include_directories(renderer_benchmark_vulkan_tests PRIVATE src/host src/host/ddraw src/common src/vulkan tests
  ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_link_libraries(renderer_benchmark_vulkan_tests PRIVATE jpt_vulkan jpt_common user32 gdi32)
target_compile_options(renderer_benchmark_vulkan_tests PRIVATE /W3)
target_compile_definitions(renderer_benchmark_vulkan_tests PRIVATE _CRT_SECURE_NO_WARNINGS BENCH_VULKAN)

add_executable(renderer_benchmark_d3d11_tests EXCLUDE_FROM_ALL tests/renderer_benchmark.c tests/renderer_benchmark_d3d11_stubs.c
  src/host/ddraw/d3d11_present.c src/host/ddraw/d3d11_renderer.c src/host/ddraw/d3d11_shaders.c
  src/host/ddraw/assao_host.cpp src/host/ddraw/assao_shader.cpp ${JPT_ASSAO_SHADER_SOURCE} src/host/ao_policy.c src/host/ce_effects.c
  src/host/texture_gpu_policy.c src/host/ddraw/d3d_translate.c src/host/ddraw/target_sync.c
  src/host/ddraw/page_guard.c src/host/ddraw/page_arena.c src/host/text_layer.c src/host/host_policy.c src/host/ddraw/dd_common.c)
target_include_directories(renderer_benchmark_d3d11_tests PRIVATE src/host src/host/ddraw src/common src/vulkan third_party/vulkan launcher tests
  ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_link_libraries(renderer_benchmark_d3d11_tests PRIVATE assao jpt_common user32 gdi32 d3d11 dxgi d3dcompiler dxguid)
target_compile_options(renderer_benchmark_d3d11_tests PRIVATE /W3)
target_compile_definitions(renderer_benchmark_d3d11_tests PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(vulkan_dispatch_tests EXCLUDE_FROM_ALL
  tests/vulkan_dispatch_tests.c
  src/host/ddraw/page_guard.c
  src/host/ddraw/renderer_backend.c
  src/host/ddraw/vk_win_renderer.c
  src/host/ddraw/vk_win_account.c
  src/host/texture_gpu_policy.c
  src/host/ddraw/d3d_translate.c
  src/host/ddraw/target_sync.c
  src/host/ce_effects.c
  src/host/ao_policy.c
  src/host/text_layer.c)
target_include_directories(vulkan_dispatch_tests PRIVATE src/host src/host/ddraw src/common src/vulkan
  ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_link_libraries(vulkan_dispatch_tests PRIVATE jpt_vulkan jpt_common user32 gdi32)
target_compile_options(vulkan_dispatch_tests PRIVATE /W3)
target_compile_definitions(vulkan_dispatch_tests PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(vulkan_runtime_tests EXCLUDE_FROM_ALL
  tests/vulkan_runtime_tests.c
  src/host/ddraw/page_guard.c
  src/host/ddraw/vk_win_renderer.c
  src/host/ddraw/vk_win_account.c
  src/host/texture_gpu_policy.c
  src/host/ddraw/d3d_translate.c
  src/host/ddraw/target_sync.c
  src/host/ce_effects.c
  src/host/ao_policy.c
  src/host/text_layer.c)
target_include_directories(vulkan_runtime_tests PRIVATE src/host src/host/ddraw src/common src/vulkan
  ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_link_libraries(vulkan_runtime_tests PRIVATE jpt_vulkan jpt_common user32 gdi32)
target_compile_options(vulkan_runtime_tests PRIVATE /W3)
target_compile_definitions(vulkan_runtime_tests PRIVATE _CRT_SECURE_NO_WARNINGS)

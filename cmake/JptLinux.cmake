# The native Linux x86_64 build (tools/build-linux.sh). Included by the top-level CMakeLists.txt on Linux only; the Windows
# build never reads this file.
#
# The guest stays a 32-bit Win32 program laid out in one 4 GB window of a 64-bit process (src/host64); the lifted C is
# compiled for x86_64 with guest addresses based on that window. Targets:
#   jpt_host64        guest address space, pointer marshalling, thunks (src/host64)
#   jpt_common_linux  settings, display maths, user data, guest path resolution, registry store, synchronisation
#   trespasser_recomp the Linux executable: guest bring-up, import binding and the Linux HLE (src/linux)
#   <name>_tests      headless suites (ctest; SDL_AUDIODRIVER=dummy), built by the target jpt_linux_tests
#   jpt_lifted_linux  the lifted guest, compiled for x86_64 (JPT_LINUX_LIFTED)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "the Linux host is a 64-bit process: guest addresses are 32-bit offsets into a 4 GB window")
endif()

option(JPT_LINUX_LIFTED "compile and link the lifted guest (about a minute at -O0, several at -O2)" ON)
option(JPT_LINUX_SANITIZE "build the host and the suites with AddressSanitizer and UndefinedBehaviorSanitizer (the lifted guest stays plain)" OFF)
option(JPT_LINUX_SDL_VIDEO "build SDL3 with its video and joystick subsystems (the window and the gamepad)" ON)
option(JPT_LINUX_LAUNCHER "build the launcher (TRG Launcher over SDL3 and Dear ImGui) into the executable; needs JPT_LINUX_SDL_VIDEO" ON)
set(JPT_LINUX_GEN_OPT "-O1" CACHE STRING "optimisation of the lifted C")
set(PCRECOMP "${CMAKE_SOURCE_DIR}/external/pcrecomp" CACHE PATH "pcrecomp checkout (tools/fetch_pcrecomp.py)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(JPT_REPO_ROOT "${CMAKE_SOURCE_DIR}")
include(JptLifts)
include(JptAudio)
find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(JPT_LINUX_WARNINGS -Wall -Wextra -Wshadow -Wpedantic -Werror)
# No path of this machine in any artifact: debug info and __FILE__ name the source and build trees relatively.
set(JPT_LINUX_COMPAT -include ${CMAKE_SOURCE_DIR}/src/common/jpt_compat.h)
if(JPT_LINUX_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_BINARY_DIR}=build)
if(FFMPEG_ROOT)
  add_compile_options(-ffile-prefix-map=${FFMPEG_ROOT}=ffmpeg)
endif()

# SDL3 from the pinned checkout, static: the window, the keyboard and mouse, the gamepad and the audio come from it and
# SDL chooses the window system and the audio driver itself.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER ON CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
if(JPT_LINUX_SDL_VIDEO)
  set(SDL_VIDEO ON CACHE BOOL "" FORCE)
  set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
else()
  set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
  set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory("${CMAKE_SOURCE_DIR}/external/SDL" "${CMAKE_BINARY_DIR}/SDL" EXCLUDE_FROM_ALL)

add_library(jpt_host64 STATIC
  src/host64/guest_memory.c src/host64/guest_ptr.c src/host64/guest_thunks.c src/host64/host_os_posix.c)
target_include_directories(jpt_host64 PUBLIC src/host64 ${PCRECOMP}/runtime/win32hle)
target_compile_options(jpt_host64 PRIVATE ${JPT_LINUX_WARNINGS})
target_link_libraries(jpt_host64 PUBLIC Threads::Threads)

# The window icon, embedded so the executable needs no icon theme or AppImage integration (packaging/icons/app-icon-256.png).
set(JPT_APP_ICON_C ${CMAKE_BINARY_DIR}/generated/jpt_app_icon.c)
add_custom_command(OUTPUT ${JPT_APP_ICON_C}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/packaging/icons/app-icon-256.png -DOUTPUT=${JPT_APP_ICON_C}
          -DSYMBOL=jpt_app_icon_png -P ${CMAKE_SOURCE_DIR}/cmake/embed_binary.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/cmake/embed_binary.cmake ${CMAKE_SOURCE_DIR}/packaging/icons/app-icon-256.png
  COMMENT "embedding the window icon")
set_source_files_properties(${JPT_APP_ICON_C} PROPERTIES COMPILE_OPTIONS "-w")

add_library(jpt_common_linux STATIC
  src/common/jpt_hash.c src/common/jpt_game_version.c src/common/jpt_lift_meta.c src/common/jpt_string_table.c
  src/common/jpt_restored_audio.c src/common/jpt_cheats.c src/common/jpt_config.c src/common/jpt_display.c
  src/common/jpt_gamepad.c src/common/jpt_prompt_text.c src/common/jpt_l10n.c src/common/jpt_tr.c
  src/common/jpt_video_subtitles.c src/common/jpt_user_data.c src/common/jpt_port.c src/common/jpt_gpu.c src/common/jpt_monitors_sdl.c src/common/jpt_app_identity.c ${JPT_APP_ICON_C} src/common/jpt_fs_posix.c src/common/jpt_vpath.c
  src/common/jpt_custom_level.c src/common/jpt_sky.c src/common/jpt_picture.c src/common/jpt_lit.c src/common/jpt_texhw.c src/common/jpt_szdd.c src/common/jpt_scene.c src/common/jpt_level_features.c src/common/jpt_alloc.c src/common/jpt_groff.c src/common/jpt_level_world.c src/common/jpt_save.c src/common/jpt_level_gate.c src/common/jpt_level_session.c src/common/jpt_launch_handoff.c
  src/common/jpt_reg_store.c src/common/jpt_sync.c src/common/jpt_renderer.c src/common/jpt_present_policy.c)
target_include_directories(jpt_common_linux PUBLIC src/common)
target_compile_options(jpt_common_linux PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
target_link_libraries(jpt_common_linux PUBLIC SDL3::SDL3-static m)

add_library(jpt_vulkan_linux STATIC
  src/vulkan/vk_policy.c src/vulkan/vk_loader.c src/vulkan/vk_alloc.c
  src/vulkan/vk_context.c src/vulkan/vk_swapchain.c src/vulkan/vk_shaders.c
  src/vulkan/vk_renderer.c src/host/ao_policy.c)
target_include_directories(jpt_vulkan_linux PUBLIC src/vulkan)
target_include_directories(jpt_vulkan_linux SYSTEM PUBLIC third_party/vulkan)
target_include_directories(jpt_vulkan_linux PRIVATE src/host)
target_compile_options(jpt_vulkan_linux PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
target_link_libraries(jpt_vulkan_linux PUBLIC jpt_common_linux SDL3::SDL3-static m)

set(JPT_RECOMP_DEFINES RECOMP_LOCAL_REGS RECOMP_X87_RING RECOMP_X87_RING_DEPTH RECOMP_ICALL_SITE_RANGE)
set(AUDIO_DIR ${CMAKE_SOURCE_DIR}/src/host/audio)
set(JPT_AUDIO_CORE ${AUDIO_DIR}/audio_policy.c ${AUDIO_DIR}/audio_pacer.c ${AUDIO_DIR}/audio_mixer.c ${AUDIO_DIR}/audio_spatial.c
  ${AUDIO_DIR}/audio_sync.c ${AUDIO_DIR}/dsound_compat.c)
set(JPT_LINUX_FONT_C ${CMAKE_BINARY_DIR}/generated/lx_font_data.c)
add_custom_command(OUTPUT ${JPT_LINUX_FONT_C}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/external/imgui/misc/fonts/DroidSans.ttf -DOUTPUT=${JPT_LINUX_FONT_C}
          -DSYMBOL=lx_font_data -P ${CMAKE_SOURCE_DIR}/cmake/embed_binary.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/cmake/embed_binary.cmake ${CMAKE_SOURCE_DIR}/external/imgui/misc/fonts/DroidSans.ttf
  COMMENT "embedding the text face")
# The translated text and the restored audio, embedded as in the Windows build (assets/translations, assets/restored_audio).
function(jpt_embed_directory glob_expr list_file source_file symbol)
  file(GLOB inputs CONFIGURE_DEPENDS ${glob_expr})
  list(SORT inputs)
  file(CONFIGURE OUTPUT ${list_file} CONTENT "${inputs}")
  add_custom_command(OUTPUT ${source_file}
    COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${list_file} -DOUTPUT=${source_file} -DSYMBOL=${symbol}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_files_c.cmake
    DEPENDS ${list_file} ${inputs} ${CMAKE_SOURCE_DIR}/cmake/embed_files_c.cmake
    COMMENT "embedding ${symbol}")
endfunction()
set(JPT_LINUX_TRANSLATIONS_C ${CMAKE_BINARY_DIR}/generated/lx_translations.c)
set(JPT_LINUX_RESTORED_C ${CMAKE_BINARY_DIR}/generated/lx_restored_audio.c)
jpt_embed_directory("${CMAKE_SOURCE_DIR}/assets/translations/*.txt" ${CMAKE_BINARY_DIR}/generated/lx_translations.txt
                    ${JPT_LINUX_TRANSLATIONS_C} lx_translation_files)
jpt_embed_directory("${CMAKE_SOURCE_DIR}/assets/restored_audio/*" ${CMAKE_BINARY_DIR}/generated/lx_restored_audio.txt
                    ${JPT_LINUX_RESTORED_C} lx_restored_audio_files)
set(JPT_STREAM_SOURCES src/host/stream_pack.c src/host/stream_vfile.c src/host/tpa_merge.c src/linux/linux_assets.c
                       ${JPT_LINUX_TRANSLATIONS_C} ${JPT_LINUX_RESTORED_C})
# The host features written against guest_abi.h (src/host/guest_abi.h): the same sources the Windows host builds, over the Linux
# implementation of that interface (src/linux/linux_host.c, linux_overrides.c).
set(JPT_GUEST_FEATURE_SOURCES
  src/host/guest_features.c src/host/scene_options.c src/host/terrain_shadows.c src/host/ce_gameplay.c src/host/active_ai.c src/host/active_ai_policy.c src/host/ai_vocals.c src/host/scene_scripts.c src/host/sky_layers.c src/host/texhw_guest.c src/host/level_actions.c src/host/level_actions_policy.c src/host/level_session.c
  src/host/patch117.c src/host/patch117_objects.c src/host/patch117_physics.c src/host/patch117_player.c
  src/host/patch117_rules.c src/host/patch117_policy.c src/host/physics_fixes.c src/host/physics_watch.c
  src/host/physics_watch_policy.c src/host/terrain_quality.c src/host/engine_heaps.c src/host/level_abort.c src/host/missing_textures.c src/host/widescreen.c src/host/fov_policy.c
  src/host/perf_sections.c src/host/override_registry.c src/host/cheats.c src/host/cheats_policy.c src/host/skip_intro.c
  src/host/debug_menu.c src/host/debug_menu_policy.c src/host/objectives.c src/host/custom_level_guide.c
  src/host/objective_policy.c src/host/overlay.c src/host/overlay_policy.c
  src/host/overlay_text.c src/host/guest_ui_actions.c src/host/water_kernels.c
  src/linux/linux_host.c src/linux/linux_overrides.c src/linux/linux_engine.c src/linux/linux_features.c src/linux/linux_panel.c src/host/l10n.c)
# Dear ImGui and the host's panel code (src/host/debug_menu_panel.cpp, the one the Windows host draws with) over a CPU backend
# (src/host/imgui_software.cpp). C++17 without exceptions or RTTI; the C++ runtime is linked statically, so the executable needs no
# libstdc++ of the machine.
set(JPT_IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
add_library(jpt_imgui_linux STATIC ${JPT_IMGUI_DIR}/imgui.cpp ${JPT_IMGUI_DIR}/imgui_draw.cpp ${JPT_IMGUI_DIR}/imgui_tables.cpp
  ${JPT_IMGUI_DIR}/imgui_widgets.cpp)
target_include_directories(jpt_imgui_linux SYSTEM PUBLIC ${JPT_IMGUI_DIR})
set_target_properties(jpt_imgui_linux PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(jpt_imgui_linux PRIVATE -w -fno-exceptions -fno-rtti)
add_library(jpt_panel_model_linux STATIC src/host/minimap.c src/host/compass_policy.c src/host/debug_menu_layout.c)
target_include_directories(jpt_panel_model_linux PUBLIC src/host)
target_compile_options(jpt_panel_model_linux PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
target_link_libraries(jpt_panel_model_linux PUBLIC jpt_common_linux)
add_library(jpt_panel_linux STATIC src/host/debug_menu_panel.cpp src/host/imgui_software.cpp)
target_include_directories(jpt_panel_linux PUBLIC src/host)
target_include_directories(jpt_panel_linux PRIVATE src/common)
set_target_properties(jpt_panel_linux PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(jpt_panel_linux PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti -include ${CMAKE_SOURCE_DIR}/src/common/jpt_compat.h)
target_link_libraries(jpt_panel_linux PUBLIC jpt_imgui_linux jpt_panel_model_linux)

add_library(jpt_linux_core STATIC ${JPT_STREAM_SOURCES} src/linux/linux_hle.c src/linux/linux_paths.c src/linux/linux_log.c src/linux/linux_crash.c src/linux/linux_shutdown.c src/linux/linux_guest_quit.c src/linux/linux_spine.c src/linux/linux_msvcrt.c src/linux/linux_modules.c src/linux/linux_msvcrt_io.c src/linux/linux_user32.c src/linux/linux_advapi32.c src/linux/linux_signatures.c src/linux/linux_processor.c src/linux/linux_window.c src/linux/linux_screen.c src/linux/linux_gdi32.c src/linux/linux_text.c src/linux/linux_stb_truetype.c ${JPT_LINUX_FONT_C} src/linux/linux_dsound.c src/linux/linux_smack.c src/linux/linux_video_subtitles.c src/linux/linux_mouse.c src/linux/linux_guest_ui.c src/linux/linux_overlay.c src/linux/linux_quick.c src/linux/linux_hostkeys.c src/linux/linux_settings.c src/host/guest_ui_model.c src/host/host_policy.c src/host/ce_policy.c src/host/window_title.c ${JPT_GUEST_FEATURE_SOURCES} ${JPT_AUDIO_CORE} ${AUDIO_DIR}/sdl_audio.c ${JPT_SMACKER_PORTABLE_SOURCES} src/linux/linux_ddraw.c src/linux/linux_d3d.c src/linux/linux_d3d_target.c src/linux/linux_d3d_guest.c src/linux/linux_vulkan.c src/host/ce_effects.c src/host/ddraw/d3d_translate.c src/host/ddraw/target_sync.c src/linux/linux_lz32.c src/linux/linux_resource.c)
if(JPT_LINUX_SDL_VIDEO)
  target_sources(jpt_linux_core PRIVATE src/linux/linux_sdl_window.c src/linux/linux_gamepad.c src/host/pad_sdl.c src/host/gamepad_policy.c src/host/gamepad_ui.c src/host/gamepad_menu.c src/host/frontend_layout.c src/host/frontend_assets.c src/host/frontend_art.c src/host/frontend_plaque.c src/host/frontend_text_stb.c src/host/loading_layout.c src/host/pad_glyphs.c src/host/save_preview.c src/host/frontend_hd.c src/host/loading_hd.c src/linux/linux_frontend.c src/linux/linux_card.c)
  target_compile_definitions(jpt_linux_core PUBLIC JPT_LINUX_SDL_VIDEO=1)
endif()
target_include_directories(jpt_linux_core PUBLIC src/linux src/host src/common)
target_include_directories(jpt_linux_core PRIVATE external/imgui)
target_include_directories(jpt_linux_core PUBLIC ${AUDIO_DIR} src/host/video)
jpt_smacker_use_ffmpeg(jpt_linux_core)
set_source_files_properties(src/linux/linux_crash.c PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)
set_source_files_properties(src/host/save_preview.c PROPERTIES COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}/external/SDL/src/video")
set_source_files_properties(src/linux/linux_stb_truetype.c ${JPT_LINUX_FONT_C} ${JPT_LINUX_TRANSLATIONS_C} ${JPT_LINUX_RESTORED_C} PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(jpt_linux_core SYSTEM PUBLIC ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(jpt_linux_core PUBLIC ${JPT_RECOMP_DEFINES})
target_compile_options(jpt_linux_core PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
target_link_libraries(jpt_linux_core PUBLIC jpt_panel_linux jpt_vulkan_linux jpt_host64 jpt_common_linux jpt_profile_linux)
target_link_options(jpt_linux_core PUBLIC -static-libstdc++ -static-libgcc)

add_library(jpt_profile_linux STATIC src/host/active_profile.c src/host/profiles/guest_profile_data.c)
target_include_directories(jpt_profile_linux PUBLIC src/host)
target_link_libraries(jpt_profile_linux PUBLIC jpt_common_linux)
target_compile_options(jpt_profile_linux PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})

# ---- headless suites ----------------------------------------------------------------------------------------------
enable_testing()
add_custom_target(jpt_linux_tests)

function(jpt_linux_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LIBS;INCLUDES;DEFINES" ${ARGN})
  add_executable(${name} EXCLUDE_FROM_ALL ${T_SOURCES})
  target_include_directories(${name} PRIVATE src/common ${T_INCLUDES})
  target_compile_options(${name} PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
  target_compile_definitions(${name} PRIVATE "JPT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"" ${T_DEFINES})
  target_link_libraries(${name} PRIVATE ${T_LIBS} m)
  add_dependencies(jpt_linux_tests ${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy;SDL_VIDEODRIVER=dummy"
                       WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()


jpt_linux_test(x64_tests SOURCES tests/x64_tests.c LIBS jpt_host64 INCLUDES tests)
jpt_linux_test(vpath_tests SOURCES tests/vpath_tests.c LIBS jpt_common_linux)
jpt_linux_test(level_session_tests SOURCES tests/level_session_tests.c LIBS jpt_common_linux)
jpt_linux_test(launch_handoff_tests SOURCES tests/launch_handoff_tests.c LIBS jpt_common_linux)
include(${CMAKE_SOURCE_DIR}/cmake/JptSquirrel.cmake)
jpt_linux_test(script_vm_tests SOURCES tests/script_vm_tests.cpp LIBS jpt_script)
set_target_properties(script_vm_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
jpt_linux_test(sync_tests SOURCES tests/sync_tests.c LIBS jpt_common_linux)
jpt_linux_test(port_tests SOURCES tests/port_tests.c LIBS jpt_common_linux)
jpt_linux_test(app_identity_tests SOURCES tests/app_identity_tests.c LIBS jpt_common_linux)
jpt_linux_test(reg_store_tests SOURCES tests/reg_store_tests.c LIBS jpt_common_linux)
jpt_linux_test(vulkan_policy_tests SOURCES tests/vulkan_policy_tests.c LIBS jpt_vulkan_linux)
jpt_linux_test(vulkan_render_tests SOURCES tests/vulkan_render_tests.c LIBS jpt_vulkan_linux)
jpt_linux_test(audio_portable_tests SOURCES tests/audio_portable_tests.c ${AUDIO_DIR}/audio_policy.c ${AUDIO_DIR}/audio_mixer.c
  LIBS SDL3::SDL3-static INCLUDES ${AUDIO_DIR})
jpt_linux_test(audio_pacer_tests SOURCES tests/audio_pacer_tests.c ${AUDIO_DIR}/audio_pacer.c LIBS SDL3::SDL3-static INCLUDES ${AUDIO_DIR})
jpt_linux_test(audio_backend_tests SOURCES tests/audio_backend_tests.c ${AUDIO_DIR}/sdl_audio.c ${AUDIO_DIR}/audio_mixer.c
  ${AUDIO_DIR}/audio_policy.c ${AUDIO_DIR}/audio_pacer.c LIBS SDL3::SDL3-static INCLUDES ${AUDIO_DIR})
jpt_linux_test(dsound_compat_tests SOURCES tests/dsound_compat_tests.c ${JPT_AUDIO_CORE} LIBS SDL3::SDL3-static INCLUDES ${AUDIO_DIR})
jpt_linux_test(hammond_voiceover_tests SOURCES tests/hammond_voiceover_tests.c ${JPT_AUDIO_CORE} LIBS SDL3::SDL3-static
  INCLUDES ${AUDIO_DIR})
jpt_linux_test(smacker_tests SOURCES tests/smacker_tests.c ${JPT_SMACKER_PORTABLE_SOURCES} ${JPT_AUDIO_CORE}
  LIBS SDL3::SDL3-static INCLUDES ${AUDIO_DIR} src/host/video)
jpt_linux_test(smacker_ffmpeg_tests SOURCES tests/smacker_ffmpeg_tests.c ${JPT_SMACKER_PORTABLE_SOURCES} ${JPT_AUDIO_CORE}
  LIBS SDL3::SDL3-static INCLUDES ${AUDIO_DIR} src/host/video)
jpt_smacker_use_ffmpeg(smacker_ffmpeg_tests)
jpt_linux_test(menu_audio_tests SOURCES tests/menu_audio_tests.c src/host/menu_audio_policy.c src/common/jpt_hash.c
  INCLUDES src/host src/common)
jpt_linux_test(gamepad_tests SOURCES tests/gamepad_tests.c src/host/gamepad_policy.c src/host/pad_glyphs.c src/host/overlay_policy.c
  LIBS jpt_profile_linux INCLUDES src/host tests)
jpt_linux_test(ce_tests SOURCES tests/ce_tests.c src/host/ce_policy.c src/host/ce_effects.c src/host/host_policy.c
  LIBS jpt_profile_linux INCLUDES src/host tests)
jpt_linux_test(input_context_tests SOURCES tests/input_context_tests.c src/host/guest_ui_model.c src/host/host_policy.c LIBS jpt_profile_linux INCLUDES src/host tests)
jpt_linux_test(hotkey_tests SOURCES tests/hotkey_tests.c src/host/host_policy.c LIBS jpt_profile_linux INCLUDES src/host tests)
jpt_linux_test(active_ai_tests SOURCES tests/active_ai_tests.c src/host/active_ai_policy.c LIBS jpt_profile_linux INCLUDES src/host tests)

jpt_linux_test(profile_dispatch_tests SOURCES tests/profile_dispatch_tests.c LIBS jpt_profile_linux INCLUDES tests)
jpt_linux_test(linux_paths_tests SOURCES tests/linux_paths_tests.c LIBS jpt_linux_core)
jpt_linux_test(linux_hle_tests SOURCES tests/linux_hle_tests.c LIBS jpt_linux_core)
jpt_linux_test(linux_video_subtitle_tests SOURCES tests/linux_video_subtitle_tests.c LIBS jpt_linux_core)
jpt_linux_test(linux_overrides_tests SOURCES tests/linux_overrides_tests.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_panel_tests SOURCES tests/linux_panel_tests.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_frontend_tests SOURCES tests/linux_frontend_tests.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_hotkey_tests SOURCES tests/linux_hotkey_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_mouse_tests SOURCES tests/linux_mouse_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_cs_tests SOURCES tests/linux_cs_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_stream_tests SOURCES tests/linux_stream_tests.c LIBS jpt_linux_core)
jpt_linux_test(linux_save_files_tests SOURCES tests/linux_save_files_tests.c LIBS jpt_linux_core)
jpt_linux_test(texhw_tests SOURCES tests/texhw_tests.c LIBS jpt_common_linux)
jpt_linux_test(linux_level_abort_tests SOURCES tests/linux_level_abort_tests.c LIBS jpt_linux_core)
jpt_linux_test(linux_guest_ptr_tests SOURCES tests/linux_guest_ptr_tests.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_call_spine_tests SOURCES tests/linux_call_spine_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core)
jpt_linux_test(linux_user32_tests SOURCES tests/linux_user32_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_gdi_tests SOURCES tests/linux_gdi_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_ddraw_tests SOURCES tests/linux_ddraw_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(sky_tests SOURCES tests/sky_tests.c LIBS jpt_common_linux INCLUDES src/host)
jpt_linux_test(linux_d3d_tests SOURCES tests/linux_d3d_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_present_tests SOURCES tests/linux_present_tests.c LIBS jpt_linux_core)
jpt_linux_test(linux_crash_tests SOURCES tests/linux_crash_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
if(JPT_LINUX_SDL_VIDEO)
  jpt_linux_test(linux_sdl_window_tests SOURCES tests/linux_sdl_window_tests.c LIBS jpt_linux_core)
  jpt_linux_test(linux_gamepad_tests SOURCES tests/linux_gamepad_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
endif()
jpt_linux_test(linux_audio_stress_tests SOURCES tests/linux_audio_stress_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests ${AUDIO_DIR})
set_source_files_properties(tests/linux_fake_guest.c PROPERTIES COMPILE_OPTIONS "-w")

# ---- the lifted guest, compiled for x86_64 -----------------------------------------------------------------------
if(JPT_LINUX_LIFTED)
  if(NOT EXISTS "${PCRECOMP}/runtime/recomp32/recomp_types.h")
    message(FATAL_ERROR "pcrecomp not found at ${PCRECOMP}: run `python3 tools/fetch_pcrecomp.py`")
  endif()
  jpt_unpack_lifts("${CMAKE_SOURCE_DIR}")
  add_library(jpt_lifted_objects OBJECT ${GEN_SOURCES})
  target_include_directories(jpt_lifted_objects PRIVATE ${PCRECOMP}/runtime/recomp32 src/common)
  target_compile_definitions(jpt_lifted_objects PRIVATE ${JPT_RECOMP_DEFINES})
  target_compile_options(jpt_lifted_objects PRIVATE -w ${JPT_LINUX_GEN_OPT})
  if(JPT_LINUX_SANITIZE)
    target_compile_options(jpt_lifted_objects PRIVATE -fno-sanitize=all)
  endif()
  add_library(jpt_lifted_linux STATIC $<TARGET_OBJECTS:jpt_lifted_objects>)

  # The lifted guest with its Linux bindings and the bootstrap, one archive so the guest's references to the runtime
  # (jpt_linux_core) and the bindings' references to the lifted bodies resolve in either order.
  add_library(jpt_linux_guest STATIC $<TARGET_OBJECTS:jpt_lifted_objects> src/linux/linux_lift.c src/linux/linux_boot.c src/linux/linux_override_table.c)
  target_compile_options(jpt_linux_guest PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
  target_link_libraries(jpt_linux_guest PUBLIC jpt_linux_core)
  jpt_linux_test(linux_boot_tests SOURCES tests/linux_boot_tests.c LIBS jpt_linux_guest)
  jpt_linux_test(linux_shutdown_tests SOURCES tests/linux_shutdown_tests.c tests/linux_fake_guest.c LIBS jpt_linux_guest INCLUDES tests)
  jpt_linux_test(linux_module_tests SOURCES tests/linux_module_tests.c LIBS jpt_linux_guest)
  jpt_linux_test(linux_session_tests SOURCES tests/linux_session_tests.c LIBS jpt_linux_guest)
  jpt_linux_test(linux_features_tests SOURCES tests/linux_features_tests.c LIBS jpt_linux_guest INCLUDES src/host)
  jpt_linux_test(linux_video_tests SOURCES tests/linux_video_tests.c LIBS jpt_linux_guest INCLUDES src/host/video src/host/audio)
endif()

# ---- the launcher ---------------------------------------------------------------------------------------------------
# The same sources as the Windows build (launcher/, config/launcher_deps.txt): TRG Launcher in an SDL3 window with an SDL
# renderer and Dear ImGui. Its file picker is zenity or kdialog and its folder buttons xdg-open (external/trg-launcher).
if(JPT_LINUX_LAUNCHER AND JPT_LINUX_SDL_VIDEO)
  set(JPT_TRG_DIR "${CMAKE_SOURCE_DIR}/external/trg-launcher")
  if(NOT EXISTS "${JPT_TRG_DIR}/include/trg/launcher.h" OR NOT EXISTS "${JPT_IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "launcher sources missing: run tools/build.ps1 once on Windows (config/launcher_deps.txt) or configure with -DJPT_LINUX_LAUNCHER=OFF")
  endif()
  add_library(trg_core_linux STATIC ${JPT_TRG_DIR}/src/settings.cpp ${JPT_TRG_DIR}/src/platform.cpp ${JPT_TRG_DIR}/src/download.cpp)
  target_include_directories(trg_core_linux PUBLIC ${JPT_TRG_DIR}/include)
  add_library(imgui_sdl_linux STATIC ${JPT_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${JPT_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
  target_link_libraries(imgui_sdl_linux PUBLIC jpt_imgui_linux SDL3::SDL3-static)
  add_library(trg_ui_linux STATIC ${JPT_TRG_DIR}/src/launcher.cpp ${JPT_TRG_DIR}/src/headers.cpp)
  target_link_libraries(trg_ui_linux PUBLIC trg_core_linux imgui_sdl_linux)
  set_target_properties(trg_core_linux imgui_sdl_linux trg_ui_linux PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_compile_options(trg_core_linux PRIVATE -w)
  target_compile_options(imgui_sdl_linux PRIVATE -w -fno-exceptions -fno-rtti)
  target_compile_options(trg_ui_linux PRIVATE -w)

  file(GLOB JPT_PAGE_ICONS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/launcher/icons/*.svg)
  list(SORT JPT_PAGE_ICONS)
  set(JPT_PAGE_ICON_LIST ${CMAKE_BINARY_DIR}/generated/page_icons.txt)
  file(CONFIGURE OUTPUT ${JPT_PAGE_ICON_LIST} CONTENT "${JPT_PAGE_ICONS}")
  set(JPT_PAGE_ICON_SOURCE ${CMAKE_BINARY_DIR}/generated/page_icon_data.cpp)
  add_custom_command(OUTPUT ${JPT_PAGE_ICON_SOURCE}
    COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${JPT_PAGE_ICON_LIST} -DOUTPUT=${JPT_PAGE_ICON_SOURCE} -DSYMBOL=kPageIconFiles
            -P ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
    DEPENDS ${JPT_PAGE_ICON_LIST} ${JPT_PAGE_ICONS} ${CMAKE_SOURCE_DIR}/launcher/embed_files.cmake
    COMMENT "embedding the page icons")

  add_library(jpt_launcher_core_linux STATIC launcher/iso9660.cpp launcher/inflate.cpp launcher/is_cabinet.cpp launcher/game_files.cpp
    launcher/game_install.cpp launcher/settings.cpp launcher/system_info.cpp launcher/file_browser.cpp launcher/file_picker.cpp launcher/options.cpp launcher/header_art.cpp
    launcher/page_icons.cpp launcher/launcher_log.cpp launcher/user_data.cpp launcher/os_file.cpp launcher/pe_resources.cpp
    launcher/localization/ui_text.cpp launcher/http_post.cpp launcher/incident_client.cpp launcher/incident_report.cpp
    launcher/json_lite.cpp launcher/log_scrubber.cpp launcher/report_history.cpp launcher/process_probe.cpp
    launcher/localization/pt_br_ui_text.cpp ${JPT_PAGE_ICON_SOURCE})
  target_include_directories(jpt_launcher_core_linux PUBLIC launcher)
  target_link_libraries(jpt_launcher_core_linux PUBLIC jpt_common_linux jpt_vulkan_linux trg_core_linux)
  # The Bug Report's HTTPS: OpenSSL linked statically (WinHTTP on Windows), so the executable stays self-contained. Without
  # its development files (libssl-dev) a development build still compiles and the page reports the service as unreachable;
  # the executable then declares transport "none", the capability bug_report_https fails and the Linux gate refuses the
  # artifact. A Release build, or JPT_LINUX_REQUIRE_BUG_REPORT=ON, refuses to configure without it.
  option(JPT_LINUX_REQUIRE_BUG_REPORT "fail the configure when OpenSSL is missing (always for Release)" OFF)
  set(OPENSSL_USE_STATIC_LIBS ON)
  find_package(OpenSSL COMPONENTS SSL Crypto)
  if(OpenSSL_FOUND)
    set(JPT_BUG_REPORT_TRANSPORT openssl)
    target_compile_definitions(jpt_launcher_core_linux PRIVATE JPT_HAVE_OPENSSL=1)
    target_compile_options(jpt_launcher_core_linux PRIVATE -ffile-prefix-map=${OPENSSL_INCLUDE_DIR}=openssl)
    target_link_libraries(jpt_launcher_core_linux PUBLIC OpenSSL::SSL OpenSSL::Crypto ${CMAKE_DL_LIBS} Threads::Threads)
  else()
    set(JPT_BUG_REPORT_TRANSPORT none)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR JPT_LINUX_REQUIRE_BUG_REPORT)
      message(FATAL_ERROR "the Bug Report needs OpenSSL (install libssl-dev, which ships the static libraries): a Release build never ships without it")
    endif()
    message(WARNING "OpenSSL was not found: the Bug Report cannot send in this build (development only; the Linux gate refuses it)")
  endif()
  target_compile_definitions(jpt_launcher_core_linux PRIVATE JPT_PORT_VERSION="${PROJECT_VERSION}")
  # The 7z decoder of the LZMA SDK (custom level packages).
  set(JPT_LZMA_SOURCES 7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zStream.c Bcj2.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c Lzma2Dec.c LzmaDec.c)
  list(TRANSFORM JPT_LZMA_SOURCES PREPEND launcher/third_party/lzma/)
  add_library(jpt_lzma_linux STATIC ${JPT_LZMA_SOURCES})
  target_compile_options(jpt_lzma_linux PRIVATE -w)
  set(JPT_ARCHIVE_SOURCES launcher/archive_reader.cpp launcher/archive_spool.cpp launcher/rar_blake2.cpp launcher/rar_ppm.cpp launcher/rar_reader.cpp launcher/rar_stream.cpp launcher/rar_unpack.cpp launcher/seven_zip_block.cpp)
  set(JPT_CUSTOM_LEVEL_SOURCES launcher/custom_levels.cpp launcher/level_analysis.cpp launcher/level_check.cpp launcher/level_corpus.cpp launcher/level_import_log.cpp launcher/level_ini.cpp launcher/level_limits.cpp launcher/level_listing.cpp launcher/level_manifest.cpp
    launcher/level_picture.cpp launcher/level_readme.cpp launcher/level_settings.cpp launcher/level_guide.cpp)
  add_library(jpt_launcher_linux STATIC launcher/launcher.cpp launcher/disc_browser.cpp ${JPT_ARCHIVE_SOURCES} ${JPT_CUSTOM_LEVEL_SOURCES} launcher/custom_levels_page.cpp launcher/level_analyze_cli.cpp launcher/host.cpp launcher/incident_page.cpp launcher/my_reports_page.cpp
    launcher/update_config.cpp launcher/updater/github_release.cpp launcher/updater/json.cpp launcher/updater/sha256.cpp
    launcher/updater/third_party/miniz/miniz.c launcher/updater/update_controller.cpp launcher/updater/update_job.cpp
    launcher/updater/update_paths.cpp launcher/updater/update_platform.cpp launcher/updater/update_ui.cpp
    launcher/updater/update_zip.cpp launcher/updater/version.cpp)
  set_source_files_properties(launcher/updater/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
  target_include_directories(jpt_launcher_linux PRIVATE launcher ${CMAKE_BINARY_DIR}/generated)
  target_link_libraries(jpt_launcher_linux PUBLIC jpt_launcher_core_linux trg_ui_linux jpt_lzma_linux)
  target_compile_definitions(jpt_launcher_linux PRIVATE JPT_PORT_VERSION="${PROJECT_VERSION}")
  set_target_properties(jpt_launcher_core_linux jpt_launcher_linux PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  foreach(lib jpt_launcher_core_linux jpt_launcher_linux)
    target_compile_options(${lib} PRIVATE -Wall -Wextra)
    target_compile_options(${lib} PRIVATE -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
  endforeach()
  set_source_files_properties(launcher/page_icons.cpp ${JPT_PAGE_ICON_SOURCE} PROPERTIES COMPILE_OPTIONS "-w")
  target_link_options(jpt_launcher_linux PUBLIC -static-libstdc++ -static-libgcc)

  jpt_linux_test(linux_launcher_tests SOURCES tests/linux_launcher_tests.cpp LIBS jpt_launcher_core_linux INCLUDES launcher)
  set_target_properties(linux_launcher_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

  jpt_linux_test(launcher_startup_tests SOURCES tests/launcher_startup_tests.cpp LIBS jpt_common_linux INCLUDES launcher)
  set_target_properties(launcher_startup_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

  jpt_linux_test(custom_level_tests SOURCES tests/custom_level_tests.cpp tests/custom_level_analysis_tests.cpp tests/level_check_tests.cpp tests/custom_level_listing_tests.cpp ${JPT_ARCHIVE_SOURCES}
                 ${JPT_CUSTOM_LEVEL_SOURCES} src/host/tpa_merge.c launcher/updater/json.cpp launcher/updater/update_paths.cpp launcher/updater/update_zip.cpp launcher/updater/third_party/miniz/miniz.c
                 LIBS jpt_launcher_core_linux jpt_lzma_linux INCLUDES launcher src/host)
  set_target_properties(custom_level_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  jpt_linux_test(custom_level_session_tests SOURCES tests/custom_level_session_tests.cpp src/host/custom_level_guide.c src/host/minimap.c src/host/objective_policy.c src/host/compass_policy.c ${JPT_ARCHIVE_SOURCES} ${JPT_CUSTOM_LEVEL_SOURCES}
                 launcher/updater/json.cpp launcher/updater/update_paths.cpp launcher/updater/update_zip.cpp launcher/updater/third_party/miniz/miniz.c
                 LIBS jpt_launcher_core_linux jpt_lzma_linux INCLUDES launcher src/host)
  set_target_properties(custom_level_session_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

  # The Bug Report page and the auto update: the same sources and the same tests as Windows (tests/bug_report_tests.cpp,
  # tests/update_tests.cpp, tests/update_network_tests.cpp); the update helper process is Windows only.
  add_library(jpt_updater_core_linux STATIC launcher/update_config.cpp launcher/updater/github_release.cpp
    launcher/updater/install_transaction.cpp launcher/updater/json.cpp launcher/updater/sha256.cpp launcher/updater/update_controller.cpp
    launcher/updater/update_job.cpp launcher/updater/update_paths.cpp launcher/updater/update_platform.cpp launcher/updater/update_zip.cpp
    launcher/updater/version.cpp launcher/updater/third_party/miniz/miniz.c)
  target_include_directories(jpt_updater_core_linux PUBLIC launcher ${CMAKE_BINARY_DIR}/generated)
  target_compile_definitions(jpt_updater_core_linux PRIVATE JPT_PORT_VERSION="${PROJECT_VERSION}")
  target_link_libraries(jpt_updater_core_linux PUBLIC trg_core_linux Threads::Threads)
  target_compile_options(jpt_updater_core_linux PRIVATE -Wall -Wextra)
  set_target_properties(jpt_updater_core_linux PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  set_source_files_properties(launcher/updater/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
  jpt_linux_test(bug_report_tests SOURCES tests/bug_report_tests.cpp LIBS jpt_launcher_core_linux INCLUDES launcher
                 DEFINES JPT_PORT_VERSION="${PROJECT_VERSION}")
  jpt_linux_test(update_tests SOURCES tests/update_tests.cpp LIBS jpt_updater_core_linux INCLUDES launcher
                 DEFINES JPT_UPDATER_SRC_DIR="${CMAKE_SOURCE_DIR}/launcher" JPT_PORT_VERSION="${PROJECT_VERSION}")
  jpt_linux_test(update_network_tests SOURCES tests/update_network_tests.cpp LIBS jpt_updater_core_linux INCLUDES launcher)
  set_target_properties(bug_report_tests update_tests update_network_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

  # The images the launcher reads next to the executable (launcher/launcher.cpp: assets/launcher).
  set(JPT_LAUNCHER_ASSET_COPIES)
  foreach(asset header-art.png sponsor-heart.png)
    set(asset_source "${CMAKE_SOURCE_DIR}/assets/launcher/${asset}")
    set(asset_stamp "${CMAKE_BINARY_DIR}/launcher-assets/${asset}.stamp")
    add_custom_command(OUTPUT "${asset_stamp}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/assets/launcher"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${asset_source}" "${CMAKE_BINARY_DIR}/bin/assets/launcher/${asset}"
      COMMAND ${CMAKE_COMMAND} -E touch "${asset_stamp}"
      DEPENDS "${asset_source}"
      VERBATIM)
    list(APPEND JPT_LAUNCHER_ASSET_COPIES "${asset_stamp}")
  endforeach()
  add_custom_target(jpt_launcher_assets DEPENDS ${JPT_LAUNCHER_ASSET_COPIES})
endif()

# ---- the executable ----------------------------------------------------------------------------------------------
find_program(JPT_PYTHON NAMES python3 python REQUIRED)
set(JPT_BUILD_INFO_DIR ${CMAKE_BINARY_DIR}/buildinfo)
add_custom_target(jpt_build_info
  COMMAND ${JPT_PYTHON} -I ${CMAKE_SOURCE_DIR}/tools/gen_build_info.py --out ${JPT_BUILD_INFO_DIR}/jpt_build_info.h
          --version ${PROJECT_VERSION} --compiler "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
          --target "linux-${CMAKE_SYSTEM_PROCESSOR}" --config "${CMAKE_BUILD_TYPE}"
          --bug-report-transport "${JPT_BUG_REPORT_TRANSPORT}"
  BYPRODUCTS ${JPT_BUILD_INFO_DIR}/jpt_build_info.h
  VERBATIM)
add_executable(trespasser_recomp src/linux/linux_main.c src/linux/linux_selfcheck.c)
add_dependencies(trespasser_recomp jpt_build_info)
jpt_smacker_use_ffmpeg(trespasser_recomp)
target_include_directories(trespasser_recomp PRIVATE ${JPT_BUILD_INFO_DIR})
target_compile_options(trespasser_recomp PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
if(JPT_LINUX_LIFTED)
  target_compile_definitions(trespasser_recomp PRIVATE JPT_LINUX_GUEST=1)
  target_link_libraries(trespasser_recomp PRIVATE jpt_linux_guest)
else()
  target_link_libraries(trespasser_recomp PRIVATE jpt_linux_core)
endif()
if(JPT_LINUX_LAUNCHER AND JPT_LINUX_SDL_VIDEO)
  target_compile_definitions(trespasser_recomp PRIVATE JPT_LINUX_LAUNCHER=1)
  target_link_libraries(trespasser_recomp PRIVATE jpt_launcher_linux)
  add_dependencies(trespasser_recomp jpt_launcher_assets)
endif()
set_target_properties(trespasser_recomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
                      BUILD_RPATH "" INSTALL_RPATH "" SKIP_BUILD_RPATH ON)

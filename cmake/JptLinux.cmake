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
set(JPT_LINUX_GEN_OPT "-O1" CACHE STRING "optimisation of the lifted C")
set(PCRECOMP "${CMAKE_SOURCE_DIR}/external/pcrecomp" CACHE PATH "pcrecomp checkout (tools/fetch_pcrecomp.py)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(JPT_REPO_ROOT "${CMAKE_SOURCE_DIR}")
include(JptLifts)
include(JptAudio)

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
target_link_libraries(jpt_host64 PUBLIC pthread)

add_library(jpt_common_linux STATIC
  src/common/jpt_hash.c src/common/jpt_game_version.c src/common/jpt_lift_meta.c src/common/jpt_string_table.c
  src/common/jpt_restored_audio.c src/common/jpt_cheats.c src/common/jpt_config.c src/common/jpt_display.c
  src/common/jpt_gamepad.c src/common/jpt_prompt_text.c src/common/jpt_l10n.c src/common/jpt_tr.c
  src/common/jpt_video_subtitles.c src/common/jpt_user_data.c src/common/jpt_fs_posix.c src/common/jpt_vpath.c
  src/common/jpt_reg_store.c src/common/jpt_sync.c)
target_include_directories(jpt_common_linux PUBLIC src/common)
target_compile_options(jpt_common_linux PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
target_link_libraries(jpt_common_linux PUBLIC SDL3::SDL3-static m)

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
set(JPT_STREAM_SOURCES src/host/stream_pack.c src/host/stream_vfile.c src/linux/linux_assets.c
                       ${JPT_LINUX_TRANSLATIONS_C} ${JPT_LINUX_RESTORED_C})
# The host features written against guest_abi.h (src/host/guest_abi.h): the same sources the Windows host builds, over the Linux
# implementation of that interface (src/linux/linux_host.c, linux_overrides.c).
set(JPT_GUEST_FEATURE_SOURCES
  src/host/guest_features.c src/host/ce_gameplay.c src/host/active_ai.c src/host/active_ai_policy.c
  src/host/patch117.c src/host/patch117_objects.c src/host/patch117_physics.c src/host/patch117_player.c
  src/host/patch117_rules.c src/host/patch117_policy.c src/host/physics_fixes.c src/host/physics_watch.c
  src/host/physics_watch_policy.c src/host/terrain_quality.c src/host/widescreen.c src/host/fov_policy.c
  src/host/perf_sections.c src/host/override_registry.c src/host/cheats.c src/host/cheats_policy.c src/host/skip_intro.c
  src/host/debug_menu.c src/host/debug_menu_policy.c src/host/objectives.c
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

add_library(jpt_linux_core STATIC ${JPT_STREAM_SOURCES} src/linux/linux_hle.c src/linux/linux_paths.c src/linux/linux_log.c src/linux/linux_crash.c src/linux/linux_shutdown.c src/linux/linux_guest_quit.c src/linux/linux_spine.c src/linux/linux_msvcrt.c src/linux/linux_modules.c src/linux/linux_msvcrt_io.c src/linux/linux_user32.c src/linux/linux_advapi32.c src/linux/linux_signatures.c src/linux/linux_processor.c src/linux/linux_window.c src/linux/linux_screen.c src/linux/linux_gdi32.c src/linux/linux_text.c src/linux/linux_stb_truetype.c ${JPT_LINUX_FONT_C} src/linux/linux_dsound.c src/linux/linux_smack.c src/linux/linux_video_subtitles.c src/linux/linux_mouse.c src/linux/linux_guest_ui.c src/linux/linux_overlay.c src/linux/linux_quick.c src/linux/linux_hostkeys.c src/linux/linux_settings.c src/host/guest_ui_model.c src/host/host_policy.c src/host/ce_policy.c src/host/window_title.c ${JPT_GUEST_FEATURE_SOURCES} ${JPT_AUDIO_CORE} ${AUDIO_DIR}/sdl_audio.c ${JPT_SMACKER_PORTABLE_SOURCES} src/linux/linux_ddraw.c src/linux/linux_lz32.c src/linux/linux_resource.c)
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
target_link_libraries(jpt_linux_core PUBLIC jpt_panel_linux jpt_host64 jpt_common_linux jpt_profile_linux)
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
jpt_linux_test(sync_tests SOURCES tests/sync_tests.c LIBS jpt_common_linux)
jpt_linux_test(reg_store_tests SOURCES tests/reg_store_tests.c LIBS jpt_common_linux)
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
jpt_linux_test(linux_guest_ptr_tests SOURCES tests/linux_guest_ptr_tests.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_call_spine_tests SOURCES tests/linux_call_spine_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core)
jpt_linux_test(linux_user32_tests SOURCES tests/linux_user32_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_gdi_tests SOURCES tests/linux_gdi_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
jpt_linux_test(linux_ddraw_tests SOURCES tests/linux_ddraw_tests.c tests/linux_fake_guest.c LIBS jpt_linux_core INCLUDES tests)
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
  add_library(jpt_lifted_objects OBJECT ${lift116_SOURCES})
  target_include_directories(jpt_lifted_objects PRIVATE ${PCRECOMP}/runtime/recomp32 src/common)
  target_compile_definitions(jpt_lifted_objects PRIVATE ${JPT_RECOMP_DEFINES})
  target_compile_options(jpt_lifted_objects PRIVATE -w ${JPT_LINUX_GEN_OPT})
  if(JPT_LINUX_SANITIZE)
    target_compile_options(jpt_lifted_objects PRIVATE -fno-sanitize=all)
  endif()
  add_library(jpt_lifted_linux STATIC $<TARGET_OBJECTS:jpt_lifted_objects>)

  # The lifted guest with its Linux bindings and the bootstrap, one archive so the guest's references to the runtime
  # (jpt_linux_core) and the bindings' references to the lifted bodies resolve in either order.
  add_library(jpt_linux_guest STATIC $<TARGET_OBJECTS:jpt_lifted_objects> src/linux/linux_lift116.c src/linux/linux_boot.c src/linux/linux_override_table.c)
  target_compile_options(jpt_linux_guest PRIVATE ${JPT_LINUX_WARNINGS} ${JPT_LINUX_COMPAT})
  target_link_libraries(jpt_linux_guest PUBLIC jpt_linux_core)
  jpt_linux_test(linux_boot_tests SOURCES tests/linux_boot_tests.c LIBS jpt_linux_guest)
  jpt_linux_test(linux_shutdown_tests SOURCES tests/linux_shutdown_tests.c tests/linux_fake_guest.c LIBS jpt_linux_guest INCLUDES tests)
  jpt_linux_test(linux_module_tests SOURCES tests/linux_module_tests.c LIBS jpt_linux_guest)
  jpt_linux_test(linux_session_tests SOURCES tests/linux_session_tests.c LIBS jpt_linux_guest)
  jpt_linux_test(linux_features_tests SOURCES tests/linux_features_tests.c LIBS jpt_linux_guest INCLUDES src/host)
  jpt_linux_test(linux_video_tests SOURCES tests/linux_video_tests.c LIBS jpt_linux_guest INCLUDES src/host/video src/host/audio)
endif()

# ---- the executable ----------------------------------------------------------------------------------------------
find_program(JPT_PYTHON NAMES python3 python REQUIRED)
set(JPT_BUILD_INFO_DIR ${CMAKE_BINARY_DIR}/buildinfo)
add_custom_target(jpt_build_info
  COMMAND ${JPT_PYTHON} -I ${CMAKE_SOURCE_DIR}/tools/gen_build_info.py --out ${JPT_BUILD_INFO_DIR}/jpt_build_info.h
          --version ${PROJECT_VERSION} --compiler "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
          --target "linux-${CMAKE_SYSTEM_PROCESSOR}" --config "${CMAKE_BUILD_TYPE}"
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
set_target_properties(trespasser_recomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
                      BUILD_RPATH "" INSTALL_RPATH "" SKIP_BUILD_RPATH ON)

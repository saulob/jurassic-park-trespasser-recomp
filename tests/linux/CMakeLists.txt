# The audio and Smacker layers built and tested natively on Linux (GCC or Clang, x86_64), headless: the same portable
# sources and the same test programs as the Windows gate, plus SDL3's dummy audio driver. Nothing of the game, its window,
# its launcher or its renderer is built. Run by tools/linux_audio_probe.sh (see docs/ARCHITECTURE.md).
cmake_minimum_required(VERSION 3.20)
project(jpt_linux_audio_probe LANGUAGES C)

if(NOT (UNIX AND NOT APPLE))
  message(FATAL_ERROR "this probe builds the Linux audio layer: run it on Linux (WSL included)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(JPT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${JPT_REPO_ROOT}/cmake")
include(JptAudio)

# SDL3 from the pinned checkout, static, with only what audio needs (no window system, no controllers).
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
add_subdirectory("${JPT_REPO_ROOT}/external/SDL" "${CMAKE_BINARY_DIR}/SDL" EXCLUDE_FROM_ALL)

set(JPT_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Werror)
set(JPT_AUDIO_CORE
  ${JPT_REPO_ROOT}/src/host/audio/audio_policy.c
  ${JPT_REPO_ROOT}/src/host/audio/audio_mixer.c
  ${JPT_REPO_ROOT}/src/host/audio/audio_spatial.c
  ${JPT_REPO_ROOT}/src/host/audio/audio_sync.c
  ${JPT_REPO_ROOT}/src/host/audio/dsound_compat.c)

function(jpt_probe name)
  add_executable(${name} ${JPT_REPO_ROOT}/tests/${name}.c ${ARGN})
  target_include_directories(${name} PRIVATE ${JPT_REPO_ROOT}/src/host/audio ${JPT_REPO_ROOT}/src/host/video)
  target_compile_options(${name} PRIVATE ${JPT_WARNINGS})
  target_compile_features(${name} PRIVATE c_std_11)
  target_link_libraries(${name} PRIVATE SDL3::SDL3-static m)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")
endfunction()

enable_testing()
jpt_probe(audio_portable_tests ${JPT_REPO_ROOT}/src/host/audio/audio_policy.c ${JPT_REPO_ROOT}/src/host/audio/audio_mixer.c)
jpt_probe(audio_backend_tests ${JPT_REPO_ROOT}/src/host/audio/sdl_audio.c ${JPT_REPO_ROOT}/src/host/audio/audio_mixer.c
  ${JPT_REPO_ROOT}/src/host/audio/audio_policy.c)
jpt_probe(dsound_compat_tests ${JPT_AUDIO_CORE})
jpt_probe(hammond_voiceover_tests ${JPT_AUDIO_CORE})
jpt_probe(smacker_tests ${JPT_AUDIO_CORE} ${JPT_SMACKER_PORTABLE_SOURCES})
jpt_probe(smacker_ffmpeg_tests ${JPT_AUDIO_CORE} ${JPT_SMACKER_PORTABLE_SOURCES})
jpt_smacker_use_ffmpeg(smacker_ffmpeg_tests)

# The whole portable layer, compiled together with the Linux platform list (empty for now): what a later native bootstrap
# links. Built, never run.
add_library(jpt_audio_portable STATIC ${JPT_AUDIO_PORTABLE_SOURCES} ${JPT_AUDIO_LINUX_SOURCES} ${JPT_SMACKER_PORTABLE_SOURCES})
target_include_directories(jpt_audio_portable PRIVATE ${JPT_REPO_ROOT}/src/host/audio ${JPT_REPO_ROOT}/src/host/video)
target_compile_options(jpt_audio_portable PRIVATE ${JPT_WARNINGS})
target_link_libraries(jpt_audio_portable PRIVATE SDL3::SDL3-static)
jpt_smacker_use_ffmpeg(jpt_audio_portable)

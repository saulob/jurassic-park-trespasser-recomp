# Source lists of the audio and Smacker layers, by platform. The portable lists compile anywhere; each platform adds only
# its bridge. JPT_REPO_ROOT is the repository root (set by the includer; the module's own parent folder otherwise).
if(NOT DEFINED JPT_REPO_ROOT)
  get_filename_component(JPT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(JPT_AUDIO_PORTABLE_SOURCES
  ${JPT_REPO_ROOT}/src/host/audio/audio_policy.c
  ${JPT_REPO_ROOT}/src/host/audio/audio_pacer.c
  ${JPT_REPO_ROOT}/src/host/audio/audio_mixer.c
  ${JPT_REPO_ROOT}/src/host/audio/audio_spatial.c
  ${JPT_REPO_ROOT}/src/host/audio/audio_sync.c
  ${JPT_REPO_ROOT}/src/host/audio/dsound_compat.c
  ${JPT_REPO_ROOT}/src/host/audio/sdl_audio.c)

set(JPT_SMACKER_PORTABLE_SOURCES
  ${JPT_REPO_ROOT}/src/host/video/smacker_header.c
  ${JPT_REPO_ROOT}/src/host/video/smacker_compat.c
  ${JPT_REPO_ROOT}/src/host/video/smacker_dsound_glue.c
  ${JPT_REPO_ROOT}/src/host/video/smacker_backend_ffmpeg.c)

set(JPT_AUDIO_WINDOWS_SOURCES
  ${JPT_REPO_ROOT}/src/host/audio/platform/windows_dsound_bridge.c
  ${JPT_REPO_ROOT}/src/host/audio/platform/windows_dsound_imports.c)

set(JPT_AUDIO_LINUX_SOURCES)

if(WIN32)
  set(JPT_AUDIO_PLATFORM_SOURCES ${JPT_AUDIO_WINDOWS_SOURCES})
elseif(UNIX AND NOT APPLE)
  set(JPT_AUDIO_PLATFORM_SOURCES ${JPT_AUDIO_LINUX_SOURCES})
else()
  set(JPT_AUDIO_PLATFORM_SOURCES)
endif()

# FFmpeg for the Smacker backend: libavformat, libavcodec and libavutil, found through pkg-config or an FFMPEG_ROOT
# (include/ and lib/). The backend is optional at build time; without it smk_ffmpeg_backend() is NULL.
option(JPT_SMACKER_FFMPEG "decode Smacker through FFmpeg libraries (the Smacker HLE backend)" ON)
option(JPT_FFMPEG_REQUIRE_LGPL "fail the configure step when FFmpeg is built with --enable-gpl or --enable-nonfree (releases)" OFF)
set(JPT_FFMPEG_FOUND OFF)
set(JPT_FFMPEG_INCLUDE_DIRS)
set(JPT_FFMPEG_LIBRARIES)
if(JPT_SMACKER_FFMPEG)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(JPT_FFMPEG_PKG QUIET libavformat libavcodec libavutil)
    if(JPT_FFMPEG_PKG_FOUND)
      set(JPT_FFMPEG_FOUND ON)
      set(JPT_FFMPEG_INCLUDE_DIRS ${JPT_FFMPEG_PKG_INCLUDE_DIRS})
      set(JPT_FFMPEG_LIBRARIES ${JPT_FFMPEG_PKG_LINK_LIBRARIES})
    endif()
  endif()
  if(NOT JPT_FFMPEG_FOUND AND FFMPEG_ROOT)
    find_path(JPT_FFMPEG_INCLUDE libavformat/avformat.h PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
    find_library(JPT_FFMPEG_AVFORMAT avformat PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(JPT_FFMPEG_AVCODEC avcodec PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(JPT_FFMPEG_AVUTIL avutil PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    if(JPT_FFMPEG_INCLUDE AND JPT_FFMPEG_AVFORMAT AND JPT_FFMPEG_AVCODEC AND JPT_FFMPEG_AVUTIL)
      set(JPT_FFMPEG_FOUND ON)
      set(JPT_FFMPEG_INCLUDE_DIRS ${JPT_FFMPEG_INCLUDE})
      set(JPT_FFMPEG_LIBRARIES ${JPT_FFMPEG_AVFORMAT} ${JPT_FFMPEG_AVCODEC} ${JPT_FFMPEG_AVUTIL})
    endif()
  endif()
endif()

set(JPT_HAS_FFMPEG ${JPT_FFMPEG_FOUND})
if(UNIX AND NOT APPLE)
  if(JPT_FFMPEG_FOUND)
    message(STATUS "FFmpeg for Smacker video: found (JPT_HAS_FFMPEG=1; libavformat, libavcodec, libavutil)")
    if(JPT_FFMPEG_REQUIRE_LGPL)
      set(_jpt_probe ${CMAKE_BINARY_DIR}/jpt_ffmpeg_license_probe.c)
      file(WRITE ${_jpt_probe} "#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <string.h>
int main(void) { const char* c = avcodec_configuration(); return strstr(c, \"--enable-gpl\") || strstr(c, \"--enable-nonfree\"); }
")
      try_run(_jpt_run _jpt_compile ${CMAKE_BINARY_DIR}/jpt_ffmpeg_license_probe ${_jpt_probe}
              CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${JPT_FFMPEG_INCLUDE_DIRS}" LINK_LIBRARIES ${JPT_FFMPEG_LIBRARIES})
      if(NOT _jpt_compile OR NOT _jpt_run STREQUAL "0")
        message(FATAL_ERROR "JPT_FFMPEG_REQUIRE_LGPL: this FFmpeg is configured with --enable-gpl or --enable-nonfree (or the probe did not build); a release needs an LGPL-only FFmpeg")
      endif()
    endif()
  else()
    message(STATUS "FFmpeg for Smacker video: NOT found (JPT_HAS_FFMPEG=0): the build works and the game skips its videos. "
                   "Ubuntu/Debian: sudo apt install libavformat-dev libavcodec-dev libavutil-dev (or pass -DFFMPEG_ROOT=<prefix with include/ and lib/>)")
  endif()
endif()

# Gives a target the FFmpeg backend when the libraries were found.
function(jpt_smacker_use_ffmpeg target)
  if(JPT_FFMPEG_FOUND)
    target_compile_definitions(${target} PRIVATE JPT_SMACKER_FFMPEG=1)
    target_include_directories(${target} PRIVATE ${JPT_FFMPEG_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE ${JPT_FFMPEG_LIBRARIES})
  endif()
endfunction()

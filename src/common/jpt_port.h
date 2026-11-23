/* Where the port lives: the folder that holds game, logs and user_data, the log folder fallback, the program to start again, and
 * the name the desktop knows the application by. Pure C over stdio and jpt_fs: the Windows and Linux builds and the
 * suites use the same code. An AppImage runs from a read-only mount that is gone once it exits, so there the port folder
 * is the folder of the AppImage file and a restart starts that file. */
#ifndef JPT_PORT_H
#define JPT_PORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Wayland app_id, the X11 window class and the .desktop file's name and StartupWMClass. */
#define JPT_APPLICATION_ID "io.github.saulob.jurassic-park-trespasser"
#define JPT_APPLICATION_NAME "Jurassic Park: Trespasser"
#define JPT_PORT_PATH_MAX 1024

/* 1 when appimage (the APPIMAGE variable) names a file. */
int jpt_port_in_appimage(const char* appimage);

/* 1 when running under Gamescope (non-Windows with a non-empty GAMESCOPE_WAYLAND_DISPLAY). */
int jpt_port_is_gamescope(const char* gamescope_wayland_display, int is_windows);
int jpt_port_in_gamescope(void);

/* The port folder: the folder of the AppImage file when appimage names one, else the folder of executable_path.
 * "." when the path has no folder. 0 when the answer does not fit in out. */
int jpt_port_folder(const char* executable_path, const char* appimage, char* out, size_t cap);

/* 1 when path lies inside folder (a mount point), compared by whole path components. */
int jpt_port_path_inside(const char* path, const char* folder);

/* The folder the logs go to: the folder of active_log when a log is already open, else <port_folder>/logs; inside an
 * AppImage that folder falls back to <user_root>/logs when it cannot be created or written. 0 when nothing fits. */
int jpt_port_logs_folder(const char* port_folder, const char* user_root, const char* active_log, int in_appimage, char* out,
                         size_t cap);

/* 1 when dir exists (or can be created) and a file can be written in it. */
int jpt_port_folder_writable(const char* dir);

/* The command that starts the port again: the AppImage file when appimage names one, else self_executable (never a path
 * inside the mount). out_argv[0] is that program, followed by argv[1..argc-1] and extra_arg when it is not NULL, then NULL.
 * The strings are not copied. Returns the argument count (without the NULL), -1 when it does not fit or there is no
 * program. */
int jpt_port_relaunch(const char* appimage, const char* self_executable, int argc, const char* const* argv,
                      const char* extra_arg, const char** out_argv, int out_cap);

#ifdef __cplusplus
}
#endif

#endif

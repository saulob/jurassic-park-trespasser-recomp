#include "jpt_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_fs.h"
#include "jpt_user_data.h"

static int is_sep(char c) { return c == '/' || c == '\\'; }

int jpt_port_in_appimage(const char* appimage) { return appimage && *appimage; }

int jpt_port_is_gamescope(const char* gamescope_wayland_display, int is_windows) {
    return !is_windows && gamescope_wayland_display && *gamescope_wayland_display;
}

int jpt_port_in_gamescope(void) {
#ifdef _WIN32
    return jpt_port_is_gamescope(NULL, 1);
#else
    return jpt_port_is_gamescope(getenv("GAMESCOPE_WAYLAND_DISPLAY"), 0);
#endif
}

static int parent_of(const char* path, char* out, size_t cap) {
    size_t n = strlen(path), cut = n;
    while (cut > 1 && is_sep(path[cut - 1])) cut--;
    while (cut > 0 && !is_sep(path[cut - 1])) cut--;
    if (cut == 0) {
        if (cap < 2) return 0;
        strcpy(out, ".");
        return 1;
    }
    while (cut > 1 && is_sep(path[cut - 1])) cut--;
    if (cut >= cap) return 0;
    memcpy(out, path, cut);
    out[cut] = 0;
    return 1;
}

int jpt_port_folder(const char* executable_path, const char* appimage, char* out, size_t cap) {
    if (!out || !cap) return 0;
    if (jpt_port_in_appimage(appimage)) return parent_of(appimage, out, cap);
    return parent_of(executable_path ? executable_path : "", out, cap);
}

int jpt_port_path_inside(const char* path, const char* folder) {
    size_t n;
    if (!path || !folder || !*folder) return 0;
    n = strlen(folder);
    while (n > 1 && is_sep(folder[n - 1])) n--;
    if (strncmp(path, folder, n)) return 0;
    return path[n] == 0 || is_sep(path[n]) || n == 1;
}

int jpt_port_folder_writable(const char* dir) {
    char probe[JPT_PORT_PATH_MAX + 32];
    FILE* f;
    int n;
    if (!dir || !*dir || !jpt_make_dirs(dir)) return 0;
    n = snprintf(probe, sizeof probe, "%s/.write_probe", dir);
    if (n <= 0 || (size_t)n >= sizeof probe) return 0;
    f = fopen(probe, "wb");
    if (!f) return 0;
    if (fputc('x', f) == EOF || fclose(f) != 0) {
        jpt_fs_remove(probe);
        return 0;
    }
    jpt_fs_remove(probe);
    return 1;
}

static int join(char* out, size_t cap, const char* base, const char* leaf) {
    size_t len = strlen(base);
    int n = snprintf(out, cap, len && is_sep(base[len - 1]) ? "%s%s" : "%s/%s", base, leaf);
    return n > 0 && (size_t)n < cap;
}

int jpt_port_logs_folder(const char* port_folder, const char* user_root, const char* active_log, int in_appimage, char* out,
                         size_t cap) {
    char portable[JPT_PORT_PATH_MAX];
    if (!out || !cap) return 0;
    if (active_log && *active_log) return parent_of(active_log, out, cap);
    if (!port_folder || !join(portable, sizeof portable, port_folder, "logs")) return 0;
    if (!in_appimage || jpt_port_folder_writable(portable)) {
        if (strlen(portable) >= cap) return 0;
        strcpy(out, portable);
        return 1;
    }
    return user_root && *user_root && join(out, cap, user_root, "logs");
}

int jpt_port_relaunch(const char* appimage, const char* self_executable, int argc, const char* const* argv,
                      const char* extra_arg, const char** out_argv, int out_cap) {
    const char* program = jpt_port_in_appimage(appimage) ? appimage : self_executable;
    int n = 0;
    if (!program || !*program || argc < 1 || !argv || !out_argv) return -1;
    if (out_cap < argc + (extra_arg ? 1 : 0) + 1) return -1;
    out_argv[n++] = program;
    for (int i = 1; i < argc; i++) out_argv[n++] = argv[i];
    if (extra_arg) out_argv[n++] = extra_arg;
    out_argv[n] = NULL;
    return n;
}

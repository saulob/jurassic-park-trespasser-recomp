#include "linux_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "jpt_fs.h"
#include "jpt_port.h"
#include "jpt_user_data.h"

static void fail(char* err, size_t len, const char* what, const char* path) {
    if (err && len) snprintf(err, len, "%s: %s", what, path);
}

static const char* pick(const char* argument, const char* variable) {
    const char* env = getenv(variable);
    if (argument && *argument) return argument;
    return env && *env ? env : NULL;
}

static int join(char* out, const char* base, const char* leaf) {
    size_t len = strlen(base);
    int n = snprintf(out, LX_PATH_MAX, len && base[len - 1] == '/' ? "%s%s" : "%s/%s", base, leaf);
    return n > 0 && n < LX_PATH_MAX;
}

int lx_paths_resolve(lx_paths_t* p, const char* exe_path, const char* game_arg, const char* user_root_arg,
                     const char* log_dir_arg, char* err, size_t err_len) {
    return lx_paths_resolve_for(p, exe_path, getenv("APPIMAGE"), game_arg, user_root_arg, log_dir_arg, err, err_len);
}

int lx_paths_resolve_for(lx_paths_t* p, const char* exe_path, const char* appimage, const char* game_arg,
                         const char* user_root_arg, const char* log_dir_arg, char* err, size_t err_len) {
    const char* chosen;
    memset(p, 0, sizeof *p);
    if (jpt_port_in_appimage(appimage)) {
        if (strlen(appimage) >= LX_PATH_MAX) return fail(err, err_len, "AppImage file name too long", appimage), 0;
        strcpy(p->appimage, appimage);
    }
    if (!jpt_port_folder(exe_path, appimage, p->port_dir, sizeof p->port_dir))
        return fail(err, err_len, "program folder name too long", exe_path ? exe_path : ""), 0;
    chosen = pick(game_arg, "TRESPASSER_GAME_DIR");
    if (chosen) {
        if (strlen(chosen) >= LX_PATH_MAX) return fail(err, err_len, "game folder name too long", chosen), 0;
        strcpy(p->game_dir, chosen);
    } else if (!join(p->game_dir, p->port_dir, "game")) {
        return fail(err, err_len, "program folder name too long", p->port_dir), 0;
    }
    chosen = pick(user_root_arg, "TRESPASSER_USER_DATA_ROOT");
    if (chosen) {
        if (strlen(chosen) >= LX_PATH_MAX) return fail(err, err_len, "user data folder name too long", chosen), 0;
        strcpy(p->user_root, chosen);
    } else if (!join(p->user_root, p->port_dir, "user_data")) {
        return fail(err, err_len, "program folder name too long", p->port_dir), 0;
    }
    chosen = pick(log_dir_arg, "TRESPASSER_LOG_DIR");
    if (chosen) {
        if (strlen(chosen) >= LX_PATH_MAX) return fail(err, err_len, "log folder name too long", chosen), 0;
        strcpy(p->logs_dir, chosen);
        p->logs_explicit = 1;
    } else if (!join(p->logs_dir, p->port_dir, "logs")) {
        return fail(err, err_len, "program folder name too long", p->port_dir), 0;
    }
    if (!join(p->launcher_dir, p->user_root, "launcher") || !join(p->saves_dir, p->user_root, "saves") ||
        !join(p->cache_dir, p->user_root, "cache"))
        return fail(err, err_len, "user data folder name too long", p->user_root), 0;
    return 1;
}

static int writable(const char* dir, char* err, size_t err_len) {
    if (!jpt_make_dirs(dir)) return fail(err, err_len, "cannot create folder", dir), 0;
    if (!jpt_port_folder_writable(dir)) return fail(err, err_len, "folder cannot be written", dir), 0;
    return 1;
}

int lx_paths_prepare(lx_paths_t* p, char* err, size_t err_len) {
    if (!writable(p->launcher_dir, err, err_len) || !writable(p->saves_dir, err, err_len) ||
        !writable(p->cache_dir, err, err_len))
        return 0;
    if (!p->logs_explicit && p->appimage[0] && !jpt_port_folder_writable(p->logs_dir)) {
        char fallback[LX_PATH_MAX];
        if (!jpt_port_logs_folder(p->port_dir, p->user_root, NULL, 1, fallback, sizeof fallback))
            return fail(err, err_len, "log folder name too long", p->user_root), 0;
        strcpy(p->logs_dir, fallback);
    }
    return writable(p->logs_dir, err, err_len);
}

int lx_relaunch(int argc, char** argv, const char* extra_arg) {
    const char* command[256];
    char self[LX_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    int count;
    self[n > 0 ? n : 0] = 0;
    count = jpt_port_relaunch(getenv("APPIMAGE"), self, argc, (const char* const*)argv, extra_arg, command, 256);
    if (count < 0) return 0;
    execv(command[0], (char* const*)command);
    return 0;
}

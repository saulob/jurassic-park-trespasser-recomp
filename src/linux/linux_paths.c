#include "linux_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_fs.h"
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
    const char* slash = exe_path ? strrchr(exe_path, '/') : NULL;
    const char* chosen;
    memset(p, 0, sizeof *p);
    if (slash && (size_t)(slash - exe_path) < LX_PATH_MAX) {
        memcpy(p->port_dir, exe_path, (size_t)(slash - exe_path));
        if (!p->port_dir[0]) strcpy(p->port_dir, "/");
    } else {
        strcpy(p->port_dir, ".");
    }
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
    } else if (!join(p->logs_dir, p->port_dir, "logs")) {
        return fail(err, err_len, "program folder name too long", p->port_dir), 0;
    }
    if (!join(p->launcher_dir, p->user_root, "launcher") || !join(p->saves_dir, p->user_root, "saves") ||
        !join(p->cache_dir, p->user_root, "cache"))
        return fail(err, err_len, "user data folder name too long", p->user_root), 0;
    return 1;
}

static int writable(const char* dir, char* err, size_t err_len) {
    char probe[LX_PATH_MAX + 32];
    FILE* f;
    if (!jpt_make_dirs(dir)) return fail(err, err_len, "cannot create folder", dir), 0;
    snprintf(probe, sizeof probe, "%s/.write_probe", dir);
    f = fopen(probe, "wb");
    if (!f || fputc('x', f) == EOF || fclose(f) != 0) {
        if (f) fclose(f);
        return fail(err, err_len, "folder cannot be written", dir), 0;
    }
    jpt_fs_remove(probe);
    return 1;
}

int lx_paths_prepare(const lx_paths_t* p, char* err, size_t err_len) {
    return writable(p->launcher_dir, err, err_len) && writable(p->saves_dir, err, err_len) &&
           writable(p->cache_dir, err, err_len) && writable(p->logs_dir, err, err_len);
}

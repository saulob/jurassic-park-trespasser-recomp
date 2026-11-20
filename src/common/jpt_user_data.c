#include "jpt_user_data.h"

#include <stdio.h>
#include <string.h>

#include "jpt_compat.h"
#include "jpt_fs.h"

int jpt_make_dirs(const char* path) {
    char buf[JPT_PATH_MAX];
    size_t n = strlen(path);
    if (!n || n >= sizeof buf) return 0;
    strcpy(buf, path);
    while (n > 3 && (buf[n - 1] == '\\' || buf[n - 1] == '/')) buf[--n] = 0;
    for (char* p = buf + 1; *p; p++) {
        if ((*p == '\\' || *p == '/') && p[-1] != ':') {
            char c = *p;
            *p = 0;
            jpt_fs_mkdir(buf);
            *p = c;
        }
    }
    return jpt_fs_mkdir(buf);
}

int jpt_is_user_file(const char* name) {
    return !_stricmp(name, JPT_REGISTRY_FILE) || !_strnicmp(name, "SAVEGAME.", 9);
}

typedef struct {
    const char* from;
    const char* user_dir;
    jpt_log_fn log;
    int copied, failed;
} carry_t;

static void say(jpt_log_fn log, const char* fmt, const char* a, const char* b) {
    char line[3 * JPT_PATH_MAX];
    if (!log) return;
    _snprintf(line, sizeof line - 1, fmt, a, b);
    line[sizeof line - 1] = 0;
    log(line);
}

static void carry_one(const char* name, int is_dir, void* user) {
    carry_t* c = (carry_t*)user;
    char src[JPT_PATH_MAX], dst[JPT_PATH_MAX];
    if (is_dir || !jpt_is_user_file(name)) return;
    _snprintf(src, sizeof src - 1, "%s/%s", c->from, name);
    _snprintf(dst, sizeof dst - 1, "%s/%s", c->user_dir, name);
    src[sizeof src - 1] = dst[sizeof dst - 1] = 0;
    if (jpt_fs_exists(dst)) return;
    if (!jpt_make_dirs(c->user_dir) || !jpt_fs_copy_new(src, dst)) {
        say(c->log, "could not copy %s to %s", src, dst);
        c->failed = 1;
        return;
    }
    say(c->log, "%s copied to %s", src, dst);
    c->copied++;
}

int jpt_carry_user_files(const char* from, const char* user_dir, jpt_log_fn log) {
    carry_t c = { from, user_dir, log, 0, 0 };
    if (!jpt_fs_list(from, carry_one, &c)) return 0;
    return c.failed ? -1 : c.copied;
}

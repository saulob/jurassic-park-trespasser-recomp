#define WIN32_LEAN_AND_MEAN
#include "jpt_user_data.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

int jpt_make_dirs(const char* path) {
    char buf[MAX_PATH];
    size_t n = strlen(path);
    if (!n || n >= sizeof buf) return 0;
    strcpy(buf, path);
    while (n > 3 && (buf[n - 1] == '\\' || buf[n - 1] == '/')) buf[--n] = 0;
    for (char* p = buf + 1; *p; p++) {
        if ((*p == '\\' || *p == '/') && p[-1] != ':') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = c;
        }
    }
    CreateDirectoryA(buf, NULL);
    {
        DWORD attributes = GetFileAttributesA(buf);
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
}

int jpt_is_user_file(const char* name) {
    return !_stricmp(name, JPT_REGISTRY_FILE) || !_strnicmp(name, "SAVEGAME.", 9);
}

static void say(jpt_log_fn log, const char* fmt, const char* a, const char* b) {
    char line[3 * MAX_PATH];
    if (!log) return;
    _snprintf(line, sizeof line - 1, fmt, a, b);
    line[sizeof line - 1] = 0;
    log(line);
}

int jpt_carry_user_files(const char* from, const char* user_dir, jpt_log_fn log) {
    char pattern[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    int copied = 0, failed = 0;
    _snprintf(pattern, sizeof pattern - 1, "%s\\*", from);
    pattern[sizeof pattern - 1] = 0;
    search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !jpt_is_user_file(found.cFileName)) continue;
        _snprintf(src, sizeof src - 1, "%s\\%s", from, found.cFileName);
        _snprintf(dst, sizeof dst - 1, "%s\\%s", user_dir, found.cFileName);
        src[sizeof src - 1] = dst[sizeof dst - 1] = 0;
        if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES) continue;
        if (!jpt_make_dirs(user_dir) || !CopyFileA(src, dst, TRUE)) {
            say(log, "could not copy %s to %s", src, dst);
            failed = 1;
            continue;
        }
        say(log, "%s copied to %s", src, dst);
        copied++;
    } while (FindNextFileA(search, &found));
    FindClose(search);
    return failed ? -1 : copied;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "jpt_fs.h"

#include <string.h>
#include <windows.h>

int jpt_fs_exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

int jpt_fs_is_dir(const char* path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int jpt_fs_is_link(const char* path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

int jpt_fs_mkdir(const char* path) {
    CreateDirectoryA(path, NULL);
    return jpt_fs_is_dir(path);
}

int jpt_fs_replace(const char* from, const char* to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int jpt_fs_remove(const char* path) { return DeleteFileA(path) != 0; }
int jpt_fs_remove_dir(const char* path) { return RemoveDirectoryA(path) != 0; }
int jpt_fs_copy_new(const char* src, const char* dst) { return CopyFileA(src, dst, TRUE) != 0; }

int jpt_fs_list(const char* dir, jpt_fs_visit_fn visit, void* user) {
    char pattern[JPT_PATH_MAX];
    WIN32_FIND_DATAA found;
    HANDLE search;
    if (strlen(dir) + 3 > sizeof pattern) return 0;
    strcpy(pattern, dir);
    strcat(pattern, "\\*");
    search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
        visit(found.cFileName, (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user);
    } while (FindNextFileA(search, &found));
    FindClose(search);
    return 1;
}
#endif

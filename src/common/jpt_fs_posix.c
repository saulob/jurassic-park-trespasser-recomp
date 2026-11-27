#ifndef _WIN32
#define _GNU_SOURCE
#include "jpt_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int jpt_fs_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int jpt_fs_is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int jpt_fs_is_link(const char* path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

int jpt_fs_mkdir(const char* path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) return 0;
    return jpt_fs_is_dir(path);
}

int jpt_fs_replace(const char* from, const char* to) {
    int fd = open(from, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    return rename(from, to) == 0;
}

int jpt_fs_remove(const char* path) { return unlink(path) == 0; }
int jpt_fs_remove_dir(const char* path) { return rmdir(path) == 0; }

int jpt_fs_copy_new(const char* src, const char* dst) {
    FILE *in, *out;
    char buffer[16384];
    size_t n;
    int ok = 1;
    int fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return 0;
    out = fdopen(fd, "wb");
    in = fopen(src, "rb");
    if (!out || !in) {
        if (in) fclose(in);
        if (out) fclose(out);
        unlink(dst);
        return 0;
    }
    while ((n = fread(buffer, 1, sizeof buffer, in)) > 0 && ok) ok = fwrite(buffer, 1, n, out) == n;
    ok = ok && !ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;
    if (!ok) unlink(dst);
    return ok;
}

/* The descriptor plus one, so that descriptor 0 is not NULL. */
jpt_fs_file_t* jpt_fs_open_read(const char* path, uint64_t* size) {
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }
    *size = (uint64_t)st.st_size;
    return (jpt_fs_file_t*)(intptr_t)(fd + 1);
}

int jpt_fs_read_at(void* file, uint64_t offset, void* buf, size_t n) {
    int fd = (int)(intptr_t)file - 1;
    size_t done = 0;
    while (done < n) {
        ssize_t got = pread(fd, (char*)buf + done, n - done, (off_t)(offset + done));
        if (got <= 0) return 0;
        done += (size_t)got;
    }
    return 1;
}

void jpt_fs_close(jpt_fs_file_t* file) {
    if (file) close((int)(intptr_t)file - 1);
}

int jpt_fs_list(const char* dir, jpt_fs_visit_fn visit, void* user) {
    DIR* d = opendir(dir);
    struct dirent* e;
    char path[JPT_PATH_MAX];
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) continue;
        visit(e->d_name, jpt_fs_is_dir(path), user);
    }
    closedir(d);
    return 1;
}
#endif

/* File system operations of the shared sources, over Win32 or POSIX. Paths are UTF-8 on POSIX and the ANSI code page on
 * Windows, as the shared sources already treat them. */
#ifndef JPT_FS_H
#define JPT_FS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_PATH_MAX 1024

int jpt_fs_exists(const char* path);
int jpt_fs_is_dir(const char* path);
/* A symbolic link, or a reparse point on Windows. */
int jpt_fs_is_link(const char* path);
/* Creates one folder; 1 when it exists afterwards. */
int jpt_fs_mkdir(const char* path);
/* Moves from onto to, replacing it, and flushes it to disk. */
int jpt_fs_replace(const char* from, const char* to);
int jpt_fs_remove(const char* path);
int jpt_fs_remove_dir(const char* path);
/* Copies src to dst only when dst does not exist. */
int jpt_fs_copy_new(const char* src, const char* dst);
/* A UTF-8 path as these functions take it; 0 when the ANSI code page cannot spell it exactly. */
int jpt_fs_from_utf8(const char* utf8, char* out, size_t cap);

/* A file read by offset, by any thread; NULL when it cannot be opened. jpt_fs_read_at is 1 when all n bytes were read. */
typedef struct jpt_fs_file jpt_fs_file_t;
jpt_fs_file_t* jpt_fs_open_read(const char* path, uint64_t* size);
int jpt_fs_read_at(void* file, uint64_t offset, void* buf, size_t n);
void jpt_fs_close(jpt_fs_file_t* file);

typedef void (*jpt_fs_visit_fn)(const char* name, int is_dir, void* user);
/* Calls visit for each entry of dir, without "." and "..". Returns 0 when dir cannot be listed. */
int jpt_fs_list(const char* dir, jpt_fs_visit_fn visit, void* user);

#ifdef __cplusplus
}
#endif

#endif

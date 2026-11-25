/* File system operations of the shared sources, over Win32 or POSIX. Paths are UTF-8 on POSIX and the ANSI code page on
 * Windows, as the shared sources already treat them. */
#ifndef JPT_FS_H
#define JPT_FS_H

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

typedef void (*jpt_fs_visit_fn)(const char* name, int is_dir, void* user);
/* Calls visit for each entry of dir, without "." and "..". Returns 0 when dir cannot be listed. */
int jpt_fs_list(const char* dir, jpt_fs_visit_fn visit, void* user);

#ifdef __cplusplus
}
#endif

#endif

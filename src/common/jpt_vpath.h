/* Guest (Win32) paths mapped onto a host directory tree: each name is matched case-insensitively against the real
 * directory entries, one directory at a time, and cached. ".." above a mount is an error, never a clamp. */
#ifndef JPT_VPATH_H
#define JPT_VPATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jpt_vpath jpt_vpath_t;

enum {
    JPT_VP_OK = 0,
    JPT_VP_NOT_FOUND = -1, /* an element does not exist (create mode: the parent) */
    JPT_VP_ESCAPE = -2,    /* ".." above the root of the mount */
    JPT_VP_BAD_PATH = -3,  /* empty, unknown drive, control character, name too long */
    JPT_VP_NO_SPACE = -4   /* the host path does not fit the buffer */
};

jpt_vpath_t* jpt_vpath_new(void);
void jpt_vpath_free(jpt_vpath_t* v);

/* Maps a drive letter to a host directory (up to 8 drives). The first mapped drive is the current drive. */
int jpt_vpath_mount(jpt_vpath_t* v, char drive, const char* host_dir);
/* The guest's current directory ("C:\" style, no host path involved); relative paths start there. */
int jpt_vpath_chdir(jpt_vpath_t* v, const char* guest_dir);
const char* jpt_vpath_cwd(const jpt_vpath_t* v);

/* Resolves a guest path to a host path. With create == 0 every element must exist (one that exists under another case
 * is returned as the host spells it). With create != 0 the last element need not exist: it is returned as given, or as
 * the existing entry spells it, and its parent must exist. */
int jpt_vpath_resolve(jpt_vpath_t* v, const char* guest_path, int create, char* host_path, size_t cap);

/* The overlay answers reads below "<drive>:\\<folder>": a file it finds (by its path relative to that folder) takes the
 * place of the mounted one. Nothing is created, changed or removed there while an overlay is set. */
typedef int (*jpt_vpath_overlay_fn)(const char* relative, char* host_path, size_t cap);
void jpt_vpath_set_overlay(jpt_vpath_t* v, char drive, const char* folder, jpt_vpath_overlay_fn find);
/* jpt_vpath_resolve without create, for a read: the overlay first. *overlaid (may be NULL) tells which one answered. */
int jpt_vpath_resolve_read(jpt_vpath_t* v, const char* guest_path, char* host_path, size_t cap, int* overlaid);
int jpt_vpath_write_refused(jpt_vpath_t* v, const char* guest_path);

/* Tells the cache a file or folder was created, renamed or removed under host_dir, so the next lookup lists the folder
 * again. A lookup also lists again once when a name is missing. */
void jpt_vpath_invalidate(jpt_vpath_t* v, const char* host_dir);

/* Counters for tests and the log: folder listings made and lookups answered from the cache. */
void jpt_vpath_stats(const jpt_vpath_t* v, unsigned* listings, unsigned* cache_hits);

#ifdef __cplusplus
}
#endif

#endif

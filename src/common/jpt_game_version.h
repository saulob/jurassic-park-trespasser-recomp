/* The game versions the launcher can import and where each one is installed: game\versions\<id>, or the game folder
 * itself for an installation made before versions existed. Shared by the host, the launcher and the tests. */
#ifndef JPT_GAME_VERSION_H
#define JPT_GAME_VERSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one program carries every lift and runs the version's own. The guest image is the installed file the guest's
 * memory is laid out from; no original executable is ever opened (the lifts' source builds are development metadata). */
#define JPT_GUEST_IMAGE "guest.img"
#define JPT_VERSIONS_DIR "versions"
#define JPT_GAME_VERSION_ID_MAX 16

typedef struct {
    const char* id;              /* folder under game\versions and the `game_version` setting */
    const char* label;           /* "English · Build 116" */
    const char* language;        /* English name of the game's own language */
    const char* language_code;   /* jpt_l10n code */
    const char* release;         /* who made it */
    int build;                   /* the build number the executable reports ("Trespasser Build: 1, 0, N") */
    const char* lift;            /* the lifted code that runs it: lift116, lift_es117, lift_ptbr117 */
    const char* text_strings;    /* the release's own string table (jpt_string_table.h), relative to the version folder, or NULL */
    int text_codepage;           /* code page of the release's own text */
    const char* font_file;       /* the release's font, drawn in place of the game's Arial, or NULL */
    const char* font_face;
    const char* saves;           /* folder under the user data root; NULL: the root's saves folder */
    const char* unplayable;      /* why the port does not run it yet; NULL once every feature is verified for it */
} jpt_game_version_t;

#define JPT_GAME_VERSION_COUNT 4
extern const jpt_game_version_t jpt_game_versions[JPT_GAME_VERSION_COUNT];

const jpt_game_version_t* jpt_game_version_by_id(const char* id);
int jpt_game_version_playable(const jpt_game_version_t* v);

/* 64 hex digits of a file's SHA-256; 0 when it cannot be read. */
int jpt_file_sha256(const char* path, char out[65]);

/* <game_root>\versions\<id>. 0 when it does not fit. */
int jpt_game_version_folder(const char* game_root, const jpt_game_version_t* v, char* out, size_t cap);

/* Where v is installed: its managed folder when that holds the guest image. 0 otherwise. */
int jpt_game_version_installed(const char* game_root, const jpt_game_version_t* v, char* out, size_t cap);

/* The installed version to run: `wanted` when it is installed (and playable, with playable_only), else the first
 * installed one in table order. 0 when there is none. */
int jpt_game_version_resolve(const char* game_root, const char* wanted, int playable_only, char* dir, size_t cap,
                             const jpt_game_version_t** out);

/* The game's Installed Directory for v: default_saves for a version without its own folder, else
 * <user_root>\<v->saves>. 0 when it does not fit. */
int jpt_game_version_saves_dir(const char* user_root, const char* default_saves, const jpt_game_version_t* v,
                               char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif

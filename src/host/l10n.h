/* The chosen language's text at run time (jpt_l10n.h): the game's string IDs, its voice captions, the menu labels
 * of its DDF files and the fonts its text is drawn with. English everywhere a translation has nothing. */
#ifndef L10N_H
#define L10N_H

#ifdef _WIN32
#include "host.h"
#else
#include "guest_abi.h"
#endif
#include "jpt_game_version.h"

/* Loads the embedded text of the chosen language (jpt_l10n.h). With English, a version whose release translated the
 * string table (version->text_strings) shows that text instead; otherwise nothing changes. Before the guest runs. */
void l10n_init(const char* cache_dir, const char* game_dir, const char* game_host_dir, const char* language, const jpt_game_version_t* version);
/* The folder the game's saves are in: a menu DDF opened from it is the game's own too (jpt_menu_ddf_source). */
void l10n_set_user_folder(const char* folder);
int l10n_active(void);
/* The language in use is the one the game version was released in: its own menu pictures and text stay as they are. */
int l10n_release_language(void);
/* The translation of a string ID: in the display code page for the game, in UTF-8 for the port's own panels. */
const char* l10n_text(unsigned id);
const char* l10n_text_utf8(unsigned id);
/* The voice captions and level TEXT actions. */
void l10n_install(void);
/* The language's text for a menu label as a DDF writes it in English ("World Initialization"), in the display code page;
 * NULL when the text is not such a label or English is in use. For text the frontend and the loading screen read from the
 * game's own controls. */
const char* l10n_label(const char* english);
/* A translated copy of a menu DDF the game opens, in the cache folder: 1 with its path in out. */
int l10n_menu_file(const char* path, char* out, size_t cap);
#ifndef _WIN32
/* LoadStringA's answer when the language has the string: its length, or -1 to leave the game's own. */
int l10n_load_string(unsigned id, char* buffer, int cap);
#endif
#ifdef _WIN32
/* MessageBoxA's text and caption in the display code page, shown as Unicode. */
int l10n_message_box(HWND owner, const char* text, const char* caption, UINT type);
#endif

#endif

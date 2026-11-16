#ifndef TRESPASSER_MENU_AUDIO_POLICY_H
#define TRESPASSER_MENU_AUDIO_POLICY_H

#include <stddef.h>

/* The menus' sounds (selection sound, music, birds) are read from <Installed Directory>\menu.tpa. The port's Installed
 * Directory is the saves folder, so the same file is served from <game>\data\Menu.tpa. Returns 1 and fills `out` when
 * `path` is that file. */
int menu_audio_source(const char* path, const char* installed_dir, const char* game_dir, char* out, size_t cap);

#endif

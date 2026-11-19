#ifndef TRESPASSER_MENU_AUDIO_POLICY_H
#define TRESPASSER_MENU_AUDIO_POLICY_H

#include <stddef.h>

/* The menus' sounds (selection sound, music, birds) are read from <Installed Directory>\menu.tpa. The port's Installed
 * Directory is the saves folder, so the same file is served from <game>\data\Menu.tpa. Returns 1 and fills `out` when
 * `path` is that file. */
int menu_audio_source(const char* path, const char* installed_dir, const char* game_dir, char* out, size_t cap);

/* Amplitude of the menus' selection sound relative to the game's own level. */
#define MENU_EFFECT_GAIN 0.5f

/* The argument for the game's sample volume setter (dB, added to the sample's attenuation) that leaves a sample whose
 * volume is current_db at MENU_EFFECT_GAIN of its amplitude. */
float menu_effect_volume_arg(float current_db, float attenuation_db);

#endif

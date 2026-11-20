#include "jpt_cheats.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_config.h"

static const jpt_cheat_choice_t k_speed_choices[JPT_CHEAT_SPEED_COUNT] = {
    { "0.5", "0.5x", 0.5f }, { "1", "1.0x", 1.0f },  { "1.5", "1.5x", 1.5f }, { "2", "2.0x", 2.0f },
    { "3", "3.0x", 3.0f },   { "5", "5.0x", 5.0f },  { "7.5", "7.5x", 7.5f }, { "10", "10.0x", 10.0f },
};

static const jpt_cheat_choice_t k_jump_choices[JPT_CHEAT_JUMP_COUNT] = {
    { "0.5", "0.5x", 0.5f }, { "1", "1.0x", 1.0f }, { "1.5", "1.5x", 1.5f },
    { "2", "2.0x", 2.0f },   { "3", "3.0x", 3.0f }, { "5", "5.0x", 5.0f },
};

const jpt_cheat_info_t jpt_cheats[JPT_CHEAT_COUNT] = {
    { JPT_CHEAT_SPEED, JPT_CHEAT_CHOICE, JPT_KEY_CHEAT_MOVEMENT_SPEED, "Movement Speed",
      "Multiplies how fast Anne walks and runs. 1.0x is the original speed.", JPT_CHEAT_MULTIPLIER_DEFAULT,
      k_speed_choices, JPT_CHEAT_SPEED_COUNT },
    { JPT_CHEAT_JUMP, JPT_CHEAT_CHOICE, JPT_KEY_CHEAT_JUMP_HEIGHT, "Jump Height",
      "Multiplies how high Anne's jumps go. Gravity and falls are unchanged. 1.0x is the original height.",
      JPT_CHEAT_MULTIPLIER_DEFAULT, k_jump_choices, JPT_CHEAT_JUMP_COUNT },
    { JPT_CHEAT_INVULNERABLE, JPT_CHEAT_TOGGLE, JPT_KEY_CHEAT_INVULNERABILITY, "Invulnerability",
      "Anne takes no damage from hits, bites, shots or falls. Level-scripted deaths still happen.", 0, NULL, 0 },
    { JPT_CHEAT_AMMO, JPT_CHEAT_TOGGLE, JPT_KEY_CHEAT_INFINITE_AMMO, "Infinite Ammo",
      "Guns do not use up ammunition.", 0, NULL, 0 },
    { JPT_CHEAT_FREEZE_AI, JPT_CHEAT_TOGGLE, JPT_KEY_CHEAT_FREEZE_DINOSAURS, "Freeze Dinosaurs",
      "Dinosaurs sleep and stop updating.", 0, NULL, 0 },
    { JPT_CHEAT_HEAL, JPT_CHEAT_ACTION, NULL, "Heal", "Anne's health back to her maximum, once.", 0, NULL, 0 },
    { JPT_CHEAT_SUPER_HEALTH, JPT_CHEAT_TOGGLE, JPT_KEY_CHEAT_SUPER_HEALTH, "Super Health",
      "Four times Anne's normal maximum health, and full. Saves keep the normal maximum.", 0, NULL, 0 },
    { JPT_CHEAT_NOCLIP, JPT_CHEAT_TOGGLE, NULL, "Noclip",
      "Walk through walls and geometry. Anne stays where Noclip ends.", 0, NULL, 0 },
    { JPT_CHEAT_STEALTH, JPT_CHEAT_TOGGLE, NULL, "Stealth Mode",
      "Creatures keep acting but no longer notice Anne.", 0, NULL, 0 },
    { JPT_CHEAT_TELEPORT, JPT_CHEAT_ACTION, NULL, "Teleport", "Moves Anne to the coordinates entered.", 0, NULL, 0 },
    { JPT_CHEAT_LEVEL_SELECT, JPT_CHEAT_ACTION, NULL, "Level Select", "Loads another level.", 0, NULL, 0 },
    { JPT_CHEAT_FREE_CAMERA, JPT_CHEAT_TOGGLE, NULL, "Fly / Free Camera",
      "A camera that moves on its own while Anne stays where she is.", 0, NULL, 0 },
};

int jpt_cheat_is_saved(const jpt_cheat_info_t* info) { return info->key != NULL && info->kind != JPT_CHEAT_ACTION; }

static int value_count(const jpt_cheat_info_t* info) {
    return info->kind == JPT_CHEAT_CHOICE ? info->choice_count : info->kind == JPT_CHEAT_TOGGLE ? 2 : 0;
}

int jpt_cheat_valid_value(const jpt_cheat_info_t* info, int value) { return value >= 0 && value < value_count(info); }

const char* jpt_cheat_saved_text(const jpt_cheat_info_t* info, int value) {
    if (!jpt_cheat_valid_value(info, value)) value = info->default_value;
    if (info->kind == JPT_CHEAT_CHOICE) return info->choices[value].saved;
    return value ? "1" : "0";
}

const char* jpt_cheat_value_text(const jpt_cheat_info_t* info, int value) {
    if (!jpt_cheat_valid_value(info, value)) value = info->default_value;
    if (info->kind == JPT_CHEAT_CHOICE) return info->choices[value].label;
    return value ? "On" : "Off";
}

int jpt_cheat_parse(const jpt_cheat_info_t* info, const char* text, int* value) {
    if (!text) return 0;
    if (info->kind == JPT_CHEAT_TOGGLE) return jpt_parse_bool(text, value);
    if (info->kind == JPT_CHEAT_CHOICE) {
        char* end;
        double v = strtod(text, &end);
        if (end == text || *end || !isfinite(v)) return 0;
        for (int i = 0; i < info->choice_count; i++)
            if (fabs(v - (double)info->choices[i].value) < 1e-6) return *value = i, 1;
    }
    return 0;
}

int jpt_cheat_next(const jpt_cheat_info_t* info, int value) {
    int n = value_count(info);
    if (!n) return value;
    return jpt_cheat_valid_value(info, value) ? (value + 1) % n : info->default_value;
}

void jpt_cheat_session_start(jpt_cheat_session_t* s, const int saved[JPT_CHEAT_COUNT]) {
    for (int i = 0; i < JPT_CHEAT_COUNT; i++) {
        const jpt_cheat_info_t* info = &jpt_cheats[i];
        s->saved[i] = saved && jpt_cheat_is_saved(info) && jpt_cheat_valid_value(info, saved[i]) ? saved[i]
                                                                                                  : info->default_value;
        s->changed[i] = -1;
    }
}

int jpt_cheat_effective(const jpt_cheat_session_t* s, jpt_cheat_t cheat) {
    if (cheat < 0 || cheat >= JPT_CHEAT_COUNT) return 0;
    return s->changed[cheat] < 0 ? s->saved[cheat] : s->changed[cheat];
}

int jpt_cheat_set(jpt_cheat_session_t* s, jpt_cheat_t cheat, int value) {
    if (cheat < 0 || cheat >= JPT_CHEAT_COUNT || !jpt_cheat_valid_value(&jpt_cheats[cheat], value))
        return jpt_cheat_effective(s, cheat);
    s->changed[cheat] = value == s->saved[cheat] ? -1 : value;
    return value;
}

int jpt_cheat_cycle(jpt_cheat_session_t* s, jpt_cheat_t cheat) {
    if (cheat < 0 || cheat >= JPT_CHEAT_COUNT) return 0;
    return jpt_cheat_set(s, cheat, jpt_cheat_next(&jpt_cheats[cheat], jpt_cheat_effective(s, cheat)));
}

void jpt_cheat_clear_changes(jpt_cheat_session_t* s) {
    for (int i = 0; i < JPT_CHEAT_COUNT; i++) s->changed[i] = -1;
}

int jpt_cheat_format_saved(char* out, size_t cap, const jpt_cheat_session_t* s) {
    size_t used = 0;
    if (!cap) return 0;
    out[0] = 0;
    for (int i = 0; i < JPT_CHEAT_COUNT; i++) {
        const jpt_cheat_info_t* info = &jpt_cheats[i];
        int n;
        if (!jpt_cheat_is_saved(info)) continue;
        n = snprintf(out + used, cap - used, "%s%s %s", used ? ", " : "", info->label,
                     jpt_cheat_value_text(info, s->saved[i]));
        if (n < 0 || (size_t)n >= cap - used) return 0;
        used += (size_t)n;
    }
    return 1;
}

/* The F5 Debug / Test Menu, F10 Cheats, F3 Objectives, Show Objective and the compass in every language: each label the
 * panels draw comes from the language's interface table, nothing stays English, numbers and coordinates do not change. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats_policy.h"
#include "compass_policy.h"
#include "debug_menu_policy.h"
#include "jpt_l10n.h"
#include "jpt_tr.h"
#include "objective_policy.h"

#ifdef JPT_SOURCE_DIR
#define TRANSLATIONS JPT_SOURCE_DIR "/assets/translations/"
#else
#define TRANSLATIONS "assets/translations/"
#endif

static int* g_checks;
static int* g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        (*g_checks)++;                                                                                                  \
        if (!(cond)) {                                                                                                  \
            (*g_failures)++;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

static int load(const char* code) {
    char path[512];
    FILE* f;
    long n;
    char* data;
    int count = -1;
    snprintf(path, sizeof path, TRANSLATIONS "%s.interface.txt", code);
    if (!(f = fopen(path, "rb"))) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char*)malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) == (size_t)n) count = jpt_tr_load(data, (size_t)n, 0);
    free(data);
    fclose(f);
    return count > 0;
}

/* Labels a language writes as English does. */
static int shared(const char* code, const char* english) {
    static const struct {
        const char *code, *text;
    } k[] = { { "de", "Level" }, { "de", "Cheats" }, { "it", "Camera" }, { "pt-BR", "Normal" }, { "de", "Normal" }, { "es", "Normal" } };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(k[i].code, code) && !strcmp(k[i].text, english)) return 1;
    return !strcmp(english, "X") || !strcmp(english, "Y") || !strcmp(english, "Z") || !strcmp(english, "HD") ||
           !strcmp(english, "Noclip");
}

void menu_l10n_tests(int* checks, int* failures) {
    static const char* const languages[] = { "pt-BR", "fr", "de", "es", "ru", "it" };
    static const char* const scenes[] = { "be.scn", "jr.scn", "ij.scn", "it.scn", "lab.scn", "as.scn", "as2.scn",
                                          "sum.scn" };
    char english_items[DM_ITEM_COUNT][64], english_levels[8][64];
    g_checks = checks;
    g_failures = failures;
    jpt_tr_reset();
    for (size_t s = 0; s < sizeof scenes / sizeof scenes[0]; s++)
        snprintf(english_levels[s], sizeof english_levels[s], "%s", dm_level_name(scenes[s]));
    for (int i = 0; i < DM_ITEM_COUNT; i++) snprintf(english_items[i], sizeof english_items[i], "%s", dm_item_label(DM_MENU_CHEATS, (dm_item_t)i));
    for (size_t l = 0; l < sizeof languages / sizeof languages[0]; l++) {
        const char* code = languages[l];
        char text[128];
        CHECK(load(code), "%s: interface table", code);
        for (int i = 0; i < DM_ITEM_COUNT; i++) {
            const char* label = dm_item_label(DM_MENU_CHEATS, (dm_item_t)i);
            CHECK(strcmp(label, english_items[i]) || shared(code, english_items[i]), "%s menu item %d \"%s\" English", code, i, label);
        }
        CHECK(strcmp(dm_item_label(DM_MENU_CHEATS, DM_FREEZE_AI), "Freeze Dinosaurs"), "%s F10 Freeze Dinosaurs", code);
        for (int i = 0; i < JPT_CHEAT_COUNT; i++)
            CHECK(jpt_tr_has(jpt_cheats[i].label), "%s F10 has a translation of %s", code, jpt_cheats[i].label);
        CHECK(strcmp(dm_menu_title(DM_MENU_OBJECTIVES), "Objectives"), "%s titles", code);
        CHECK(strcmp(dm_menu_title(DM_MENU_CHEATS), "Cheats") || !strcmp(code, "de"), "%s Cheats title", code);
        for (size_t s = 0; s < sizeof scenes / sizeof scenes[0]; s++)
            CHECK(dm_level_name(scenes[s]) && strcmp(dm_level_name(scenes[s]), english_levels[s]), "%s level name of %s", code,
                  scenes[s]);
        CHECK(ch_format_change(text, sizeof text, DM_ACT_SPEED, 1, 7) && strstr(text, "10.0x") && !strstr(text, "Movement"),
              "%s F10 speed: %s", code, text);
        CHECK(ch_format_change(text, sizeof text, DM_ACT_INVULNERABLE, 1, 0) && !strstr(text, "On"), "%s F10: %s", code, text);
        for (int deg = 0; deg < 360; deg += 45) CHECK(cp_direction_name(deg) && cp_direction_name(deg)[0], "%s compass %d", code, deg);
        CHECK(!strcmp(cp_direction_name(0), !strcmp(code, "ru") ? "\xD0\xA1" : "N"), "%s north", code);
        CHECK(!strcmp(cp_direction_name(90), !strcmp(code, "ru") ? "\xD0\x92" : !strcmp(code, "de") ? "O" : !strcmp(code, "pt-BR") ? "L" : "E"),
              "%s east: %s", code, cp_direction_name(90));
        CHECK(!strcmp(cp_direction_name(270), !strcmp(code, "ru") ? "\xD0\x97" : !strcmp(code, "de") ? "W" : "O"), "%s west", code);
        CHECK(cp_format_distance(42.0f, text, sizeof text) && !strcmp(text, !strcmp(code, "ru") ? "42 \xD0\xBC" : "42 m"),
              "%s distance: %s", code, text);
        CHECK(cp_format_distance(1500.0f, text, sizeof text) && strstr(text, "1.5"), "%s km keeps its number: %s", code, text);
        for (int i = 0; i < ob_level_count(); i++) {
            const ob_level_t* lv = ob_level_at(i);
            for (int k = 0; k < lv->count; k++) {
                CHECK(jpt_tr_has(lv->items[k].title), "%s objective \"%s\"", code, lv->items[k].title);
                CHECK(strcmp(jpt_tr(lv->items[k].title), lv->items[k].title), "%s objective \"%s\" English", code,
                      lv->items[k].title);
            }
        }
    }
    jpt_tr_reset();
    CHECK(!strcmp(dm_item_label(DM_MENU_CHEATS, DM_CLOSE), "Close") && !strcmp(cp_direction_name(0), "N"), "English again");
}

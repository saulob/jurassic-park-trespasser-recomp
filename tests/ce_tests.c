/*
 * Headless checks of the Trespasser CE features reimplemented in the port: Quick Save / Quick Load decisions and files,
 * the Save dialog's list without the Quick Save, Dual stow's choices over the retail stow handler, their settings and
 * the F7 / F8 keys. Build: CMake target ce_tests.
 */
#include <math.h>
#include "test_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ce_effects.h"
#include "ce_policy.h"
#include "host_policy.h"
#include "jpt_config.h"

#ifndef JPT_SOURCE_DIR
#define JPT_SOURCE_DIR "."
#endif

static int s_failures, s_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

enum { WM_KEYDOWN_ = 0x100, WM_KEYUP_ = 0x101, WM_SYSKEYDOWN_ = 0x104, VK_F5_ = 0x74, VK_F7_ = 0x76, VK_F8_ = 0x77,
       VK_F9_ = 0x78, VK_F10_ = 0x79 };

static qs_world_t world(qs_place_t place) {
    qs_world_t w;
    memset(&w, 0, sizeof w);
    w.place = place;
    w.player_alive = 1;
    w.file_exists = 1;
    return w;
}

static void test_quick_save(void) {
    qs_world_t w = world(QS_PLACE_LEVEL);
    CHECK(qs_decide_save(&w) == QS_DO_SAVE);
    w.file_exists = 0;
    CHECK(qs_decide_save(&w) == QS_DO_SAVE);
    w.player_alive = 0;
    CHECK(qs_decide_save(&w) == QS_REFUSE_DEAD);
    w = world(QS_PLACE_LEVEL);
    w.host_panel = 1;
    CHECK(qs_decide_save(&w) == QS_REFUSE_BUSY);
    w = world(QS_PLACE_LEVEL);
    w.load_pending = 1;
    CHECK(qs_decide_save(&w) == QS_REFUSE_BUSY);
    w = world(QS_PLACE_MAIN_MENU);
    CHECK(qs_decide_save(&w) == QS_REFUSE_PLACE);
    w = world(QS_PLACE_NONE);
    CHECK(qs_decide_save(&w) == QS_REFUSE_PLACE);
}

static void test_quick_load(void) {
    qs_world_t w = world(QS_PLACE_LEVEL);
    CHECK(qs_decide_load(&w) == QS_DO_LOAD_LEVEL);
    w.player_alive = 0;
    CHECK(qs_decide_load(&w) == QS_DO_LOAD_LEVEL);
    w = world(QS_PLACE_MAIN_MENU);
    CHECK(qs_decide_load(&w) == QS_DO_LOAD_MAIN_MENU);
    w = world(QS_PLACE_NONE);
    CHECK(qs_decide_load(&w) == QS_REFUSE_PLACE);
    w = world(QS_PLACE_LEVEL);
    w.host_panel = 1;
    CHECK(qs_decide_load(&w) == QS_REFUSE_BUSY);
}

static void test_missing_quick_save(void) {
    qs_world_t w = world(QS_PLACE_LEVEL);
    w.file_exists = 0;
    CHECK(qs_decide_load(&w) == QS_REFUSE_MISSING);
    w.place = QS_PLACE_MAIN_MENU;
    CHECK(qs_decide_load(&w) == QS_REFUSE_MISSING);
}

static void test_files(void) {
    char path[64], name[32];
    CHECK(qs_path("C:\\user_data\\saves\\", path, sizeof path) && !strcmp(path, "C:\\user_data\\saves\\SAVEGAME.QCK"));
    CHECK(qs_path("C:\\saves", path, sizeof path) && !strcmp(path, "C:\\saves\\SAVEGAME.QCK"));
    CHECK(!qs_path("C:\\a-folder-whose-path-is-far-too-long-for-the-buffer\\", path, 24) && !path[0]);
    CHECK(!qs_path("", path, sizeof path));
    CHECK(qs_is_quick_file("SAVEGAME.QCK") && qs_is_quick_file("c:\\x\\savegame.qck") && qs_is_quick_file("x/SAVEGAME.QCK"));
    CHECK(!qs_is_quick_file("SAVEGAME.001") && !qs_is_quick_file("SAVEGAME.QCK.jpg") && !qs_is_quick_file(NULL));
    qs_display_name("Salvamento r\xC3\xA1pido", name, sizeof name);
    CHECK(strlen(name) <= QS_NAME_MAX && !strcmp(name, "Salvamento r\xC3\xA1pido"));
    qs_display_name("A save name longer than twenty characters", name, sizeof name);
    CHECK(strlen(name) == QS_NAME_MAX && !strncmp(name, "A save name longer t", QS_NAME_MAX));
}

/* The retail list entry: 84 bytes, file name at +0x28. The Quick Save is a retail save under its own name. */
static void test_save_list(void) {
    enum { STRIDE = 0x54, FILE_AT = 0x28 };
    static const char* const names[] = { "SAVEGAME.001", "SAVEGAME.QCK", "SAVEGAME.002", "savegame.qck", "SAVEGAME.010" };
    uint8_t entries[5][STRIDE];
    memset(entries, 0, sizeof entries);
    for (int i = 0; i < 5; i++) {
        strcpy((char*)entries[i] + FILE_AT, names[i]);
        entries[i][0] = (uint8_t)('a' + i);
    }
    CHECK(qs_filter_entries(&entries[0][0], 5, STRIDE, FILE_AT) == 3);
    CHECK(!strcmp((char*)entries[0] + FILE_AT, "SAVEGAME.001") && entries[0][0] == 'a');
    CHECK(!strcmp((char*)entries[1] + FILE_AT, "SAVEGAME.002") && entries[1][0] == 'c');
    CHECK(!strcmp((char*)entries[2] + FILE_AT, "SAVEGAME.010") && entries[2][0] == 'e');
    CHECK(qs_filter_entries(&entries[0][0], 3, STRIDE, FILE_AT) == 3);
    CHECK(qs_filter_entries(&entries[0][0], 0, STRIDE, FILE_AT) == 0);
}

static ds_press_t press(int modifier, int chooser, int belt, int back) {
    ds_press_t p;
    memset(&p, 0, sizeof p);
    p.enabled = 1;
    p.stow_pressed = 1;
    p.modifier = modifier;
    p.chooser = chooser;
    p.belt = belt;
    p.back = back;
    return p;
}

static void test_dual_stow(void) {
    ds_press_t p = press(1, 0, 0, 1);
    ds_plan_t plan;
    CHECK(ds_needs_chooser(&p));
    plan = ds_plan(&p);
    CHECK(plan.stow_only && !plan.hide_belt);
    p = press(1, 1, 1, 0);
    CHECK(ds_plan(&p).stow_only);
    p = press(1, -1, 1, 1);
    plan = ds_plan(&p);
    CHECK(!plan.stow_only && plan.hide_belt);
    p = press(1, -1, 1, 0);
    plan = ds_plan(&p);
    CHECK(!plan.stow_only && !plan.hide_belt);
    p = press(0, 0, 0, 1);
    plan = ds_plan(&p);
    CHECK(!ds_needs_chooser(&p) && !plan.stow_only && !plan.hide_belt);
    p = press(0, -1, 1, 1);
    CHECK(!ds_plan(&p).hide_belt);
    p = press(1, 0, 0, 1);
    p.phase = 2;
    CHECK(!ds_needs_chooser(&p) && !ds_plan(&p).stow_only);
    p = press(1, 0, 0, 1);
    p.stow_pressed = 0;
    CHECK(!ds_plan(&p).stow_only);
    CHECK(ds_stop_after_stow(1, 3, 4, 1));
    CHECK(!ds_stop_after_stow(1, 3, 4, 0) && !ds_stop_after_stow(0, 3, 4, 1) && !ds_stop_after_stow(1, 4, 4, 1) &&
          !ds_stop_after_stow(1, 3, 3, 1));
    CHECK(ds_pose_preload(3, 1, 0, 0) == 1 && ds_pose_preload(5, 0, 1, 1) == 1 && ds_pose_preload(5, 1, 1, 1) == 0);
    CHECK(ds_pose_preload(5, 0, 0, 1) == -1 && ds_pose_preload(5, 0, 1, 0) == -1 && ds_pose_preload(2, 0, 1, 1) == -1);
    CHECK(ds_pose_preload(3, 2, 0, 0) == -1);
    CHECK(ds_pose_swapped(3, 4) && ds_pose_swapped(3, 0) && !ds_pose_swapped(3, 3) && !ds_pose_swapped(5, 6));
}

static void test_dual_stow_off(void) {
    ds_press_t p = press(1, 0, 0, 1);
    ds_plan_t plan;
    p.enabled = 0;
    plan = ds_plan(&p);
    CHECK(!ds_needs_chooser(&p) && !plan.stow_only && !plan.hide_belt);
    p = press(1, -1, 1, 1);
    p.enabled = 0;
    CHECK(!ds_plan(&p).hide_belt);
}

/* The retail throttle: a pair plays when fMinDelay + fLastTime < now. */
static int plays(float min_delay, float last, float now) { return min_delay + last < now; }

static void test_footsteps(void) {
    float last = 600.0f;
    /* A save from t = 300 loaded after a step at t = 600: silent for five minutes without the fix. */
    CHECK(!plays(0.1f, last, 300.5f));
    CHECK(sound_rearm(&last, 0.1f, 300.0f) && plays(0.1f, last, 300.05f));
    CHECK(!sound_rearm(&last, 0.1f, 300.0f));
    last = 250.0f;
    CHECK(!sound_rearm(&last, 0.1f, 300.0f) && last == 250.0f);
    last = 300.0f;
    CHECK(!sound_rearm(&last, 0.1f, 300.0f) && last == 300.0f);
    last = 10.0f;
    CHECK(sound_rearm(&last, -1.0f, 0.0f) && last == 0.0f);
}

static void test_settings(void) {
    jpt_config_t c;
    jpt_config_defaults(&c);
    CHECK(c.dual_stow && !c.max_object_detail);
    CHECK(c.ambient_occlusion == JPT_AO_MEDIUM);
    CHECK(c.hotkey_vk[JPT_HOTKEY_QUICK_SAVE] == VK_F7_ && c.hotkey_vk[JPT_HOTKEY_QUICK_LOAD] == VK_F8_);
    CHECK(!strcmp(jpt_default_value(JPT_KEY_DUAL_STOW), "1") && !strcmp(jpt_default_value(JPT_KEY_MAX_OBJECT_DETAIL), "0"));
    CHECK(!strcmp(jpt_default_value(JPT_KEY_QUICK_SAVE), "F7") && !strcmp(jpt_default_value(JPT_KEY_QUICK_LOAD), "F8"));
    CHECK(!strcmp(jpt_default_value(JPT_KEY_AMBIENT_OCCLUSION), "medium"));
    CHECK(jpt_config_apply(&c, JPT_KEY_DUAL_STOW, "off") && !c.dual_stow);
    CHECK(jpt_config_apply(&c, JPT_KEY_MAX_OBJECT_DETAIL, "1") && c.max_object_detail);
    CHECK(jpt_config_apply(&c, JPT_KEY_MAX_OBJECT_DETAIL, "0") && !c.max_object_detail);
    CHECK(!jpt_config_apply(&c, JPT_KEY_DUAL_STOW, "maybe") && !c.dual_stow);
    CHECK(jpt_config_apply(&c, JPT_KEY_QUICK_SAVE, "f9") && c.hotkey_vk[JPT_HOTKEY_QUICK_SAVE] == VK_F9_);
    CHECK(jpt_config_apply(&c, JPT_KEY_QUICK_LOAD, "PageDown") && c.hotkey_vk[JPT_HOTKEY_QUICK_LOAD] == 0x22);
    CHECK(!jpt_config_valid(JPT_KEY_QUICK_SAVE, "F5") && !jpt_config_valid(JPT_KEY_QUICK_SAVE, "F10") &&
          !jpt_config_valid(JPT_KEY_QUICK_SAVE, "E") && !jpt_config_valid(JPT_KEY_QUICK_LOAD, "") &&
          !jpt_config_valid(JPT_KEY_QUICK_LOAD, "none"));
    for (int i = 0; i < JPT_HOTKEY_CHOICE_COUNT; i++) {
        uint32_t vk = jpt_hotkey_choices[i].vk;
        CHECK(jpt_hotkey_vk(jpt_hotkey_choices[i].key) == vk);
        CHECK(vk != 0x1B && vk != 0x70 && vk != 0x71 && vk != 0x72 && vk != 0x73 && vk != VK_F5_ && vk != VK_F10_ &&
              vk != 0x7A && vk != 0x7B && !(vk >= '0' && vk <= 'Z') && vk != 0x20);
    }
    /* No toggle: the keys can be moved, never switched off. */
    CHECK(jpt_hotkey_vk("none") == 0 && jpt_hotkey_vk("off") == 0);
    /* Not applicable to the retail executable, or already the renderer's: no setting. */
    CHECK(!jpt_default_value("experimental_ai") && !jpt_default_value("bump_mapping") && !jpt_default_value("quick_save"));
}

static void test_ce9_settings(void) {
    jpt_config_t c;
    jpt_config_defaults(&c);
    CHECK(c.active_ai_enhanced == JPT_ACTIVE_AI_ENHANCED && jpt_active_ai_limits[c.active_ai_enhanced] == 6);
    CHECK(c.water_refraction && c.enhanced_sky);
    CHECK(!strcmp(jpt_default_value(JPT_KEY_ACTIVE_AI), "enhanced") && !strcmp(jpt_default_value(JPT_KEY_WATER_REFRACTION), "1") &&
          !strcmp(jpt_default_value(JPT_KEY_ENHANCED_SKY), "1"));
    CHECK(jpt_config_apply(&c, JPT_KEY_ACTIVE_AI, "original") && jpt_active_ai_limits[c.active_ai_enhanced] == 3);
    CHECK(jpt_config_apply(&c, JPT_KEY_ACTIVE_AI, "Enhanced") && jpt_active_ai_limits[c.active_ai_enhanced] == 6);
    CHECK(!jpt_config_valid(JPT_KEY_ACTIVE_AI, "7") && !jpt_config_valid(JPT_KEY_ACTIVE_AI, "6") &&
          !jpt_config_valid(JPT_KEY_ACTIVE_AI, "max") && !jpt_config_valid(JPT_KEY_ACTIVE_AI, ""));
    CHECK(jpt_config_apply(&c, JPT_KEY_WATER_REFRACTION, "off") && !c.water_refraction);
    CHECK(jpt_config_apply(&c, JPT_KEY_ENHANCED_SKY, "0") && !c.enhanced_sky);
    CHECK(!jpt_config_apply(&c, JPT_KEY_WATER_REFRACTION, "auto") && !c.water_refraction);
    {
        char path[] = "ce9_settings.ini";
        FILE* f = fopen(path, "w");
        CHECK(f != NULL);
        if (f) {
            fputs("active_ai = seven\nwater_refraction = 0\nsettings_version = 3\n", f);
            fclose(f);
            CHECK(jpt_config_load(path, &c));
            CHECK(c.active_ai_enhanced == JPT_ACTIVE_AI_ENHANCED && !c.water_refraction && c.enhanced_sky);
            remove(path);
        }
        CHECK(!jpt_config_load("no_such_ce9_file.ini", &c) && c.active_ai_enhanced == JPT_ACTIVE_AI_ENHANCED &&
              c.water_refraction && c.enhanced_sky);
    }
}

/* F5 changes the running state only; a new run starts from the setting again. */
static void test_live_switches(void) {
    fx_switch_t s;
    for (int setting = 0; setting < 2; setting++) {
        fx_switch_start(&s, setting);
        CHECK(s.running == setting && s.setting == setting);
        CHECK(fx_switch_set(&s, !setting) && s.running == !setting && s.setting == setting);
        CHECK(!fx_switch_set(&s, !setting));
        for (int i = 0; i < 9; i++) CHECK(fx_switch_set(&s, i & 1 ? !setting : setting));
        CHECK(s.setting == setting);
        fx_switch_start(&s, setting);
        CHECK(s.running == setting);
    }
}

static int near_f(float a, float b) { return a - b < 1e-5f && b - a < 1e-5f; }

static void test_sky(void) {
    volatile float zero = 0.0f;
    float nan_value = zero / zero, inf = 1.0f / zero;
    /* CSky's defaults 0.75 / 0.95: clear up to 0.75 of the way to the horizon, full fog from 0.95. */
    CHECK(fx_sky_fog(0.0f, 0.75f, 0.95f) == 0.0f && fx_sky_fog(0.75f, 0.75f, 0.95f) == 0.0f);
    CHECK(near_f(fx_sky_fog(0.85f, 0.75f, 0.95f), 0.5f) && fx_sky_fog(0.95f, 0.75f, 0.95f) == 1.0f &&
          fx_sky_fog(1.0f, 0.75f, 0.95f) == 1.0f);
    /* Equal and inverted bounds: a step at the low bound, never a division. */
    CHECK(fx_sky_fog(0.5f, 0.6f, 0.6f) == 0.0f && fx_sky_fog(0.7f, 0.6f, 0.6f) == 1.0f && fx_sky_fog(0.6f, 0.6f, 0.6f) == 0.0f);
    CHECK(fx_sky_fog(0.75f, 0.9f, 0.5f) == 0.0f && fx_sky_fog(0.95f, 0.9f, 0.5f) == 1.0f);
    for (int i = 0; i <= 100; i++) {
        float f = fx_sky_fog(i / 100.0f, 0.75f, 0.95f);
        CHECK(f >= 0.0f && f <= 1.0f && f == f);
    }
    CHECK(fx_sky_usable(0.75f, 0.95f, 0.02f) && fx_sky_usable(0.6f, 0.6f, -0.01f) && fx_sky_usable(0.9f, 0.5f, 0.0f));
    CHECK(!fx_sky_usable(nan_value, 0.95f, 0.02f) && !fx_sky_usable(0.75f, inf, 0.02f) &&
          !fx_sky_usable(0.75f, 0.95f, nan_value) && !fx_sky_usable(1e30f, 0.95f, 0.02f));
    /* The plane point straight above sees v = 0; far out it nears the horizon (1). */
    CHECK(fx_sky_horizontal(0.0f, 0.0f, 0.02f) == 0.0f && fx_sky_horizontal(0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(near_f(fx_sky_horizontal(0.02f, 0.0f, 0.02f), 0.70710678f) && fx_sky_horizontal(10.0f, 10.0f, 0.02f) > 0.999f);
    CHECK(near_f(fx_sky_horizontal(0.03f, 0.04f, -0.05f), 0.70710678f));
}

/* One water row as water_rows writes it: the texel's wave differences, in the source surface's memory. */
static void write_rows(uintptr_t bits, long pitch, int width, int rows) {
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < width; x++) fx_water_store(bits + (uintptr_t)(y * pitch) + (uintptr_t)x * 2, 0.5, -0.25);
}

/* What the game does with the water: the rows land in the system-memory source surface (CRasterD3D's Lock pointer), and
 * IDirect3DTexture::Load copies that surface into the texture the draws bind. */
static void test_water_identity(void) {
    enum { W = 256, H = 256, PITCH = 512 };
    const uintptr_t source = 0x20000000u, drawn_bits = 0x30000000u, ordinary_source = 0x20100000u, ordinary_bits = 0x30100000u;
    int drawn_surface, source_surface, other_drawn, other_source;
    const fx_water_link_t* link;
    fx_water_reset();
    write_rows(source, PITCH, W, H);
    /* the old lookup asked the drawn texture's own memory, which no row ever touched */
    CHECK(!fx_water_written(drawn_bits, drawn_bits + PITCH * H));
    CHECK(fx_water_written(source, source + PITCH * H));
    CHECK(!fx_water_link(&drawn_surface));
    CHECK(fx_water_loaded(&drawn_surface, &source_surface, source, PITCH, W, H, 2));
    link = fx_water_link(&drawn_surface);
    CHECK(link && link->source == &source_surface && link->bits == source && link->pitch == PITCH && link->width == W &&
          link->height == H);
    /* a normal alpha material: loaded from memory no water row wrote, with the same size and format */
    CHECK(!fx_water_loaded(&other_drawn, &other_source, ordinary_source, PITCH, W, H, 2));
    CHECK(!fx_water_link(&other_drawn) && !fx_water_written(ordinary_bits, ordinary_bits + PITCH * H));
    CHECK(!fx_water_loaded(&other_drawn, &other_source, ordinary_source, PITCH, W, H, 4));
    /* only 16-bit textures hold the water's alpha codes */
    CHECK(!fx_water_loaded(&other_drawn, &source_surface, source, PITCH, W, H, 4) && !fx_water_link(&other_drawn));
    /* a texture that stops being loaded from the water is no longer the water */
    CHECK(!fx_water_loaded(&drawn_surface, &other_source, ordinary_source, PITCH, W, H, 2) && !fx_water_link(&drawn_surface));
    CHECK(fx_water_loaded(&drawn_surface, &source_surface, source, PITCH, W, H, 2) && fx_water_link(&drawn_surface));
    /* two water surfaces, each with its own source */
    {
        int second_drawn, second_source;
        const uintptr_t second = 0x21000000u;
        write_rows(second, 128, 64, 64);
        CHECK(fx_water_loaded(&second_drawn, &second_source, second, 128, 64, 64, 2));
        CHECK(fx_water_link(&second_drawn)->bits == second && fx_water_link(&drawn_surface)->bits == source);
    }
    /* freeing either surface ends the link and the record of its rows */
    fx_water_surface_freed(&source_surface, source, source + PITCH * H);
    CHECK(!fx_water_link(&drawn_surface) && !fx_water_written(source, source + PITCH * H));
    CHECK(!fx_water_loaded(&drawn_surface, &source_surface, source, PITCH, W, H, 2));
    fx_water_reset();
}

/* Surfaces share pages: a neighbour of the water's memory is not water. */
static void test_water_neighbour(void) {
    int drawn_surface, source_surface;
    const uintptr_t page = 0x40000000u, water = page + 0x100, neighbour = page + 0x800;
    fx_water_reset();
    write_rows(water, 64, 32, 8); /* 8 rows of 64 bytes: [page + 0x100, page + 0x300) */
    CHECK(fx_water_written(water, water + 64 * 8));
    CHECK(!fx_water_written(neighbour, neighbour + 0x400) && !fx_water_written(page, page + 0x100));
    CHECK(!fx_water_loaded(&drawn_surface, &source_surface, neighbour, 64, 32, 16, 2) && !fx_water_link(&drawn_surface));
    CHECK(fx_water_loaded(&drawn_surface, &source_surface, water, 64, 32, 8, 2));
    fx_water_reset();
}

static void test_water_shadow(void) {
    static float image[4 * 3 * 2];
    uintptr_t base = 0x10000000u + 0xFFC; /* rows across a page boundary */
    fx_water_stats_t stats;
    fx_water_reset();
    CHECK(fx_water_clamp(0.0) == 0.0f && fx_water_clamp(1.0) == 1.0f && fx_water_clamp(-1.0) == -1.0f &&
          fx_water_clamp(5.0) == 1.0f && fx_water_clamp(-5.0) == -1.0f);
    CHECK(!fx_water_written(base, base + 24) && fx_water_serial() == 0);
    CHECK(!fx_water_fill(base, 8, 4, 3, image, &stats) && image[0] == 0.0f && stats.texels == 0 && stats.max == 0.0f);
    fx_water_store(base + 2, 0.5, -0.25);
    fx_water_store(base + 8 + 4, -1.0, 1.0);
    CHECK(fx_water_written(base, base + 24) && fx_water_serial() == 2);
    fx_water_store(base + 2, 0.25, 0.0);
    CHECK(fx_water_serial() == 3);
    CHECK(fx_water_fill(base, 8, 4, 3, image, &stats));
    CHECK(image[2] == 0.25f && image[3] == 0.0f);
    CHECK(image[4 * 2 + 4] == -1.0f && image[4 * 2 + 5] == 1.0f);
    CHECK(image[0] == 0.0f && image[1] == 0.0f && image[23] == 0.0f);
    CHECK(stats.texels == 2 && stats.max == 1.0f && stats.mean > 0.5f && stats.mean < 0.7f);
    CHECK(!fx_water_written(0x20000000u, 0x20001000u));
    for (unsigned i = 0; i < FX_WATER_PAGES + 8; i++) fx_water_store(0x30000000u + i * 4096u, 0.1, 0.1);
    CHECK(fx_water_written(0x30000000u + (FX_WATER_PAGES + 7) * 4096u, 0x30000000u + (FX_WATER_PAGES + 7) * 4096u + 2));
    fx_water_reset();
    CHECK(!fx_water_written(base, base + 24) && fx_water_serial() == 0);
}

/* The values the water really produces are small: they reach the shader as they are, to float precision. */
static void test_water_precision(void) {
    static const double values[] = { 0.00001, 0.0001, 0.001, 0.01, 0.1, -0.00001, -0.1 };
    static float image[7 * 2];
    const uintptr_t base = 0x50000000u;
    fx_water_stats_t stats;
    fx_water_reset();
    for (int i = 0; i < 7; i++) fx_water_store(base + (uintptr_t)i * 2, values[i], -values[i] * 2.0);
    CHECK(fx_water_fill(base, 14, 7, 1, image, &stats));
    for (int i = 0; i < 7; i++) {
        CHECK(image[i * 2] == (float)values[i] && image[i * 2 + 1] == (float)(-values[i] * 2.0));
        CHECK(image[i * 2] != 0.0f);
    }
    CHECK(stats.max > 0.19f && stats.max < 0.21f && stats.mean > 0.0f);
    {
        const fx_water_raw_t* raw = fx_water_raw();
        CHECK(raw->written == 7 && raw->nonzero == 7 && raw->a_min == -0.1 && raw->a_max == 0.1);
        CHECK(raw->b_min == -0.2 && raw->b_max == 0.2 && fx_water_abs_max() == 0.2);
    }
    fx_water_store(base, 3.0, 0.0);
    CHECK(fx_water_raw()->a_max == 3.0 && fx_water_abs_max() == 3.0);
    fx_water_reset();
    CHECK(fx_water_abs_max() == 0.0 && fx_water_raw()->written == 0);
    fx_water_store(base, 0.0, 0.0);
    CHECK(fx_water_raw()->written == 1 && fx_water_raw()->nonzero == 0);
    fx_water_reset();
}

/* rhw is 0.1 / metres: CE's 1 / max(metres, 1) is 1 within a metre and 1/z past it. */
static void test_water_attenuation(void) {
    CHECK(fabs(fx_water_attenuation(0.1) - 1.0) < 1e-6 && fx_water_attenuation(0.5) == 1.0 && fx_water_attenuation(10.0) == 1.0);
    CHECK(fabs(fx_water_attenuation(0.01) - 0.1) < 1e-6 && fabs(fx_water_attenuation(0.0155) - 0.155) < 1e-6);
    CHECK(fabs(fx_water_attenuation(0.1 / 20.0) - 1.0 / 20.0) < 1e-6);
    CHECK(fx_water_attenuation(0.0) == 0.0 && fx_water_attenuation(-1.0) == 0.0);
}

/* The shift in pixels is a fraction of the render height, so it grows with the resolution and never vanishes. */
static void test_water_displacement(void) {
    static const int sizes[][2] = { { 640, 480 }, { 1920, 1080 }, { 2560, 1920 }, { 3840, 2160 } };
    const double rhw = 0.0155; /* the nearest Beach water in log 119: 6.5 m */
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        double h = sizes[i][1], px = fx_water_displacement_px(0.17, FX_WATER_STRENGTH, rhw, h);
        CHECK(fabs(px - 0.17 * 0.35 * 0.155 * h) < 1e-4 && px >= 0.17 * 0.35 * 0.155 * 480.0 - 1e-4);
    }
    CHECK(fabs(fx_water_displacement_px(0.17, FX_WATER_STRENGTH, rhw, 1920.0) - 17.71) < 0.01);
    CHECK(fx_water_displacement_px(0.02, 0.35, 0.05, 1920.0) / fx_water_displacement_px(0.02, 0.35, 0.05, 480.0) == 4.0);
    CHECK(fx_water_displacement_px(0.02, 0.35, 5.0, 1000.0) == 0.02 * 0.35 * 1000.0);
    CHECK(fx_water_displacement_px(0.02, 0.35, -1.0, 1000.0) == 0.0 && fx_water_displacement_px(0.0, 0.35, 1.0, 1000.0) == 0.0);
    /* calm water far away stays under a pixel at any resolution here */
    CHECK(fx_water_displacement_px(0.004, 0.35, 0.003, 2160.0) < 1.0);
}

/* The expected shift for a wave difference and a distance, at three render sizes. */
static void test_water_shift_table(void) {
    static const double waves[] = { 0.01, 0.05, 0.1, 0.25, 0.5, 0.75 };
    static const double metres[] = { 0.5, 1.0, 2.0, 5.0, 10.0, 20.0 };
    static const int sizes[][2] = { { 1920, 1080 }, { 2560, 1920 }, { 3840, 2160 } };
    printf("water shift in pixels (wave difference x distance), strength 0.35\n");
    for (size_t z = 0; z < 3; z++) {
        printf("  %dx%d      0.5 m     1 m     2 m     5 m    10 m    20 m\n", sizes[z][0], sizes[z][1]);
        for (size_t w = 0; w < 6; w++) {
            printf("    %.2f", waves[w]);
            for (size_t m = 0; m < 6; m++) {
                double rhw = FX_WATER_RHW_METRE / metres[m], dx, dy, px;
                fx_water_offset_px(waves[w], 0.0, FX_WATER_STRENGTH, rhw, sizes[z][1], &dx, &dy);
                px = dx;
                printf(" %7.1f", px);
                CHECK(dy == 0.0 && fabs(px - waves[w] * (double)FX_WATER_STRENGTH * (metres[m] < 1.0 ? 1.0 : 1.0 / metres[m]) * sizes[z][1]) < 1e-6);
            }
            printf("\n");
        }
    }
}

/* Nearer than a metre the fade stops at 1 (CE's 1 / max(depth, 1)); at zero distance and beyond nothing blows up. */
static void test_water_near_camera(void) {
    double dx, dy;
    CHECK(fx_water_attenuation(0.2) == 1.0 && fx_water_attenuation(0.99) == 1.0 && fx_water_attenuation(1e30) == 1.0);
    CHECK(fx_water_attenuation(0.0) == 0.0 && fx_water_attenuation(-0.5) == 0.0 && fx_water_attenuation(1e-30) < 1e-20);
    fx_water_offset_px(0.7449, 0.0, FX_WATER_STRENGTH, 1e30, 1920.0, &dx, &dy);
    CHECK(fabs(dx - 0.7449 * (double)FX_WATER_STRENGTH * 1920.0) < 1e-6 && dy == 0.0); /* the largest possible: the wave factor itself is capped at 1 */
    CHECK(fabs(dx - 500.6) < 0.1);
    CHECK(fx_water_clamp(10.0) == 1.0f && fx_water_clamp(-10.0) == -1.0f);
    fx_water_offset_px(1.0, 1.0, FX_WATER_STRENGTH, 1.0, 2160.0, &dx, &dy);
    CHECK(dx == dy && fabs(dx - (double)FX_WATER_STRENGTH * 2160.0) < 1e-6); /* never more than 0.35 of the target height at any distance */
}

/* x and y both scale by the height: the shader's UV shift (x times height / width) times the width is the same. */
static void test_water_offset_axes(void) {
    static const int sizes[][2] = { { 640, 480 }, { 2560, 1920 }, { 3840, 2160 } };
    for (size_t i = 0; i < 3; i++) {
        double w = sizes[i][0], h = sizes[i][1], dx, dy, uv_x, uv_y;
        fx_water_offset_px(0.3, -0.2, FX_WATER_STRENGTH, 0.05, h, &dx, &dy);
        uv_x = 0.3 * (double)FX_WATER_STRENGTH * fx_water_attenuation(0.05) * (h / w);
        uv_y = -0.2 * (double)FX_WATER_STRENGTH * fx_water_attenuation(0.05);
        CHECK(fabs(uv_x * w - dx) < 1e-9 && fabs(uv_y * h - dy) < 1e-9 && dx > 0.0 && dy < 0.0);
    }
}

static void test_water_shift_stats(void) {
    fx_shift_t* sh = fx_water_shifts();
    float values[4 * 4 * 2];
    double out[2];
    fx_water_reset();
    CHECK(fx_shift_mean(sh) == 0.0 && fx_shift_percentile(sh, 0.99) == 0.0 && sh->max == 0.0);
    for (int i = 0; i < 99; i++) fx_shift_add(sh, 2.0, 1.0, 0.1, 0.02);
    fx_shift_add(sh, 493.9, 1.0, 0.7449, 0.0987);
    CHECK(fabs(fx_shift_mean(sh) - (99 * 2.0 + 493.9) / 100.0) < 1e-9);
    CHECK(fx_shift_percentile(sh, 0.50) <= 2.25 && fx_shift_percentile(sh, 0.99) <= 2.25); /* one outlier does not move p99 */
    CHECK(fx_shift_percentile(sh, 1.0) > 493.0 && sh->max == 493.9 && sh->max_wave == 0.7449 && sh->max_rhw == 0.0987);
    fx_shift_add(sh, 5000.0, 1.0, 1.0, 1.0);
    CHECK(fx_shift_percentile(sh, 1.0) <= FX_SHIFT_BINS * FX_SHIFT_BIN_PX && sh->max == 5000.0);
    fx_shift_add(sh, -1.0, 1.0, 0, 0);
    fx_shift_add(sh, 1.0, 0.0, 0, 0);
    CHECK(sh->weight == 101.0);
    for (int i = 0; i < 16; i++) values[i * 2] = (float)i, values[i * 2 + 1] = -(float)i;
    fx_water_wave_at(values, 4, 4, 0.125, 0.125, out); /* the centre of texel (0, 0) */
    CHECK(out[0] == 0.0 && out[1] == 0.0);
    fx_water_wave_at(values, 4, 4, 0.375, 0.125, out); /* the centre of texel (1, 0) */
    CHECK(out[0] == 1.0 && out[1] == -1.0);
    fx_water_wave_at(values, 4, 4, 0.25, 0.125, out); /* halfway between them */
    CHECK(fabs(out[0] - 0.5) < 1e-9);
    fx_water_wave_at(values, 4, 4, 5.0, 5.0, out); /* clamped to the last texel */
    CHECK(out[0] == 15.0);
    fx_water_reset();
}

/* The retail startup disk check wraps in 32 bits on a large volume; the port never blocks on it. */
/* The loader's check (0x004B069A) multiplies in 32 bits: a drive with 721599037440 free bytes wrapped to 42 MB and the
 * level was refused. */
static void test_disk_space_report(void) {
    uint32_t g[4];
    const uint64_t drives[] = { 0, 50ull << 20, 3ull << 30, 4ull << 30, 721599037440ull, 4ull << 40, ~0ull };
    for (unsigned i = 0; i < sizeof drives / sizeof drives[0]; i++) {
        uint32_t product;
        legacy_disk_geometry(drives[i], drives[i] * 2 > drives[i] ? drives[i] * 2 : drives[i], g);
        product = g[0] * g[1] * g[2];
        CHECK(g[0] == 8 && g[1] == 512 && g[3] >= g[2]);
        CHECK(product <= 0x7FFFF000u);
        CHECK((uint64_t)product == (drives[i] / 4096 < 0x7FFFFu ? drives[i] / 4096 * 4096 : 0x7FFFF000ull));
    }
    legacy_disk_geometry(721599037440ull, 2ull << 40, g);
    CHECK(g[0] * g[1] * g[2] > 100u << 20);
    CHECK(legacy_free_bytes(8, 512, 176171640u, 0) < 100u << 20);
}

static void test_legacy_disk_check(void) {
    uint32_t spc = 8, bps = 512, clusters = 100u * 1048576u + 12800u; /* about 429 GB free */
    uint32_t wrapped = legacy_free_bytes(spc, bps, clusters, 0);
    CHECK(wrapped == 12800u * 4096u && wrapped < 110u << 20);
    CHECK(!legacy_disk_check_passes(wrapped, 110) && !legacy_disk_check_passes(wrapped, 100));
    CHECK(startup_disk_check_passes() == 1);
    CHECK(legacy_disk_check_passes(legacy_free_bytes(8, 512, 1000000, 0), 110)); /* 4 GB: the check itself is fine */
    CHECK(legacy_free_bytes(8, 512, 4096u * 1048576u / 4096u / 1u, 4096) == 4096u); /* exactly 4 GiB wraps to the swap size */
    CHECK(!legacy_disk_check_passes(0, 110) && legacy_disk_check_passes(110u << 20, 110) && !legacy_disk_check_passes((110u << 20) - 1, 110));
    CHECK(startup_disk_check_passes() == 1 && startup_disk_check_passes() == 1);
}

/* The sampling area and the shortened offset: ps_water's edge rule, on the CPU. */
static void test_water_edges(void) {
    float b[4], out[2], base[2], off[2];
    fx_water_scene_bounds(0, 0, 640, 480, 640, 480, b);
    CHECK(b[0] == 0.5f / 640 && b[1] == 0.5f / 480 && b[2] == 639.5f / 640 && b[3] == 479.5f / 480);
    fx_water_scene_bounds(100, 50, 540, 430, 640, 480, b);
    CHECK(b[0] == 100.5f / 640 && b[1] == 50.5f / 480 && b[2] == 539.5f / 640 && b[3] == 429.5f / 480);
    fx_water_scene_bounds(-5, -5, 9000, 9000, 640, 480, b);
    CHECK(b[0] == 0.5f / 640 && b[2] == 639.5f / 640 && b[3] == 479.5f / 480);
    fx_water_scene_bounds(10, 10, 10, 400, 640, 480, b);
    CHECK(b[0] == 0.5f / 640 && b[2] == 639.5f / 640 && b[1] == 0.5f / 480); /* empty: the whole copy */
    fx_water_scene_bounds(0, 0, 640, 480, 640, 480, b);
    base[0] = 0.5f, base[1] = 0.5f;
    off[0] = 0.1f, off[1] = 0.0f;
    fx_water_refract_uv(base, off, b, out);
    CHECK(fabsf(out[0] - 0.6f) < 1e-6f && out[1] == 0.5f); /* inside: untouched */
    off[0] = 0.9f;
    fx_water_refract_uv(base, off, b, out);
    CHECK(out[0] <= b[2] + 1e-6f && out[0] > 0.99f && out[1] == 0.5f); /* leaves on the right: stops at the last texel centre */
    base[0] = b[2], off[0] = 0.2f;
    fx_water_refract_uv(base, off, b, out);
    CHECK(out[0] == base[0] && out[1] == base[1]); /* already at the edge: the base uv, no smear */
    base[0] = b[0], base[1] = b[1], off[0] = -0.3f, off[1] = -0.3f;
    fx_water_refract_uv(base, off, b, out);
    CHECK(out[0] == b[0] && out[1] == b[1]); /* corner */
    base[0] = 0.5f, base[1] = 0.9f, off[0] = 0.1f, off[1] = 0.3f;
    fx_water_refract_uv(base, off, b, out);
    CHECK(out[1] <= b[3] + 1e-6f && fabsf((out[0] - 0.5f) / 0.1f - (out[1] - 0.9f) / 0.3f) < 1e-4f); /* kept on its own direction */
    off[0] = off[1] = 0.0f;
    fx_water_refract_uv(base, off, b, out);
    CHECK(out[0] == base[0] && out[1] == base[1]);
}

/* The alpha the water texture's own texels carry (the palette's 0.25 to 1.0), only where rows wrote. */
static void test_water_alpha(void) {
    static uint16_t texels[4] = { 0x4123, 0xF456, 0x8789, 0x0000 };
    uintptr_t base = (uintptr_t)texels;
    int lo, hi;
    fx_water_reset();
    CHECK(!fx_water_alpha_range(base, 8, 4, 1, &lo, &hi) && lo == 0 && hi == 0);
    fx_water_store(base, 0.0, 0.0);
    fx_water_store(base + 2, 0.0, 0.0);
    fx_water_store(base + 4, 0.0, 0.0);
    CHECK(fx_water_alpha_range(base, 8, 4, 1, &lo, &hi) && lo == 4 && hi == 15);
    fx_water_reset();
}

static void test_water_plan(void) {
    fx_water_plan_t p = fx_water_plan(0, 1, 1, 10, 0, 0, 4);
    CHECK(!p.refract && !p.copy_scene && !p.resolve); /* Off: no pass, no copy */
    p = fx_water_plan(1, 0, 1, 10, 0, 0, 4);
    CHECK(!p.refract && !p.copy_scene); /* resources failed: the water as before */
    p = fx_water_plan(1, 1, 0, 10, 0, 0, 1);
    CHECK(!p.refract); /* not drawn alpha-over: as before */
    p = fx_water_plan(1, 1, 1, 10, 0, 0, 1);
    CHECK(p.refract && p.copy_scene && !p.resolve); /* first water of the frame without MSAA: a copy */
    for (unsigned samples = 2; samples <= 8; samples *= 2) {
        p = fx_water_plan(1, 1, 1, 10, 9, 1, samples);
        CHECK(p.refract && p.copy_scene && p.resolve); /* MSAA: a resolve */
    }
    p = fx_water_plan(1, 1, 1, 10, 10, 1, 4);
    CHECK(p.refract && !p.copy_scene && !p.resolve); /* the frame's copy is reused */
    p = fx_water_plan(1, 1, 1, 10, 10, 0, 4);
    CHECK(p.copy_scene && p.resolve); /* a recreated copy (resize, new device) is filled again */
    CHECK(fx_scene_copy_fits(2560, 1440, 87, 2560, 1440, 87) && !fx_scene_copy_fits(2560, 1440, 87, 1920, 1080, 87) &&
          !fx_scene_copy_fits(0, 0, 87, 0, 0, 87));
}

static void test_migration_and_reset(void) {
    char path[] = "ce_tests_settings.ini";
    FILE* f = fopen(path, "w");
    jpt_config_t c;
    const char* keys[] = { JPT_KEY_DUAL_STOW };
    const char* off[] = { "0" };
    CHECK(f != NULL);
    if (!f) return;
    fputs("# an earlier file\nrenderer = software\nambient_occlusion = high\nsettings_version = 3\n", f);
    fclose(f);
    CHECK(jpt_config_migrate(path) == JPT_MIGRATED_NOTHING);
    CHECK(jpt_config_load(path, &c));
    CHECK(!c.hardware && c.ambient_occlusion == JPT_AO_HIGH);
    CHECK(c.dual_stow && !c.max_object_detail && c.hotkey_vk[JPT_HOTKEY_QUICK_SAVE] == VK_F7_ &&
          c.hotkey_vk[JPT_HOTKEY_QUICK_LOAD] == VK_F8_);
    CHECK(jpt_ini_set(path, keys, off, 1));
    CHECK(jpt_config_load(path, &c) && !c.dual_stow && !c.hardware && c.ambient_occlusion == JPT_AO_HIGH);
    remove(path);
}

static int load_max_object_detail(const char* text, int* value) {
    char path[] = "ce_tests_detail.ini";
    FILE* f = fopen(path, "w");
    jpt_config_t c;
    int loaded;
    CHECK(f != NULL);
    if (!f) return 0;
    fputs(text, f);
    fclose(f);
    CHECK(jpt_config_migrate(path) != JPT_MIGRATED_EXPERIMENTAL);
    jpt_config_defaults(&c);
    loaded = jpt_config_load(path, &c);
    *value = c.max_object_detail;
    remove(path);
    return loaded;
}

/* A new config starts with the game's own LOD; a saved choice, from any settings version, is kept. */
static void test_max_object_detail(void) {
    int value = -1;
    CHECK(load_max_object_detail("renderer = d3d11\nsettings_version = 3\n", &value) && value == 0);
    CHECK(load_max_object_detail("max_object_detail = 1\nsettings_version = 3\n", &value) && value == 1);
    CHECK(load_max_object_detail("max_object_detail = off\nsettings_version = 3\n", &value) && value == 0);
    CHECK(load_max_object_detail("max_object_detail = on\nsettings_version = 2\nmsaa = 1\n", &value) && value == 1);
    CHECK(load_max_object_detail("max_object_detail = 1\nsettings_version = 1\nvsync = 0\n", &value) && value == 1);
    /* Off leaves the game's bDetailReduce as its own LOD set it; On clears it. */
    CHECK(ce_detail_reduce(0, 0) == 0 && ce_detail_reduce(0, 1) == 1 && ce_detail_reduce(0, 0xFF) == 0xFF);
    CHECK(ce_detail_reduce(1, 0) == 0 && ce_detail_reduce(1, 1) == 0 && ce_detail_reduce(1, 0xFF) == 0);
}

static char* read_source(const char* relative) {
    char path[512];
    FILE* f;
    long n;
    char* text;
    size_t at = 0;
    snprintf(path, sizeof path, "%s/%s", JPT_SOURCE_DIR, relative);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)malloc((size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(text);
        return NULL;
    }
    fclose(f);
    for (long i = 0; i < n; i++)
        if (text[i] != '\r') text[at++] = text[i];
    text[at] = 0;
    return text;
}

/* Quick Save pauses and resumes the game from the UI loop's GetCursorPos: the state that call returns the cursor for
 * must be the one after the hooks, and taking the pointer back must set the virtual cursor before the state changes. */
static void test_quick_save_input_order(void) {
    char* source = read_source("src/host/window_host.c");
    const char *shim, *hooks, *refresh, *read, *update, *recapture, *change;
    CHECK(source != NULL);
    if (!source) return;
    shim = strstr(source, "void shim_GetCursorPos(void)");
    hooks = shim ? strstr(shim, "ce_gameplay_ui_loop();") : NULL;
    refresh = hooks ? strstr(hooks, "update_input(\"GetCursorPos\");") : NULL;
    read = refresh ? strstr(refresh, "*out = game_cursor();") : NULL;
    CHECK(shim && hooks && refresh && read && refresh < read && refresh - hooks < 60);
    update = strstr(source, "static void update_input(const char* why)");
    recapture = update ? strstr(update, "pointer_recapture(s.state, next,") : NULL;
    change = update ? strstr(update, "s.state = next;") : NULL;
    CHECK(update && recapture && change && recapture < change);
    free(source);
}

static void test_keys(void) {
    key_policy_set_quick_keys(VK_F7_, VK_F8_);
    CHECK(key_policy(WM_KEYDOWN_, VK_F7_, 0, 0, 0) == KEY_HOST_QUICK_SAVE);
    CHECK(key_policy(WM_KEYDOWN_, VK_F8_, 0, 0, 0) == KEY_HOST_QUICK_LOAD);
    CHECK(key_policy(WM_KEYDOWN_, VK_F7_, 1, 0, 0) == KEY_DROP && key_policy(WM_KEYUP_, VK_F7_, 0, 0, 0) == KEY_DROP);
    CHECK(key_policy(WM_SYSKEYDOWN_, VK_F8_, 0, 0, 1) == KEY_DROP);
    CHECK(key_policy(WM_KEYDOWN_, VK_F5_, 0, 0, 0) == KEY_HOST_MINIMAP);
    CHECK(key_policy(WM_KEYDOWN_, VK_F10_, 0, 0, 0) == KEY_HOST_CHEATS);
    CHECK(key_policy(WM_KEYDOWN_, VK_F9_, 0, 0, 0) == KEY_HOST_SNAPSHOT);
    key_policy_set_quick_keys(VK_F9_, VK_F7_);
    CHECK(key_policy(WM_KEYDOWN_, VK_F9_, 0, 0, 0) == KEY_HOST_QUICK_SAVE);
    CHECK(key_policy(WM_KEYDOWN_, VK_F7_, 0, 0, 0) == KEY_HOST_QUICK_LOAD);
    CHECK(key_policy(WM_KEYDOWN_, VK_F8_, 0, 0, 0) == KEY_TO_GAME);
    key_policy_set_quick_keys(VK_F7_, VK_F7_);
    CHECK(key_policy(WM_KEYDOWN_, VK_F7_, 0, 0, 0) == KEY_HOST_QUICK_SAVE);
    key_policy_set_quick_keys(VK_F7_, VK_F8_);
}

static int table_has(const char* code, const char* line) {
    char path[512], buf[1024];
    int found = 0;
    FILE* f;
    snprintf(path, sizeof path, "%s/assets/translations/%s.interface.txt", JPT_SOURCE_DIR, code);
    f = fopen(path, "rb");
    if (!f) return 0;
    while (!found && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        found = !strcmp(buf, line);
    }
    fclose(f);
    return found;
}

static void test_interface_text(void) {
    static const char* const codes[] = { "en", "pt-BR", "fr", "de", "es", "it", "ru" };
    static const char* const keys[] = { "Quick Save", "Quick saved", "Quick Save failed", "Quick Load failed",
                                        "No Quick Save yet", "Quick Save works in a level",
                                        "Quick Save is not available now", "Quick Load is not available now" };
    CHECK(table_has("pt-BR", "Quick Save\tSalvamento r\xC3\xA1pido"));
    CHECK(table_has("pt-BR", "Quick saved\tSalvamento r\xC3\xA1pido feito"));
    CHECK(table_has("pt-BR", "No Quick Save yet\tAinda n\xC3\xA3o h\xC3\xA1 salvamento r\xC3\xA1pido"));
    for (size_t l = 0; l < sizeof codes / sizeof codes[0]; l++)
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            char path[512], buf[1024];
            size_t n = strlen(keys[k]);
            int found = 0;
            FILE* f;
            snprintf(path, sizeof path, "%s/assets/translations/%s.interface.txt", JPT_SOURCE_DIR, codes[l]);
            f = fopen(path, "rb");
            CHECK(f != NULL);
            if (!f) continue;
            while (!found && fgets(buf, sizeof buf, f)) found = !strncmp(buf, keys[k], n) && buf[n] == '\t' && (unsigned char)buf[n + 1] > ' ';
            fclose(f);
            CHECK(found);
        }
}

int main(void) {
    test_profile_activate();
    test_quick_save();
    test_quick_load();
    test_missing_quick_save();
    test_files();
    test_save_list();
    test_dual_stow();
    test_dual_stow_off();
    test_footsteps();
    test_settings();
    test_ce9_settings();
    test_live_switches();
    test_sky();
    test_water_shadow();
    test_water_precision();
    test_water_attenuation();
    test_water_displacement();
    test_water_shift_table();
    test_water_near_camera();
    test_water_offset_axes();
    test_water_shift_stats();
    test_water_alpha();
    test_water_edges();
    test_disk_space_report();
    test_legacy_disk_check();
    test_water_identity();
    test_water_neighbour();
    test_water_plan();
    test_migration_and_reset();
    test_max_object_detail();
    test_keys();
    test_quick_save_input_order();
    test_interface_text();
    printf("ce_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}

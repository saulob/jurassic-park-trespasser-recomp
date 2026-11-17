/*
 * Field of view and mouse sensitivity: the settings, the camera's arithmetic for every aspect ratio, the scaling of the
 * mouse's counts, the row under the game's Controls window, persistence, and where each one may be written.
 * Headless: ninja -C <build dir> fov_mouse_tests && fov_mouse_tests.exe
 */
#include <windows.h>
#include "test_profile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fov_policy.h"
#include "host_policy.h"
#include "jpt_config.h"
#include "jpt_display.h"

#define PI 3.14159265358979323846

static int g_checks, g_failures;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_failures++;                                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static double deg(double radians) { return radians * 180.0 / PI; }
static double rad(double degrees) { return degrees * PI / 180.0; }
static int close_to(double a, double b, double eps) { return fabs(a - b) <= eps; }

static char* read_source(const char* relative) {
    char path[MAX_PATH];
    FILE* f;
    long n;
    char* text;
    snprintf(path, sizeof path, "%s/%s", JPT_SOURCE_DIR, relative);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)calloc((size_t)n + 1, 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) text[0] = 0;
    fclose(f);
    return text;
}

static void temp_file(char* path, size_t cap, const char* name) {
    char dir[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    snprintf(path, cap, "%sjpt_fov_mouse_%lu_%s", dir, GetCurrentProcessId(), name);
}

static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* ---- settings ---- */

static void test_settings(void) {
    jpt_config_t c;
    char text[16];
    jpt_config_defaults(&c);
    CHECK(c.fov == 75 && JPT_FOV_DEFAULT == 75 && !strcmp(jpt_default_value(JPT_KEY_FOV), "75"), "FOV default 75");
    CHECK(c.mouse_sensitivity == 1.0f && !strcmp(jpt_default_value(JPT_KEY_MOUSE_SENSITIVITY), "1.00"),
          "mouse sensitivity default 1.00");
    CHECK(jpt_config_apply(&c, JPT_KEY_FOV, "90") && c.fov == 90, "fov = 90");
    CHECK(jpt_config_apply(&c, JPT_KEY_FOV, "75.0") && c.fov == 75, "CE's 75.0 reads as 75");
    CHECK(jpt_config_apply(&c, JPT_KEY_FOV, "89.6") && c.fov == 90, "rounded to a whole degree");
    CHECK(jpt_config_apply(&c, JPT_KEY_FOV, "175") && c.fov == JPT_FOV_MAX, "175 (CE's maximum) clamps to %d", JPT_FOV_MAX);
    CHECK(jpt_config_apply(&c, JPT_KEY_FOV, "30") && c.fov == JPT_FOV_MIN, "30 (CE's minimum) clamps to %d", JPT_FOV_MIN);
    CHECK(jpt_config_apply(&c, JPT_KEY_FOV, "-1e300") && c.fov == JPT_FOV_MIN, "huge values clamp");
    c.fov = 80;
    CHECK(!jpt_config_apply(&c, JPT_KEY_FOV, "wide") && !jpt_config_apply(&c, JPT_KEY_FOV, "") &&
              !jpt_config_apply(&c, JPT_KEY_FOV, "nan") && !jpt_config_apply(&c, JPT_KEY_FOV, "90x") && c.fov == 80,
          "text that is not a number is refused and the value kept");
    CHECK(jpt_fov_clamp(0) == 60 && jpt_fov_clamp(200) == 110 && jpt_fov_clamp(75) == 75, "jpt_fov_clamp");

    CHECK(jpt_config_apply(&c, JPT_KEY_MOUSE_SENSITIVITY, "2.5") && c.mouse_sensitivity == 2.5f, "sensitivity 2.5");
    CHECK(jpt_config_apply(&c, JPT_KEY_MOUSE_SENSITIVITY, "0.1") && c.mouse_sensitivity == JPT_MOUSE_SENSITIVITY_MIN,
          "0.1 (CE's minimum) clamps to 0.25");
    CHECK(jpt_config_apply(&c, JPT_KEY_MOUSE_SENSITIVITY, "10") && c.mouse_sensitivity == JPT_MOUSE_SENSITIVITY_MAX,
          "10 (CE's maximum) clamps to 4");
    CHECK(jpt_config_apply(&c, JPT_KEY_MOUSE_SENSITIVITY, "1.234") && close_to(c.mouse_sensitivity, 1.25, 1e-6),
          "snapped to the 0.05 step");
    c.mouse_sensitivity = 1.5f;
    CHECK(!jpt_config_apply(&c, JPT_KEY_MOUSE_SENSITIVITY, "fast") && !jpt_config_apply(&c, JPT_KEY_MOUSE_SENSITIVITY, "inf") &&
              c.mouse_sensitivity == 1.5f,
          "a sensitivity that is not a number is refused");
    CHECK(jpt_mouse_sensitivity_clamp(NAN) == 1.0f && jpt_mouse_sensitivity_clamp(-3.0f) == 0.25f, "clamp edge cases");
    jpt_mouse_sensitivity_text(1.0f, text, sizeof text);
    CHECK(!strcmp(text, "1.00"), "1.00");
    jpt_mouse_sensitivity_text(0.249f, text, sizeof text);
    CHECK(!strcmp(text, "0.25"), "0.25");
    jpt_mouse_sensitivity_text(3.977f, text, sizeof text);
    CHECK(!strcmp(text, "4.00"), "4.00");
    for (float v = 0.0f; v < 5.0f; v += 0.013f) {
        float k = jpt_mouse_sensitivity_clamp(v);
        CHECK(k >= JPT_MOUSE_SENSITIVITY_MIN && k <= JPT_MOUSE_SENSITIVITY_MAX &&
                  close_to(k / JPT_MOUSE_SENSITIVITY_STEP, floor(k / JPT_MOUSE_SENSITIVITY_STEP + 0.5), 1e-3),
              "%.3f -> %.3f on the range and the step", v, k);
    }
    /* X and Y are one setting */
    for (int i = 0; i < JPT_SETTING_COUNT; i++)
        CHECK(!strstr(jpt_settings[i].key, "mouse_sensitivity_") && !strstr(jpt_settings[i].key, "sens_x") &&
                  !strstr(jpt_settings[i].key, "safe"),
              "no per-axis or Safe Mode setting: %s", jpt_settings[i].key);
}

/* Older files have neither key: the defaults apply, and nothing else in them changes. */
static void test_migration(void) {
    char path[MAX_PATH], value[64];
    jpt_config_t c;
    temp_file(path, sizeof path, "old.ini");
    write_text(path, "settings_version = 3\nrenderer = d3d11\nwater_refraction = 0\nactive_ai = original\nmsaa = 8\n");
    CHECK(jpt_config_load(path, &c) && c.fov == 75 && c.mouse_sensitivity == 1.0f, "an old file reads FOV 75, mouse 1.00");
    CHECK(!c.water_refraction && c.active_ai_enhanced == 0 && c.msaa == 8, "its other settings are kept");
    CHECK(jpt_config_migrate(path) == JPT_MIGRATED_NOTHING, "version 3 needs no migration");
    CHECK(!jpt_ini_get(path, JPT_KEY_FOV, value, sizeof value) && !jpt_ini_get(path, JPT_KEY_MOUSE_SENSITIVITY, value, sizeof value),
          "nothing is written to it before a change");
    DeleteFileA(path);
}

/* Launcher and game share one file and one key; the game's Controls row writes the same line the launcher reads. */
static void test_persistence(void) {
    char path[MAX_PATH], value[64];
    const char* keys[2] = { JPT_KEY_FOV, JPT_KEY_MOUSE_SENSITIVITY };
    const char* values[2] = { "92", "1.75" };
    const char* key = JPT_KEY_MOUSE_SENSITIVITY;
    const char* text = "0.60";
    jpt_config_t c;
    temp_file(path, sizeof path, "shared.ini");
    write_text(path, "# launcher\nlanguage = pt-BR\ninvert = untouched\n");
    CHECK(jpt_ini_set(path, keys, values, 2), "the launcher saves both");
    CHECK(jpt_config_load(path, &c) && c.fov == 92 && close_to(c.mouse_sensitivity, 1.75, 1e-6), "the next start reads them");
    CHECK(jpt_ini_set(path, &key, &text, 1), "a save of the sensitivity alone");
    CHECK(jpt_config_load(path, &c) && close_to(c.mouse_sensitivity, 0.6, 1e-6) && c.fov == 92 && !strcmp(c.language, "pt-BR"),
          "the launcher sees the game's change; FOV and language are untouched");
    CHECK(jpt_ini_get(path, "invert", value, sizeof value) && !strcmp(value, "untouched"), "unknown lines are kept");
    DeleteFileA(path);
}

/* ---- field of view ---- */

static void test_fov_math(void) {
    const double game = tan(rad(37.5));
    float game_f = (float)game;
    CHECK(fov_tan_half(game_f, 75) == game_f, "75 leaves the game's tangent bit for bit");
    CHECK(close_to(fov_tan_half(game, 90), 1.0, 1e-12), "90: tan 45");
    CHECK(close_to(fov_tan_half(game, 60), tan(rad(30.0)), 1e-12), "60: tan 30");
    CHECK(close_to(fov_tan_half(game, 110), tan(rad(55.0)), 1e-12), "110: tan 55");
    /* a level's own angle (CSetRendererAction) keeps its ratio to 75, as CE scales it */
    CHECK(close_to(fov_tan_half(tan(rad(30.0)), 90), tan(rad(36.0)), 1e-12), "a level's 60 degrees at FOV 90: 72");
    CHECK(close_to(fov_tan_half(tan(rad(80.0)), 110), tan(rad(FOV_HALF_ANGLE_LIMIT)), 1e-12), "never past 170 degrees");
    CHECK(fov_tan_half(0.0, 90) == 0.0 && fov_tan_half(-1.0, 90) == -1.0 && isnan(fov_tan_half(NAN, 90)),
          "orthographic or broken values pass through");
    for (int fov = JPT_FOV_MIN; fov <= JPT_FOV_MAX; fov++) {
        double t = fov_tan_half(game, fov);
        CHECK(isfinite(t) && t > 0.0 && close_to(2.0 * deg(atan(t)), fov, 1e-9), "FOV %d gives %d degrees across 4:3", fov, fov);
        if (fov > JPT_FOV_MIN) CHECK(t > fov_tan_half(game, fov - 1), "FOV %d is wider than %d", fov, fov - 1);
    }
}

/* Hor+: the 4:3 vertical view is kept for every shape; only the width grows. */
static void test_aspects(void) {
    static const struct {
        int w, h;
        double scale;
    } shapes[] = { { 4, 3, 1.0 }, { 16, 9, 4.0 / 3.0 }, { 16, 10, 1.2 }, { 64, 27, 16.0 / 9.0 }, { 32, 9, 8.0 / 3.0 },
                   { 5, 4, 1.0 } };
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
        CHECK(close_to(fov_hor_plus(shapes[i].w, shapes[i].h), shapes[i].scale, 1e-12), "%d:%d widens by %.4f", shapes[i].w,
              shapes[i].h, shapes[i].scale);
    CHECK(fov_hor_plus(0, 0) == 1.0 && fov_hor_plus(100, 0) == 1.0, "no shape: no change");
    for (int a = 0; a < JPT_ASPECT_COUNT; a++) {
        const jpt_aspect_t* s = &jpt_aspects[a];
        CHECK(close_to(fov_hor_plus(s->compat_w, s->compat_h), jpt_hor_plus_scale(s->compat_w, s->compat_h), 1e-9) ||
                  (a == 0 && fov_hor_plus(s->compat_w, s->compat_h) == 1.0),
              "%s: the camera and the display agree on the widening", s->key);
        for (int fov = JPT_FOV_MIN; fov <= JPT_FOV_MAX; fov += 5) {
            double t = fov_tan_half(tan(rad(37.5)), fov);
            double horizontal = fov_horizontal_degrees(t, s->compat_w, s->compat_h), vertical = fov_vertical_degrees(t);
            CHECK(close_to(vertical, 2.0 * deg(atan(tan(rad(fov / 2.0)) * 0.75)), 1e-9),
                  "%s FOV %d: the vertical view is the 4:3 one (%.2f)", s->key, fov, vertical);
            CHECK(horizontal >= fov - 1e-9 && horizontal < 179.0 && vertical > 0.0 && vertical < 179.0,
                  "%s FOV %d: %.1f x %.1f degrees, a usable projection", s->key, fov, horizontal, vertical);
        }
    }
    CHECK(close_to(fov_horizontal_degrees(1.0, 16, 9), 2.0 * deg(atan(4.0 / 3.0)), 1e-9), "FOV 90 at 16:9: 106.3 across");
    CHECK(close_to(fov_horizontal_degrees(1.0, 4, 3), 90.0, 1e-9) && close_to(fov_vertical_degrees(1.0), 73.7397952917, 1e-6),
          "FOV 90 at 4:3: 90 x 73.7");
    CHECK(close_to(fov_horizontal_degrees(tan(rad(37.5)), 4, 3), 75.0, 1e-9), "the default is the retail 75 at 4:3");
}

/* ---- mouse ---- */

static void test_mouse_scale(void) {
    mouse_scale_t m;
    long x, y, sum_x = 0, sum_y = 0;
    memset(&m, 0, sizeof m);
    mouse_scale_counts(&m, 1.0f, 7, -3, &x, &y);
    CHECK(x == 7 && y == -3 && m.rest_x == 0.0 && m.rest_y == 0.0, "1.00 passes counts through");
    mouse_scale_counts(&m, 2.0f, 3, -4, &x, &y);
    CHECK(x == 6 && y == -8, "2.00 doubles both axes");
    mouse_scale_counts(&m, 0.5f, 3, 3, &x, &y);
    CHECK(x == 1 && y == 1 && close_to(m.rest_x, 0.5, 1e-12) && close_to(m.rest_y, 0.5, 1e-12), "half a count carries");
    mouse_scale_counts(&m, 0.5f, 1, 1, &x, &y);
    CHECK(x == 1 && y == 1 && m.rest_x == 0.0, "and arrives with the next motion");
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 1000; i++) {
        mouse_scale_counts(&m, 0.25f, 1, -1, &x, &y);
        sum_x += x;
        sum_y += y;
    }
    CHECK(sum_x == 250 && sum_y == -250, "slow motion is not lost: %ld, %ld", sum_x, sum_y);
    memset(&m, 0, sizeof m);
    mouse_scale_counts(&m, 1.5f, 5, -5, &x, &y);
    CHECK(x == -y, "both directions alike (%ld, %ld)", x, y);
    mouse_scale_counts(&m, 1.0f, 2, 2, &x, &y);
    CHECK(x == 2 && y == 2 && m.rest_x == 0.0, "back at 1.00 a leftover fraction is dropped, never a jump");
}

/* Mouse sensitivity lives in the launcher only: nothing is added to the game's Controls window. */
static void test_no_controls_row(void) {
    static const char* const sources[] = { "src/host/window_host.c", "src/host/debug_menu.c", "src/host/debug_menu_panel.cpp",
                                           "src/host/debug_menu_panel.h", "src/host/ddraw/d3d11_renderer.c",
                                           "src/host/ddraw/d3d11_present.c", "src/host/frontend_hd.c", "src/host/frontend_layout.c",
                                           "src/host/frontend_assets.c", "src/host/main.c", "src/host/host.h", "CMakeLists.txt" };
    static const char* const forbidden[] = { "mouse_settings", "mouse_panel", "mp_layout", "dmp_mouse", "late_pointer",
                                             "pointer_late", "window_host_game_to_view", "window_host_game_space",
                                             "jpt_tr(\"Mouse sensitivity" };
    static const char* const tables[] = { "en", "pt-BR", "fr", "de", "es", "it", "ru" };
    char* absent[] = { read_source("src/host/mouse_settings.c"), read_source("src/host/mouse_settings.h"),
                       read_source("src/host/mouse_panel_policy.c"), read_source("src/host/mouse_panel_policy.h") };
    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; i++) {
        CHECK(absent[i] == NULL, "the in-game row's modules are gone");
        free(absent[i]);
    }
    for (size_t i = 0; i < sizeof sources / sizeof sources[0]; i++) {
        char* text = read_source(sources[i]);
        CHECK(text != NULL, "%s found", sources[i]);
        if (!text) continue;
        for (size_t k = 0; k < sizeof forbidden / sizeof forbidden[0]; k++)
            CHECK(!strstr(text, forbidden[k]), "%s has no trace of \"%s\"", sources[i], forbidden[k]);
        free(text);
    }
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        char path[128];
        char* text;
        snprintf(path, sizeof path, "assets/translations/%s.interface.txt", tables[i]);
        text = read_source(path);
        CHECK(text && !strstr(text, "Mouse sensitivity\t"), "%s: no in-game Mouse sensitivity string", tables[i]);
        free(text);
    }
}

/* ---- where each value may be written ---- */

static void test_sources(void) {
    char *debug = read_source("src/host/debug_menu.c"), *wide = read_source("src/host/widescreen.c"),
         *window = read_source("src/host/window_host.c"),
         *launcher = read_source("launcher/launcher.cpp"), *policy = read_source("src/host/debug_menu_policy.c");
    CHECK(debug && wide && window && launcher && policy, "sources found");
    if (!debug || !wide || !window || !launcher || !policy) return;
    CHECK(!strstr(debug, "jpt_ini_set") && !strstr(debug, "JPT_KEY_FOV") && !strstr(wide, "jpt_ini_set") &&
              !strstr(policy, "jpt_ini_set"),
          "no host menu writes the settings file for the field of view");
    CHECK(!strstr(debug, "widescreen_set_fov") && !strstr(debug, "widescreen_apply_fov"),
          "no host menu changes the field of view: it comes from the settings");
    CHECK(strstr(window, "widescreen_apply_fov();") != NULL, "the UI loop applies a change on the next safe frame");
    CHECK(strstr(window, "void window_host_pad_relative(int dx, int dy) { add_relative(dx, dy); }") != NULL,
          "the gamepad's look keeps its own sensitivity");
    CHECK(strstr(window, "mouse_relative(input.data.mouse.lLastX, input.data.mouse.lLastY)") &&
              strstr(window, "mouse_relative(client.x - s.last_move.x, client.y - s.last_move.y)"),
          "Raw Input and cursor motion both take the sensitivity");
    {
        const char *graphics = strstr(launcher, "void PageGraphics("), *resolution = strstr(launcher, "\"Render resolution\", kRenderResolutionHelp"),
                   *fov = strstr(launcher, "        FieldOfViewRow(ui);"), *aa = strstr(launcher, "        AntiAliasingRow(ui);"),
                   *ao = strstr(launcher, "ui.Combo(\"Ambient occlusion\"");
        const char *keyboard = strstr(launcher, "ui.Section(\"Keyboard & Mouse\""), *sens = strstr(launcher, "            MouseSensitivityRow(ui);"),
                   *invert = strstr(launcher, "            InvertMouseRow(ui);");
        int aa_rows = 0, filter_rows = 0;
        for (const char* p = launcher; (p = strstr(p, "\"Anti-aliasing\"")) != NULL; p++) aa_rows++;
        for (const char* p = launcher; (p = strstr(p, "\"Texture filtering\"")) != NULL; p++) filter_rows++;
        CHECK(graphics && resolution && fov && aa && ao && graphics < resolution && resolution < fov && fov < aa && aa < ao,
              "Graphics: Render resolution, Field of view, Anti-aliasing, Ambient occlusion");
        CHECK(keyboard && sens && invert && keyboard < sens && sens < invert, "Keyboard & Mouse: Mouse sensitivity, then Invert mouse");
        CHECK(strstr(launcher, "SliderWithDefault(\"mouse_sensitivity\", sensitivity,") != NULL &&
                  strstr(launcher, "SliderWithDefault(\"fov\", fov,") != NULL,
              "Mouse sensitivity and Field of view share the slider with a Default button");
        CHECK(aa_rows == 1 && filter_rows == 1 && strstr(launcher, "{\"anisotropic16\", \"Anisotropic 16x\"}") &&
                  !strstr(launcher, "\"Anisotropic filtering\""),
              "one Anti-aliasing row, one Texture filtering row reaching 16x, no separate AF row");
        CHECK(strstr(launcher, "samples <= 8; samples *= 2") && strstr(launcher, "if (msaa & (1u << samples))"),
              "anti-aliasing lists only the levels the card supports");
        CHECK(!strstr(launcher, "Safe Mode") && !strstr(launcher, "DisableSafeMode") && !strstr(debug, "Safe Mode") &&
                  !strstr(policy, "Safe Mode"),
              "Safe Mode is never an option");
    }
    free(debug);
    free(wide);
    free(window);
    free(launcher);
    free(policy);
}

int main(void) {
    test_profile_activate();
    setvbuf(stdout, NULL, _IONBF, 0);
    test_settings();
    test_migration();
    test_persistence();
    test_fov_math();
    test_aspects();
    test_mouse_scale();
    test_no_controls_row();
    test_sources();
    printf("fov_mouse_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

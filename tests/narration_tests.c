/*
 * Disable Hammond Narration, headless: which samples are Hammond's, the default and the runtime switch, the setting's
 * persistence, and the retail facts the host hook relies on (from game/trespass.exe, Stream.tpa and the embedded English
 * captions when present). Build: CMake target narration_tests.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_config.h"
#include "jpt_hammond.h"
#include "jpt_hash.h"
#include "narration_policy.h"

#ifndef JPT_SOURCE_DIR
#define JPT_SOURCE_DIR "."
#endif

static int s_failures, s_checks;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* Sample names the level scripts use: Hammond's narration, Anne's lines, a conversation of both, and other sounds. */
static const char* const k_hammond[] = { "VH2", "VH5", "VH11", "VH101", "vh11", "VH20a", "VH0", "VH299" };
static const char* const k_not_hammond[] = { "VA89", "VA24", "VA208", "VA34", "ANNEGET1", "ANNESWIM1", "AnneGetGun3",
                                             "VC H128 - A120", "VC H29 - A64", "VRADIO", "VBB01", "VCRAYCOMPUTER",
                                             "SPEC-PHONE OPERATOR", "SPEC-TOUR GUIDE", "GUN - BARRET LIGHT 02",
                                             "MUSIC - HAMMOND HOUSE", "VH", "VHX", "VH300", "VH1e", "V11" };

static void test_identification(void) {
    CHECK(jpt_sample_id("VH2") == 0xcace7ddfu && jpt_sample_id("VH11") == 0xbe57bd4cu && jpt_sample_id("VH101") == 0xe51384f0u,
          "the retail IDs of three Hammond samples");
    CHECK(jpt_sample_id("VA89") == 0x6e9fb5b8u && jpt_sample_id("VA24") == 0xeac1218fu, "the retail IDs of two Anne samples");
    for (size_t i = 0; i < sizeof k_hammond / sizeof k_hammond[0]; i++)
        CHECK(jpt_hammond_sample(jpt_sample_id(k_hammond[i])), "%s is Hammond's", k_hammond[i]);
    for (size_t i = 0; i < sizeof k_not_hammond / sizeof k_not_hammond[0]; i++)
        CHECK(!jpt_hammond_sample(jpt_sample_id(k_not_hammond[i])), "%s is not Hammond's", k_not_hammond[i]);
    CHECK(!jpt_hammond_sample(0) && !jpt_hammond_sample(0xFFFFFFFFu), "no sample, or an unknown one, is not Hammond's");
}

static void test_name_space(void) {
    char name[48];
    int hammond = 0;
    for (int n = 0; n <= 299; n++) {
        const char* const suffix[] = { "", "a", "b", "c", "d" };
        for (int s = 0; s < 5; s++) {
            snprintf(name, sizeof name, "VH%d%s", n, suffix[s]);
            hammond += jpt_hammond_sample(jpt_sample_id(name)) ? 1 : 0;
            snprintf(name, sizeof name, "VA%d%s", n, suffix[s]);
            CHECK(!jpt_hammond_sample(jpt_sample_id(name)), "%s (Anne) is not Hammond's", name);
        }
    }
    CHECK(hammond == 1500, "every VH sample name is recognised (%d of 1500)", hammond);
    for (int h = 0; h <= 299; h++)
        for (int a = 0; a <= 299; a++) {
            snprintf(name, sizeof name, "VC H%d - A%d", h, a);
            if (jpt_hammond_sample(jpt_sample_id(name))) CHECK(0, "%s (both speakers) is not Hammond's", name);
        }
}

static void test_switch(void) {
    const uint32_t hammond = jpt_sample_id("VH11"), anne = jpt_sample_id("VA24"), effect = jpt_sample_id("GUN - BARRET LIGHT 02"),
                   both = jpt_sample_id("VC H128 - A120");
    CHECK(!narration_disabled(), "the narration is on by default");
    CHECK(!narration_blocked(hammond) && !narration_blocked(anne), "by default Hammond plays as in retail");
    narration_set_disabled(1);
    CHECK(narration_disabled() && narration_blocked(hammond), "disabled: Hammond's voice-over is not started");
    CHECK(narration_blocked(jpt_sample_id("VH101")) && narration_blocked(jpt_sample_id("VH2")),
          "disabled: every Hammond sample is blocked");
    CHECK(!narration_blocked(anne) && !narration_blocked(jpt_sample_id("VA89")), "disabled: Anne's voice-overs still play");
    CHECK(!narration_blocked(effect) && !narration_blocked(jpt_sample_id("MUSIC - HAMMOND HOUSE")),
          "disabled: effects and music still play");
    CHECK(!narration_blocked(both) && !narration_blocked(jpt_sample_id("VRADIO")),
          "disabled: a conversation of both speakers is left whole, Anne's lines in it included");
    narration_set_disabled(0);
    CHECK(!narration_blocked(hammond), "switched off again at run time: Hammond plays at once");
    narration_set_disabled(7);
    CHECK(narration_blocked(hammond), "any non-zero value disables");
    narration_set_disabled(0);
    CHECK(!narration_disabled() && !narration_blocked(hammond), "off again, without a restart");
}

static void test_persistence(void) {
    char path[512];
    const char* key = JPT_KEY_DISABLE_HAMMOND_NARRATION;
    const char* on = "1";
    const char* off = "0";
    jpt_config_t c;
    FILE* f;
    snprintf(path, sizeof path, "%s", "narration_tests_settings.ini");
    CHECK(!strcmp(key, "disable_hammond_narration"), "the setting's key");
    jpt_config_defaults(&c);
    CHECK(c.disable_hammond_narration == 0 && !strcmp(jpt_default_value(key), "0"), "Off by default");
    CHECK(jpt_config_valid(key, "1") && jpt_config_valid(key, "off") && !jpt_config_valid(key, "maybe"), "it is a switch");
    f = fopen(path, "w");
    CHECK(f != NULL, "a settings file for the test");
    if (!f) return;
    fputs("settings_version = 3\nminimap = 0\n", f);
    fclose(f);
    CHECK(jpt_config_load(path, &c) && c.disable_hammond_narration == 0, "a file without the key keeps it Off");
    CHECK(jpt_ini_set(path, &key, &on, 1) && jpt_config_load(path, &c) && c.disable_hammond_narration == 1,
          "disable_hammond_narration = 1 is read back");
    CHECK(c.minimap == 0, "the other settings are kept");
    CHECK(jpt_ini_set(path, &key, &off, 1) && jpt_config_load(path, &c) && c.disable_hammond_narration == 0,
          "disable_hammond_narration = 0 is read back");
    CHECK(jpt_ini_set(path, &key, &on, 1) && jpt_config_load(path, &c) && c.disable_hammond_narration == 1,
          "the choice survives a restart");
    remove(path);
}

static char* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    char* data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char*)malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) == (size_t)n) {
        data[n] = 0;
        *size = (size_t)n;
        fclose(f);
        return data;
    }
    free(data);
    fclose(f);
    return NULL;
}

/* The embedded English captions: a Hammond sample's captions sit under his ID, and they are shown only while that
 * sample plays, so keeping the sample from starting keeps them from showing. */
static void test_captions(void) {
    char path[512], *text, *line;
    size_t size = 0;
    int samples = 0, hammond = 0, hammond_lines = 0, hammond_introduces = 0, anne_listed = 0, anne_blocked = 0;
    uint32_t current = 0;
    int current_hammond = 0;
    snprintf(path, sizeof path, "%s/assets/translations/en.subtitles.txt", JPT_SOURCE_DIR);
    text = read_file(path, &size);
    CHECK(text != NULL, "the English captions are readable");
    if (!text) return;
    narration_set_disabled(1);
    for (line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "S\tstream\t", 9)) {
            current = (uint32_t)strtoul(line + 9, NULL, 16);
            samples++;
            current_hammond = narration_blocked(current);
            hammond += current_hammond;
        } else if (!strncmp(line, "C\t", 2)) {
            if (current_hammond) hammond_lines++;
            if (current == jpt_sample_id("VH2") && strstr(line, "John Parker Hammond")) hammond_introduces = current_hammond;
            if (current == jpt_sample_id("VA24") && strstr(line, "No dinosaurs in here")) {
                anne_listed = 1;
                anne_blocked = current_hammond;
            }
        }
    }
    narration_set_disabled(0);
    CHECK(samples > 300, "the captions cover the retail samples (%d)", samples);
    CHECK(hammond > 60 && hammond < samples / 2, "a share of them is Hammond's (%d of %d)", hammond, samples);
    CHECK(hammond_lines > hammond, "his samples carry captions (%d lines)", hammond_lines);
    CHECK(hammond_introduces, "his introduction, 'My name is John Parker Hammond', is in a blocked sample");
    CHECK(anne_listed && !anne_blocked, "Anne's 'No dinosaurs in here' is captioned and not blocked");
    free(text);
}

static uint32_t rd32(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* game/trespass.exe: CVoiceOverAction's vtable holds the execute function in slot 2; it reads the sample ID at +8. */
static const unsigned char* image_at(const unsigned char* exe, size_t size, uint32_t va) {
    uint32_t pe = rd32(exe + 0x3C), base, sections, opt;
    if (pe + 0x100 > size) return NULL;
    sections = exe[pe + 6] | (uint32_t)exe[pe + 7] << 8;
    opt = exe[pe + 20] | (uint32_t)exe[pe + 21] << 8;
    base = rd32(exe + pe + 24 + 28);
    for (uint32_t i = 0; i < sections; i++) {
        const unsigned char* s = exe + pe + 24 + opt + 40 * i;
        uint32_t vsize = rd32(s + 8), rva = rd32(s + 12), rsize = rd32(s + 16), raw = rd32(s + 20);
        uint32_t span = vsize > rsize ? vsize : rsize;
        if (va >= base + rva && va < base + rva + span && raw + (va - base - rva) < size) return exe + raw + (va - base - rva);
    }
    return NULL;
}

static void test_retail(void) {
    static const unsigned char prologue[] = { 0x83, 0xEC, 0x58, 0x56, 0x8B, 0xF1 };
    char path[512];
    size_t size = 0;
    unsigned char* exe;
    const unsigned char *slot, *code;
    snprintf(path, sizeof path, "%s/game/trespass.exe", JPT_SOURCE_DIR);
    exe = (unsigned char*)read_file(path, &size);
    if (!exe) {
        printf("narration_tests: %s not found, the retail lint is skipped\n", path);
        return;
    }
    slot = image_at(exe, size, 0x0064B780u + 8u);
    CHECK(slot && rd32(slot) == 0x0046CD80u, "CVoiceOverAction's vtable slot 2 is 0x0046CD80");
    code = image_at(exe, size, 0x0046CD80u);
    CHECK(code && !memcmp(code, prologue, sizeof prologue), "0x0046CD80 starts as the host's override expects");
    /* mov esi, [esi+8]: the sample ID is read from the action at +8 and goes into the message at msg+0x10. */
    code = image_at(exe, size, 0x0046CDBDu);
    CHECK(code && code[0] == 0x8B && code[1] == 0x76 && code[2] == 0x08, "the action's +8 is the ID the message carries");
    code = image_at(exe, size, 0x0046CE44u);
    CHECK(code && code[0] == 0xC3, "the action ends with a plain ret (no arguments)");
    free(exe);
}

static void test_stream_directory(void) {
    char path[512];
    size_t size = 0;
    unsigned char* tpa;
    uint32_t count, hammond = 0;
    snprintf(path, sizeof path, "%s/game/versions/en-116/data/Stream.tpa", JPT_SOURCE_DIR);
    {
        FILE* f = fopen(path, "rb");
        unsigned char head[0x18];
        if (!f) {
            printf("narration_tests: %s not found, the directory check is skipped\n", path);
            return;
        }
        if (fread(head, 1, sizeof head, f) != sizeof head) {
            fclose(f);
            return;
        }
        count = rd32(head + 4);
        size = (size_t)count * 56u;
        tpa = (unsigned char*)malloc(size ? size : 1);
        if (!tpa || count > 8192 || fread(tpa, 1, size, f) != size) {
            free(tpa);
            fclose(f);
            CHECK(0, "the retail Stream.tpa directory is readable");
            return;
        }
        fclose(f);
    }
    narration_set_disabled(1);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t id = rd32(tpa + 56u * i);
        if (narration_blocked(id)) {
            hammond++;
            CHECK(id != jpt_sample_id("VA24") && id != jpt_sample_id("VA89") && id != jpt_sample_id("VC H128 - A120"),
                  "no Anne sample is in Hammond's set");
        }
    }
    narration_set_disabled(0);
    CHECK(count == 455, "the retail Stream.tpa has its 455 samples (%u)", count);
    CHECK(hammond == 153, "Hammond's narration is 153 of them (%u)", hammond);
    free(tpa);
}

static int contains(const char* path, const char* needle) {
    size_t size = 0;
    char* text = read_file(path, &size);
    int found = text && strstr(text, needle) != NULL;
    free(text);
    return found;
}

static void test_wiring(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/src/host/narration.c", JPT_SOURCE_DIR);
    CHECK(contains(path, "GVA(0x0046CD80)") && contains(path, "GSUB(0x0046CD80)") && contains(path, "narration_blocked(MEM32(action"),
          "the hook is the voice-over action, gated by the policy");
    snprintf(path, sizeof path, "%s/src/host/diagnostics.c", JPT_SOURCE_DIR);
    CHECK(contains(path, "narration_lookup(va)"), "the dispatch asks the narration hook");
    snprintf(path, sizeof path, "%s/src/host/main.c", JPT_SOURCE_DIR);
    CHECK(contains(path, "narration_configure(cfg.disable_hammond_narration)") && contains(path, "narration_install()"),
          "the host reads the setting and installs the hook");
    snprintf(path, sizeof path, "%s/launcher/launcher.cpp", JPT_SOURCE_DIR);
    CHECK(contains(path, "\"Disable Hammond Narration\"") && contains(path, "JPT_KEY_DISABLE_HAMMOND_NARRATION, false"),
          "the launcher's Gameplay page has the option, Off by default");
}

int main(void) {
    test_identification();
    test_name_space();
    test_switch();
    test_persistence();
    test_captions();
    test_retail();
    test_stream_directory();
    test_wiring();
    printf("narration_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}

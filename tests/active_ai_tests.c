/*
 * Headless checks of Active AI (src/host/active_ai_policy.c): the limit per mode, the retail loop orders over 1..N, the
 * think scheduler, Activate's choice, the physics box guard, save compatibility, and that every retail function holding
 * the limit is replaced in Enhanced (from game/trespass.exe and analysis/functions.json when present).
 * Build: CMake target active_ai_tests.
 */
#include <math.h>
#include "test_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "active_ai_policy.h"

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

/* The retail functions whose code holds the limit (the arrays at CAISystem +0x3188 / +0x31A0, or the save block). */
static const uint32_t k_limit_functions[] = { 0x00419330u, 0x004195A0u, 0x00419800u, 0x004199D0u,
                                              0x00419C60u, 0x00419CE0u, 0x00419DB0u, 0x00419E30u,
                                              0x0041A200u, 0x0041AE50u, 0x0041AF00u, 0x004213F0u };
#define LIMIT_FUNCTIONS (sizeof k_limit_functions / sizeof k_limit_functions[0])

static void test_limits(void) {
    int order[ACTIVE_AI_MAX_LIMIT];
    CHECK(active_ai_limit(0) == 3 && active_ai_limit(1) == 6);
    CHECK(active_ai_descending(3, order) == 3 && order[0] == 3 && order[1] == 2 && order[2] == 1);
    CHECK(active_ai_descending(6, order) == 6 && order[0] == 6 && order[5] == 1);
    CHECK(active_ai_descending(9, order) == 6);
    CHECK(ACTIVE_AI_ENHANCED_LIMIT <= ACTIVE_AI_MAX_LIMIT && ACTIVE_AI_MAX_LIMIT + 1 <= ACTIVE_AI_SAVE_ENTRIES);
}

/* Retail: idx = ++cursor; idx >= 4 restarts; empty entries are skipped within the tick. */
static void test_think_scheduler(void) {
    uint32_t retail[4] = { 0, 0xA1, 0xA2, 0xA3 }, six[7] = { 0, 1, 2, 3, 4, 5, 6 }, holes[7] = { 0, 0, 0x22, 0, 0, 0x55, 0 };
    int32_t cursor = -1;
    int seen[16], n = 0;
    for (int t = 0; t < 8; t++) seen[n++] = active_ai_think_next(retail, 3, &cursor);
    CHECK(seen[0] == 1 && seen[1] == 2 && seen[2] == 3 && seen[3] == -1 && seen[4] == 1 && seen[7] == -1);
    CHECK(cursor == -1);
    cursor = -1;
    n = 0;
    for (int t = 0; t < 14; t++) seen[n++] = active_ai_think_next(six, 6, &cursor);
    for (int t = 0; t < 6; t++) CHECK(seen[t] == t + 1 && seen[t + 7] == t + 1);
    CHECK(seen[6] == -1 && seen[13] == -1);
    cursor = -1;
    CHECK(active_ai_think_next(holes, 6, &cursor) == 2 && cursor == 2);
    CHECK(active_ai_think_next(holes, 6, &cursor) == 5 && cursor == 5);
    CHECK(active_ai_think_next(holes, 6, &cursor) == -1 && cursor == -1);
    /* Enhanced with the animals in entries 1..3 thinks as retail does, one idle tick a round longer. */
    {
        uint32_t three[7] = { 0, 0xA1, 0xA2, 0xA3, 0, 0, 0 };
        cursor = -1;
        CHECK(active_ai_think_next(three, 6, &cursor) == 1 && active_ai_think_next(three, 6, &cursor) == 2 &&
              active_ai_think_next(three, 6, &cursor) == 3 && active_ai_think_next(three, 6, &cursor) == -1);
    }
}

/* Activate over a model: occupied entries have distances; returns the entry placed into, -1 refused, or -(10 + k) for
 * the eviction of k. */
static int model_activate(const double* dist, const int* occupied, int limit, float new_distance) {
    active_ai_pick_t p;
    active_ai_pick_begin(&p, new_distance);
    for (int i = limit; i >= 1; i--) active_ai_pick_slot(&p, i, occupied[i], dist[i]);
    if (p.empty >= 0) return p.empty;
    return p.farthest >= 0 ? -(10 + p.farthest) : -1;
}

static void test_activate_choice(void) {
    double d[7] = { 0, 50, 10, 70, 70, 20, 5 };
    int occ[7] = { 0, 1, 1, 1, 1, 1, 1 };
    CHECK(model_activate(d, occ, 3, 30.0f) == -13);
    CHECK(model_activate(d, occ, 6, 30.0f) == -14); /* the tie at 70 keeps the first visited, the top entry */
    CHECK(model_activate(d, occ, 6, 80.0f) == -1);
    CHECK(model_activate(d, occ, 6, 70.0f) == -1);  /* not farther than the new one */
    occ[2] = 0;
    occ[5] = 0;
    CHECK(model_activate(d, occ, 6, 1.0f) == 2); /* the lowest empty entry */
    occ[2] = 1;
    CHECK(model_activate(d, occ, 6, 1.0f) == 5);
    CHECK(model_activate(d, occ, 3, 1.0f) == -13);
    {
        double nan_d[7] = { 0, NAN, NAN, NAN, 0, 0, 0 };
        int all[7] = { 0, 1, 1, 1, 0, 0, 0 };
        CHECK(model_activate(nan_d, all, 3, 1.0f) == -1);
    }
}

static void test_x87(void) {
    float a[3] = { 1.0f, 2.0f, 3.0f }, b[3] = { 0.5f, -1.0f, 7.25f };
    float big[3] = { 16777216.0f, 3.0f, 1e-3f }, zero[3] = { 0, 0, 0 };
    double x = 0.5, y = 3.0, z = -4.25;
    CHECK(active_ai_dist2_zyx(a, b) == (z * z + y * y) + x * x);
    CHECK(active_ai_dist2_zxy(a, b) == (z * z + x * x) + y * y);
    CHECK(active_ai_dist2_zyx(a, b) == active_ai_dist2_zyx(b, a));
    CHECK(active_ai_dist2_zyx(big, zero) == ((double)1e-3f * (double)1e-3f + 9.0) + 16777216.0 * 16777216.0);
    CHECK(active_ai_x87_c0(1.0, 2.0) && !active_ai_x87_c0(2.0, 2.0) && !active_ai_x87_c0(3.0, 2.0) &&
          active_ai_x87_c0(NAN, 2.0) && active_ai_x87_c0(1.0, NAN));
    CHECK(active_ai_x87_c0c3(1.0, 2.0) && active_ai_x87_c0c3(2.0, 2.0) && !active_ai_x87_c0c3(3.0, 2.0) &&
          active_ai_x87_c0c3(NAN, 2.0));
}

static void test_box_guard(void) {
    uint32_t parts[8] = { 1, 2, 0, 3, 0, 0, 4, 5 }, none[8] = { 0 };
    CHECK(active_ai_count_set(parts, 8) == 5 && active_ai_count_set(none, 8) == 0);
    CHECK(active_ai_boxes_allow(42, 8, 0, ACTIVE_AI_BOX_POOL));
    CHECK(!active_ai_boxes_allow(43, 8, 0, ACTIVE_AI_BOX_POOL));
    CHECK(active_ai_boxes_allow(50, 8, 8, ACTIVE_AI_BOX_POOL));
    CHECK(!active_ai_boxes_allow(50, 8, 7, ACTIVE_AI_BOX_POOL));
    CHECK(active_ai_boxes_allow(0, 0, 0, ACTIVE_AI_BOX_POOL) && ACTIVE_AI_BOX_POOL == 50);
    /* Player and three animals of eight parts fit; six do not, before any loose object. */
    CHECK(active_ai_boxes_allow(8 + 2 * 8, 8, 0, ACTIVE_AI_BOX_POOL));
    CHECK(!active_ai_boxes_allow(8 + 5 * 8, 8, 0, ACTIVE_AI_BOX_POOL));
}

/* Retail Save (0x0041AE50): entries 0..3, then six zeros. */
static void retail_save(const uint32_t active[4], uint32_t out[10]) {
    for (int i = 0; i < 4; i++) out[i] = active[i];
    for (int i = 4; i < 10; i++) out[i] = 0;
}

/* A loader: hands the first `activations` handles to Activate (lowest empty entry of 1..limit), reads all ten. */
static int model_load(const uint32_t block[10], int activations, int limit, uint32_t active[7]) {
    int read = 0;
    memset(active, 0, 7 * sizeof(uint32_t));
    for (int i = 0; i < ACTIVE_AI_SAVE_ENTRIES; i++, read++) {
        if (i >= activations || !block[i]) continue;
        for (int e = 1; e <= limit; e++)
            if (!active[e]) {
                active[e] = block[i];
                break;
            }
    }
    return read;
}

static void test_save_block(void) {
    uint32_t enhanced[7] = { 0, 0xA1, 0, 0xA3, 0, 0, 0 }, retail_active[4] = { 0, 0xA1, 0, 0xA3 };
    uint32_t a[10], b[10], loaded[7];
    active_ai_save_values(enhanced, 6, a);
    retail_save(retail_active, b);
    CHECK(!memcmp(a, b, sizeof a));
    active_ai_save_values(retail_active, 3, a);
    CHECK(!memcmp(a, b, sizeof a));
    CHECK(active_ai_load_activations(3) == 4 && active_ai_load_activations(6) == 7);
    /* A retail save loads in Enhanced as in retail. */
    CHECK(model_load(b, active_ai_load_activations(6), 6, loaded) == 10);
    CHECK(loaded[1] == 0xA1 && loaded[2] == 0xA3 && !loaded[3] && !loaded[4]);
    /* Six animals round-trip in their order. */
    {
        uint32_t six[7] = { 0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6 }, again[10];
        active_ai_save_values(six, 6, a);
        CHECK(a[0] == 0 && a[4] == 0xB4 && a[6] == 0xB6 && !a[7] && !a[8] && !a[9]);
        CHECK(model_load(a, active_ai_load_activations(6), 6, loaded) == 10);
        CHECK(!memcmp(loaded, six, sizeof six));
        active_ai_save_values(loaded, 6, again);
        CHECK(!memcmp(a, again, sizeof a));
        /* The retail loader activates the first three, reads the same ten entries and leaves 4..6 inactive. */
        CHECK(model_load(a, active_ai_load_activations(3), 3, loaded) == 10);
        CHECK(loaded[1] == 0xB1 && loaded[2] == 0xB2 && loaded[3] == 0xB3 && !loaded[4] && !loaded[5] && !loaded[6]);
    }
}

static void test_override_table(void) {
    for (unsigned i = 1; i < active_ai_override_count; i++) CHECK(active_ai_override_va(i - 1) < active_ai_override_va(i));
    for (size_t i = 0; i < LIMIT_FUNCTIONS; i++) CHECK(active_ai_overrides(k_limit_functions[i]));
    CHECK(active_ai_overrides(0x004218C0u) && !active_ai_overrides(0x00419720u) && !active_ai_overrides(0x00419AA0u));
}

/* game/trespass.exe, read as raw sections. */
static unsigned char* s_exe;
static size_t s_exe_size;
static struct {
    char name[9];
    uint32_t va, size, raw, raw_size;
} s_sec[16];
static int s_sec_count;
static uint32_t s_base;

static int load_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    uint32_t pe;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    s_exe_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_exe = (unsigned char*)malloc(s_exe_size);
    if (!s_exe || s_exe_size < 0x400 || fread(s_exe, 1, s_exe_size, f) != s_exe_size) {
        fclose(f);
        return 0;
    }
    fclose(f);
    pe = *(const uint32_t*)(s_exe + 0x3C);
    s_base = *(const uint32_t*)(s_exe + pe + 24 + 28);
    for (int i = 0; i < *(const uint16_t*)(s_exe + pe + 6) && i < 16; i++) {
        const unsigned char* s = s_exe + pe + 24 + *(const uint16_t*)(s_exe + pe + 20) + i * 40;
        memcpy(s_sec[i].name, s, 8);
        s_sec[i].va = s_base + *(const uint32_t*)(s + 12);
        s_sec[i].size = *(const uint32_t*)(s + 8);
        s_sec[i].raw_size = *(const uint32_t*)(s + 16);
        s_sec[i].raw = *(const uint32_t*)(s + 20);
        s_sec_count = i + 1;
    }
    return 1;
}

static const unsigned char* exe_at(uint32_t va, uint32_t n) {
    for (int i = 0; i < s_sec_count; i++)
        if (va >= s_sec[i].va && va + n <= s_sec[i].va + s_sec[i].raw_size && s_sec[i].raw + (va - s_sec[i].va) + n <= s_exe_size)
            return s_exe + s_sec[i].raw + (va - s_sec[i].va);
    return NULL;
}

/* analysis/functions.json: "address": N ... "end": M for every function. */
static struct {
    uint32_t start, end;
} * s_fn;
static int s_fn_count;

static int load_functions(const char* path) {
    FILE* f = fopen(path, "rb");
    char* text;
    const char* p;
    long n;
    int cap = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)malloc((size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(text);
        return 0;
    }
    fclose(f);
    text[n] = 0;
    for (p = strstr(text, "\"address\": "); p; p = strstr(p, "\"address\": ")) {
        const char* e = strstr(p, "\"end\": ");
        uint32_t start = (uint32_t)strtoul(p + 11, NULL, 10);
        if (!e) break;
        if (s_fn_count == cap) {
            void* grown;
            cap = cap ? cap * 2 : 4096;
            grown = realloc(s_fn, (size_t)cap * sizeof *s_fn);
            if (!grown) break;
            s_fn = grown;
        }
        s_fn[s_fn_count].start = start;
        s_fn[s_fn_count].end = (uint32_t)strtoul(e + 7, NULL, 10);
        s_fn_count++;
        p = e;
    }
    free(text);
    return s_fn_count > 0;
}

static int containing(uint32_t va, uint32_t* out, int cap) {
    int n = 0;
    for (int i = 0; i < s_fn_count; i++)
        if (s_fn[i].start <= va && va < s_fn[i].end && n < cap) out[n++] = s_fn[i].start;
    return n;
}

static int has_bytes(uint32_t start, uint32_t end, const unsigned char* pat, uint32_t len) {
    for (uint32_t va = start; va + len <= end; va++) {
        const unsigned char* p = exe_at(va, len);
        if (p && !memcmp(p, pat, len)) return 1;
    }
    return 0;
}

static void test_retail_lint(const char* root) {
    static const uint32_t displacements[] = { 0x3188u, 0x3194u, 0x31A0u, 0x31ACu };
    static const unsigned char four[] = { 0xBB, 4, 0, 0, 0 }, six[] = { 0xBF, 6, 0, 0, 0 };
    char path[512];
    uint32_t found[64];
    int found_count = 0;
    snprintf(path, sizeof path, "%s/game/trespass.exe", root);
    if (!load_exe(path)) {
        printf("active_ai_tests: %s not installed, retail lint skipped\n", path);
        return;
    }
    snprintf(path, sizeof path, "%s/analysis/functions.json", root);
    if (!load_functions(path)) {
        printf("active_ai_tests: %s missing, retail lint skipped\n", path);
        return;
    }
    for (int s = 0; s < s_sec_count; s++) {
        if (strncmp(s_sec[s].name, ".text", 5)) continue;
        for (uint32_t off = 0; off + 4 <= s_sec[s].raw_size && s_sec[s].raw + off + 4 <= s_exe_size; off++) {
            uint32_t v, owners[8];
            int n;
            memcpy(&v, s_exe + s_sec[s].raw + off, 4);
            if (v != displacements[0] && v != displacements[1] && v != displacements[2] && v != displacements[3]) continue;
            n = containing(s_sec[s].va + off, owners, 8);
            for (int k = 0; k < n; k++) {
                int known = 0;
                for (int j = 0; j < found_count; j++) known |= found[j] == owners[k];
                if (!known && found_count < 64) found[found_count++] = owners[k];
                if (!active_ai_overrides(owners[k])) {
                    s_checks++;
                    s_failures++;
                    printf("FAIL: sub_%08X holds the active AI arrays (0x%X at 0x%08X) but Enhanced does not replace it\n",
                           owners[k], v, s_sec[s].va + off);
                }
            }
        }
    }
    CHECK(found_count >= 11);
    for (size_t i = 0; i < LIMIT_FUNCTIONS; i++) {
        int seen = 0;
        for (int j = 0; j < found_count; j++) seen |= found[j] == k_limit_functions[i];
        if (k_limit_functions[i] == 0x0041AF00u) continue; /* Load holds only the block sizes, checked below */
        CHECK(seen);
    }
    /* Save and Load: mov ebx, 4 (handles) and mov edi, 6 (padding), the fixed 10-entry block. */
    for (int i = 0; i < s_fn_count; i++) {
        if (s_fn[i].start != 0x0041AE50u && s_fn[i].start != 0x0041AF00u) continue;
        CHECK(has_bytes(s_fn[i].start, s_fn[i].end, four, 5) && has_bytes(s_fn[i].start, s_fn[i].end, six, 5));
    }
    /* The box pool: the allocator stops at 50 (cmp ecx, 0x32) over the in-use words at 0x0069A138, 0x1090 apart. */
    {
        static const unsigned char pool[] = { 0xB8, 0x38, 0xA1, 0x69, 0x00, 0x39, 0x18, 0x74, 0x13,
                                              0x05, 0x90, 0x10, 0x00, 0x00, 0x41, 0x3D, 0x58, 0xDD, 0x6C, 0x00 };
        static const unsigned char cap[] = { 0x83, 0xF9, 0x32 };
        CHECK(has_bytes(0x004DA751u, 0x004DA766u, pool, sizeof pool) && has_bytes(0x004DA76Du, 0x004DA770u, cap, 3));
        CHECK((0x006CDD58u - 0x0069A138u + 0x108Fu) / 0x1090u == ACTIVE_AI_BOX_POOL);
    }
    /* The animal's eight parts: 0x004213F0 walks +0xAC..+0xC8 of each active animal. */
    {
        static const unsigned char parts[] = { 0xBE, 0xAC, 0x00, 0x00, 0x00 }, end[] = { 0x81, 0xFE, 0xCC, 0x00, 0x00, 0x00 };
        CHECK(has_bytes(0x004217E8u, 0x004217EDu, parts, 5) && has_bytes(0x0042180Bu, 0x00421811u, end, 6));
    }
}

int main(void) {
    test_profile_activate();
    test_limits();
    test_think_scheduler();
    test_activate_choice();
    test_x87();
    test_box_guard();
    test_save_block();
    test_override_table();
    test_retail_lint(JPT_SOURCE_DIR);
    printf("active_ai_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}

/*
 * Official Patch 1.1 (build 117): the terrain level fix over GROFF scene images, the retail fingerprint, modified and
 * already fixed files, the setting and its wiring. With game/data present every retail scene is fixed; with
 * JPT_PATCH117_DIR naming the patch's level folder, the result is compared with the official scene's section.
 * Headless: ninja -C <build dir> patch117_tests && patch117_tests.exe
 */
#include <windows.h>
#include <lzexpand.h>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_config.h"
#include "patch117_policy.h"
#include "patch117_rules.h"

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

typedef struct {
    uint8_t* data;
    size_t size, cap;
} buf_t;

static void put(buf_t* b, const void* p, size_t n) {
    if (b->size + n > b->cap) {
        b->cap = (b->size + n) * 2;
        b->data = (uint8_t*)realloc(b->data, b->cap);
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void put32(buf_t* b, uint32_t v) { put(b, &v, 4); }
static void putf(buf_t* b, float v) { put(b, &v, 4); }
static void set32(buf_t* b, size_t at, uint32_t v) { memcpy(b->data + at, &v, 4); }

typedef struct {
    const char* name;
    const uint8_t* bytes;
    uint32_t size;
} section_t;

/* A GROFF image as the game writes one: header, section table, section bytes, symbol table. */
static buf_t groff(const section_t* sections, int count) {
    buf_t b = { 0 };
    size_t table;
    for (int i = 0; i < 12; i++) put32(&b, 0);
    set32(&b, 0, PATCH117_GROFF_MAGIC);
    set32(&b, 8, (uint32_t)count);
    set32(&b, 12, (uint32_t)count);
    set32(&b, 32, 10);
    table = b.size;
    for (int i = 0; i < count * 8; i++) put32(&b, 0);
    for (int i = 0; i < count; i++) {
        set32(&b, table + 32u * i, 0x100u + (uint32_t)i);
        set32(&b, table + 32u * i + 8, (uint32_t)b.size);
        set32(&b, table + 32u * i + 12, sections[i].size);
        set32(&b, table + 32u * i + 28, 0x200u + (uint32_t)i);
        put(&b, sections[i].bytes, sections[i].size);
    }
    set32(&b, 20, (uint32_t)b.size);
    for (int i = 0; i < count; i++) {
        uint32_t length = (uint32_t)strlen(sections[i].name) + 1;
        put32(&b, 0x100u + (uint32_t)i);
        put32(&b, length);
        put32(&b, 1);
        put(&b, sections[i].name, length);
    }
    set32(&b, 4, (uint32_t)b.size);
    return b;
}

static buf_t terrain_bytes(uint32_t version, float tol, float far_tol) {
    buf_t t = { 0 };
    put32(&t, version);
    putf(&t, tol);
    putf(&t, far_tol);
    putf(&t, 8.0f);
    putf(&t, 2.0f);
    put32(&t, 8);
    put32(&t, 3);
    putf(&t, 0.2f);
    putf(&t, 0.2f);
    putf(&t, 32.0f);
    putf(&t, 230.0f);
    putf(&t, 230.0f);
    put32(&t, 1);
    put32(&t, 1);
    for (int i = 0; i < 3; i++) put32(&t, 0);
    putf(&t, 1.0f);
    return t;
}

static buf_t scene(uint32_t version, float tol, float far_tol) {
    static const uint8_t header[16] = { 1, 2, 3, 4 };
    static const uint8_t other[8] = { 0xAA, 0xBB };
    buf_t t = terrain_bytes(version, tol, far_tol);
    section_t s[3] = { { "Header", header, sizeof header }, { PATCH117_TERRAIN_SECTION, t.data, (uint32_t)t.size },
                       { "Terrain", other, sizeof other } };
    buf_t b = groff(s, 3);
    free(t.data);
    return b;
}

static float terrain_value(uint8_t* image, size_t size, int index) {
    uint32_t n = 0;
    uint8_t* t = patch117_groff_section(image, size, PATCH117_TERRAIN_SECTION, &n);
    float v = -1.0f;
    if (t && n >= 4u * (index + 1)) memcpy(&v, t + 4 * index, 4);
    return v;
}

static const patch117_level_t* level(const char* id) {
    for (int i = 0; i < PATCH117_LEVEL_COUNT; i++)
        if (!strcmp(patch117_levels[i].id, id)) return &patch117_levels[i];
    return NULL;
}

static void test_level_table(void) {
    for (int i = 0; i < PATCH117_LEVEL_COUNT; i++) {
        const patch117_level_t* l = &patch117_levels[i];
        CHECK(strlen(l->sha256) == 64 && l->size > 0, "%s fingerprint", l->file);
        CHECK(l->pixel_tol.retail == 8.0f && l->pixel_tol.official == 10.0f, "%s TrrPixelTol 8 -> 10", l->file);
        CHECK(l->pixel_tol_far.retail == 20.0f && l->pixel_tol_far.official == 50.0f, "%s TrrPixelTolFar 20 -> 50", l->file);
        for (int j = 0; j < i; j++) CHECK(_stricmp(l->file, patch117_levels[j].file), "%s listed twice", l->file);
    }
    CHECK(patch117_level_named("be.scn") == level("BE"), "plain name");
    CHECK(patch117_level_named("data\\BE.SCN") == level("BE"), "case and folder");
    CHECK(patch117_level_named("C:/Games/Trespasser/data/IT.scn") == level("IT"), "IT ships as IT.scn");
    CHECK(patch117_level_named("data\\as2.scn") == level("AS2") && patch117_level_named("as.scn") == level("AS"), "as/as2");
    CHECK(!patch117_level_named("TestScene.scn") && !patch117_level_named("be.grf") && !patch117_level_named("abe.scn") &&
              !patch117_level_named("SAVEGAME.000") && !patch117_level_named("be.scn.bak") && !patch117_level_named(NULL),
          "only the eight level scenes");
}

static void test_fix_terrain(void) {
    const patch117_level_t* be = level("BE");
    buf_t b = scene(1, 8.0f, 20.0f);
    uint8_t* before = (uint8_t*)malloc(b.size);
    uint32_t n = 0;
    uint8_t* t;
    size_t differ = 0;
    memcpy(before, b.data, b.size);
    t = patch117_groff_section(b.data, b.size, PATCH117_TERRAIN_SECTION, &n);
    CHECK(t && n == 72, "Terrain section found by its symbol");
    CHECK(patch117_fix_scene(be, b.data, b.size) == 2, "two terrain values fixed");
    CHECK(terrain_value(b.data, b.size, 1) == 10.0f && terrain_value(b.data, b.size, 2) == 50.0f, "official values");
    for (size_t i = 0; i < b.size; i++) differ += b.data[i] != before[i];
    CHECK(differ > 0 && differ <= 8, "only the two values change (%zu bytes)", differ);
    CHECK(terrain_value(b.data, b.size, 3) == 8.0f && terrain_value(b.data, b.size, 10) == 230.0f, "the rest is kept");
    memcpy(before, b.data, b.size);
    CHECK(patch117_fix_scene(be, b.data, b.size) == -1 && !memcmp(before, b.data, b.size),
          "an already fixed image is left alone: applied once");
    free(before);
    free(b.data);
}

static void test_modified_scenes(void) {
    const patch117_level_t* be = level("BE");
    struct {
        uint32_t version;
        float tol, far_tol;
        const char* what;
    } cases[] = { { 1, 9.0f, 20.0f, "a changed TrrPixelTol" }, { 1, 8.0f, 30.0f, "a changed TrrPixelTolFar" },
                  { 2, 8.0f, 20.0f, "another section version" }, { 1, 10.0f, 50.0f, "the official values" } };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        buf_t b = scene(cases[i].version, cases[i].tol, cases[i].far_tol);
        uint8_t* copy = (uint8_t*)malloc(b.size);
        memcpy(copy, b.data, b.size);
        CHECK(patch117_fix_scene(be, b.data, b.size) == -1 && !memcmp(copy, b.data, b.size), "%s is not fixed", cases[i].what);
        free(copy);
        free(b.data);
    }
    {
        static const uint8_t header[4] = { 0 };
        section_t only[1] = { { "Header", header, sizeof header } };
        buf_t b = groff(only, 1);
        CHECK(patch117_fix_scene(be, b.data, b.size) == -1, "no Terrain section");
        free(b.data);
    }
    {
        buf_t b = scene(1, 8.0f, 20.0f);
        uint8_t* copy = (uint8_t*)malloc(b.size);
        uint32_t n = 0;
        size_t needed = (size_t)(patch117_groff_section(b.data, b.size, PATCH117_TERRAIN_SECTION, &n) - b.data) + n;
        memcpy(copy, b.data, b.size);
        for (size_t cut = 0; cut < b.size; cut++) {
            uint8_t* image = (uint8_t*)malloc(cut ? cut : 1);
            int r;
            memcpy(image, copy, cut);
            r = patch117_fix_scene(be, image, cut);
            CHECK(cut < needed ? r == -1 : r == -1 || r == 2, "truncated image (%zu bytes): %d", cut, r);
            free(image);
        }
        free(copy);
        b.data[0] ^= 1;
        CHECK(patch117_fix_scene(be, b.data, b.size) == -1, "not a GROFF");
        CHECK(patch117_fix_scene(NULL, b.data, b.size) == -1 && patch117_fix_scene(be, NULL, 0) == -1, "no level, no image");
        free(b.data);
    }
}


/* The retail recursion (0x00475F60) over a small tree, with the flag each nested call gets decided by the policy. */
typedef struct node {
    int priority, can_have_children, lo, hi; /* contains [lo, hi] */
    struct node *child, *next, *parent;
} node_t;

static int g_fixes_on, g_depth, g_caller_flag;

static int add_child(node_t* self, node_t* item, int own_flag) {
    int caller = g_caller_flag, flag = patch117_add_child_flag(g_fixes_on, g_depth > 0, caller, own_flag), ok = 0;
    if (!self->can_have_children || self == item) return 0;
    if (flag && self->priority > item->priority) return 0;
    if (item->lo < self->lo || item->hi > self->hi) return 0;
    g_caller_flag = flag;
    g_depth++;
    for (node_t* c = self->child; c && !ok; c = c->next) ok = add_child(c, item, 0);
    g_depth--;
    g_caller_flag = caller;
    if (!ok) item->parent = self;
    return 1;
}

static void test_add_child(void) {
    CHECK(!patch117_add_child_flag(0, 1, 1, 0) && patch117_add_child_flag(0, 0, 0, 1), "Off: every call keeps its own flag");
    CHECK(patch117_add_child_flag(1, 1, 1, 0) && !patch117_add_child_flag(1, 1, 0, 1), "On: nested calls take the caller's");
    CHECK(patch117_add_child_flag(1, 0, 0, 1) && !patch117_add_child_flag(1, 0, 1, 0), "On: the top call keeps its own");
    for (int on = 0; on <= 1; on++) {
        node_t root = { 0, 1, 0, 100 }, detail = { 3, 1, 10, 20 }, moved = { 0, 0, 12, 14 };
        root.child = &detail;
        g_fixes_on = on;
        g_depth = 0;
        CHECK(add_child(&root, &moved, 1) == 1, "moved object placed");
        CHECK(moved.parent == (on ? &root : &detail),
              "%s: the moved object goes under %s", on ? "On" : "Off", on ? "the root" : "the priority-3 partition");
        moved.parent = NULL;
        CHECK(add_child(&root, &moved, 0) == 1 && moved.parent == &detail, "loading (flag 0) is unchanged");
    }
}

/* Frames: h = the jump key's hit frame, d = held, - = up. Physics steps after them: G = the foot test passes, A = it
 * fails (airborne). Returns the number of jumps. */
static int jumps(int on, const char* script) {
    patch117_jump_t j = { 0 };
    int n = 0;
    for (const char* c = script; *c; c++) {
        if (*c == 'G' || *c == 'A')
            n += patch117_jump_step(on, &j, 1, *c == 'G');
        else
            patch117_jump_frame(on, &j, patch117_jump_request(on, *c == 'h', *c == 'h' || *c == 'd'));
    }
    return n;
}

static char* repeat(char* out, const char* head, const char* unit, int count, const char* tail) {
    strcpy(out, head);
    for (int i = 0; i < count; i++) strcat(out, unit);
    strcat(out, tail);
    return out;
}

static void test_jump(void) {
    static char s[512];
    patch117_jump_request_t r;
    CHECK(jumps(0, "hA-G") == 0 && jumps(1, "hA-G") == 1, "a press between two steps: Off loses it, On jumps");
    CHECK(jumps(0, "h-G") == 0 && jumps(1, "h-G") == 1, "a press in a frame with no step: Off loses it, On jumps");
    CHECK(jumps(0, "hG") == 1 && jumps(1, "hG") == 1, "a press on the ground jumps either way");
    CHECK(jumps(0, repeat(s, "h", "A", 24, "G")) == 1 && jumps(1, s) == 1, "24 failed checks in one frame");
    CHECK(jumps(0, repeat(s, "h", "A", 25, "G")) == 1 && jumps(1, s) == 0, "On: the request expires after 25");
    CHECK(jumps(0, repeat(s, "h", "-AA", 10, "-G")) == 0 && jumps(1, s) == 1, "On: 20 steps before landing, released");
    CHECK(jumps(0, repeat(s, "h", "-AA", 13, "-G")) == 0 && jumps(1, s) == 0, "On: 26 steps before landing expires");
    CHECK(jumps(0, repeat(s, "hG", "dAA", 30, "dG")) == 1 && jumps(1, s) == 2, "held through a flight: On jumps on landing");
    CHECK(jumps(0, "-G-G") == 0 && jumps(1, "-G-G") == 0, "no key, no jump");
    r = patch117_jump_request(0, 0, 1);
    CHECK(r.amount == 0.0f, "Off: a held key asks nothing");
    r = patch117_jump_request(1, 0, 1);
    CHECK(r.amount == 0.9f && !r.is_new, "On: a held key asks without the new-press mark");
    r = patch117_jump_request(1, 1, 1);
    CHECK(r.amount == 0.9f && r.is_new, "the hit frame is a new press");
    {
        patch117_jump_t j = { 0 };
        patch117_jump_impulse(1, &j, 1.0f, 2.0f, 3.0f);
        patch117_jump_frame(1, &j, patch117_jump_request(1, 0, 1));
        CHECK(j.z == 3.0f && !j.voluntary && j.ttl == 25, "On: a held key does not replace a pending impulse");
        patch117_jump_frame(1, &j, patch117_jump_request(1, 1, 1));
        CHECK(j.z == 1.0f && j.x == 0.0f && j.voluntary, "a new press does");
        patch117_jump_frame(0, &j, patch117_jump_request(0, 0, 0));
        CHECK(j.z == 0.0f && j.voluntary, "Off: a frame without a press clears it, voluntary kept");
    }
}

/* The retail face tests of one corner (0x004EB2B9..0x004EC586): each face applies only to a corner within the skin,
 * so a corner deeper than eps behind every face gets no force. */
static int retail_faces(const float c[3], const float h[3], patch117_face_t out[6]) {
    static const int axis[3] = { 2, 0, 1 };
    int n = 0;
    for (int q = 0; q < 3; q++)
        for (int s = 1; s >= -1; s -= 2) {
            int a = axis[q], b = (a + 1) % 3, e = (a + 2) % 3;
            float d = (float)(((double)s * c[a] - h[a]) - PATCH117_CONTACT_EPS);
            if (d <= 0.0f && -PATCH117_CONTACT_EPS < d && fabs(c[b]) < PATCH117_CONTACT_EPS + h[b] &&
                fabs(c[e]) < PATCH117_CONTACT_EPS + h[e]) {
                out[n].axis = a;
                out[n].sign = s;
                out[n].d = d;
                out[n].w = 1.0f;
                n++;
            }
        }
    return n;
}

/* Every face a corner gets: the retail ones, and with the fix the deep push-out. */
static int corner_faces(int on, const float c[3], const float h[3], patch117_face_t out[9]) {
    int n = retail_faces(c, h, out);
    return n + (on ? patch117_deep_corner(c, h, out + n) : 0);
}

static int near_f(float a, float b) { return fabs((double)a - b) < 1e-5; }

/* The velocity kill (0x004E85A0) with an identity box rotation: n = the face normal it is given. */
static void kill(const float n[3], float self[3], float other[3]) {
    double a = (double)n[0] * self[0] + (double)n[1] * self[1] + (double)n[2] * self[2];
    if (a > 0)
        for (int i = 0; i < 3; i++) self[i] = (float)(self[i] - a * n[i]);
    a = (double)n[0] * other[0] + (double)n[1] * other[1] + (double)n[2] * other[2];
    if (a < 0)
        for (int i = 0; i < 3; i++) other[i] = (float)(other[i] - a * n[i]);
}

static void test_box_contact(void) {
    static const float unit[3] = { 1.0f, 1.0f, 1.0f }, still[3] = { 0.0f, 0.0f, 0.0f };
    patch117_contact_coeffs_t k = { 1.0f, 1.0f, 1.0f, 1.0f };
    patch117_face_t f[9];
    {
        static const float c[3] = { 0.5f, 0.2f, 0.1f };
        static const float w[3] = { 0.182574f, 0.912871f, 0.365148f }, force[3] = { 0.023004f, 0.115022f, 0.046009f };
        static const int axis[3] = { 2, 0, 1 }, code[3] = { 0, 2, 4 };
        CHECK(corner_faces(0, c, unit, f) == 0, "Off: a corner deep inside gets no force");
        CHECK(corner_faces(1, c, unit, f) == 3, "On: it is pushed out through three faces");
        for (int i = 0; i < 3; i++) {
            patch117_face_force_t r = patch117_face_force(&f[i], still, &k);
            CHECK(f[i].axis == axis[i] && f[i].sign == 1 && f[i].d == -PATCH117_CONTACT_EPS && near_f(f[i].w, w[i]),
                  "deep face %d: axis %d weight %f", i, f[i].axis, f[i].w);
            CHECK(near_f(r.f[axis[i]], force[i]) && patch117_face_code(&f[i]) == code[i], "deep face %d force %f", i,
                  r.f[axis[i]]);
        }
    }
    {
        static const float c[3] = { 0.5f, 0.5f, 0.05f }, h[3] = { 1.0f, 1.0f, 0.25f };
        patch117_face_force_t r;
        CHECK(corner_faces(0, c, h, f) == 0, "Off: a stacked box sunk 0.2 m into a flat one falls through");
        CHECK(corner_faces(1, c, h, f) == 1 && f[0].axis == 2 && f[0].w == 1.0f, "On: it is pushed up");
        r = patch117_face_force(&f[0], still, &k);
        CHECK(near_f(r.f[2], 0.126f) && r.f[0] == 0.0f && r.f[1] == 0.0f, "upward force %f", r.f[2]);
    }
    {
        static const float resting[3] = { 0.5f, 0.2f, 1.05f }, edge[3] = { 1.03f, 0.2f, 1.05f }, apart[3] = { 1.2f, 0.0f, 0.0f };
        patch117_face_t off[9];
        CHECK(corner_faces(1, resting, unit, f) == 1 && corner_faces(0, resting, unit, off) == 1 && f[0].axis == 2 &&
                  near_f(f[0].d, -0.04f),
              "a resting corner: the same single face either way");
        CHECK(corner_faces(1, edge, unit, f) == 2 && corner_faces(0, edge, unit, off) == 2 && !memcmp(f, off, sizeof f[0] * 2),
              "a corner near an edge: the same faces either way");
        CHECK(corner_faces(1, apart, unit, f) == 0 && corner_faces(0, apart, unit, off) == 0, "apart: nothing");
    }
    {
        patch117_face_t z = { 2, 1, -0.04f, 1.0f };
        static const float v[3] = { 0.5f, -0.25f, 2.0f };
        patch117_face_force_t r = patch117_face_force(&z, v, &k);
        CHECK(near_f(r.f[2], (float)((-(-0.04 * 1.4) - 2.0 * 0.25 * 1.4) * 1.0)), "separating corner: quarter damping");
        CHECK(near_f(r.f[0], -0.5f) && near_f(r.f[1], 0.25f), "friction against the sliding velocity");
        z.sign = -1;
        r = patch117_face_force(&z, v, &k);
        CHECK(near_f(r.f[2], (float)(-0.04 * 1.4 - 2.0 * 1.4)), "-z face approached: full damping");
    }
    {
        static const float quarter_turn_x[9] = { 1, 0, 0, 0, 0, -1, 0, 1, 0 };
        float n116[3] = { 0, 0, 1 }, n117[3] = { 0, 0, 1 };
        float self116[3] = { 0, -2, 0 }, other116[3] = { 0, 3, 0 }, self117[3] = { 0, -2, 0 }, other117[3] = { 0, 3, 0 };
        patch117_sub_box_rotate(quarter_turn_x, n117);
        CHECK(n117[0] == 0.0f && n117[1] == -1.0f && n117[2] == 0.0f, "On: the +z face of a turned sub-box points along -y");
        kill(n116, self116, other116);
        kill(n117, self117, other117);
        CHECK(self116[1] == -2.0f && other116[1] == 3.0f, "Off: the closing velocity along the real normal is kept");
        CHECK(self117[1] == 0.0f && other117[1] == 0.0f, "On: it is removed");
    }
    {
        static const float boxes[2][6] = { { 0.5f, 0.3f, 0.7f, 0, 0, 0 }, { 0.2f, 0.9f, 0.4f, 0, 0, 0 } };
        float m = patch117_min_half(&boxes[0][0], 6, 2);
        CHECK(m == 0.2f && patch117_min_half(&boxes[0][0], 6, 1) == 0.3f, "smallest half extent over the sub-boxes");
        CHECK(patch117_min_half(&boxes[0][0], 6, 0) == FLT_MAX, "no sub-box");
    }
    CHECK(!patch117_edge_contact(0, 0.5f, 0.2f, 3, 7) && !patch117_edge_contact(0, 0.2f, 0.5f, 7, 3),
          "Off: edge-edge contact never runs");
    CHECK(patch117_edge_contact(1, 0.5f, 0.2f, 7, 3) && !patch117_edge_contact(1, 0.2f, 0.5f, 3, 7),
          "On: from the box with the larger smallest half extent");
    CHECK(patch117_edge_contact(1, 0.4f, 0.4f, 3, 7) && !patch117_edge_contact(1, 0.4f, 0.4f, 7, 3),
          "On: a tie runs once, from the lower index");
    {
        patch117_wake_box_t a116 = { 1, 0, 5, 0, 9 }, b116 = { 0, 0, 0, 2, 4 }, a117 = a116, b117 = b116;
        patch117_wake_box_t c116 = a116, d116 = b116, c117 = a116, d117 = b116;
        patch117_pair_wake(0, 1, &a116, 3, &b116, 0, 3);
        patch117_pair_wake(1, 1, &a117, 3, &b117, 0, 3);
        CHECK(b116.state == 0 && b116.since == 0 && b116.active == 4, "Off: a pair that ignores collisions stays asleep");
        CHECK(b117.state == 3 && b117.since == 5 && b117.active == 9 && a117.active == 9, "On: the pair wakes together");
        patch117_pair_wake(0, 0, &c116, 3, &d116, 0, 3);
        patch117_pair_wake(1, 0, &c117, 3, &d117, 0, 3);
        CHECK(!memcmp(&c116, &c117, sizeof c116) && !memcmp(&d116, &d117, sizeof d116) && d117.state == 3,
              "a colliding pair wakes the same either way");
    }
}

/* A pick-up at `picked` pulls the object in to carry it once the hold time is over (0x0045FD30: now >= E2C). */
static int carried(int on, float picked, float now) { return now >= picked + patch117_pickup_hold_seconds(on); }

/* Raising an empty hand toward a wall keypad whose face is `wall` metres from the shoulder, straight ahead: the hand
 * target lands in front of the face, or inside the panel (the physics then drags the hand along it). */
static int hand_inside_panel(int on, float wall) { return patch117_empty_reach(on) > wall; }

static void test_hand(void) {
    int state;
    CHECK(patch117_grab_allowed(0, 2) && !patch117_grab_allowed(1, 2), "a hand still settling: Off grabs, On waits");
    CHECK(patch117_grab_allowed(0, 3) && patch117_grab_allowed(1, 3) && !patch117_grab_allowed(0, 1), "settled hand grabs");
    CHECK(patch117_hand_steered(0, 1) && !patch117_hand_steered(1, 1) && patch117_hand_steered(1, 2),
          "a rising hand: Off follows the mouse, On only once raised");
    CHECK(patch117_empty_reach(0) == 0.8f && patch117_empty_reach(1) == 0.7f, "empty-hand reach 0.8 -> 0.7 m");
    CHECK(patch117_pickup_hold_seconds(0) == 1.0f && patch117_pickup_hold_seconds(1) == 2.0f, "pick-up hold 1 -> 2 s");
    CHECK(carried(0, 10.0f, 11.5f) && !carried(1, 10.0f, 11.5f) && carried(1, 10.0f, 12.0f),
          "1.5 s after a pick-up: Off already drags the object in, On still holds it");
    CHECK(patch117_pickup_lift(0) == 0.0f && patch117_pickup_lift(1) == 0.1f, "picked-up object lifted 0.1 m at once");
    CHECK(!patch117_hand_settles(0.1, 0.5f), "On: a raise blocked by a crate keeps the hand tucked");
    CHECK(patch117_hand_settles(0.1, -100.0f) && patch117_hand_settles(0.1, -10.0f), "nothing on the way: it settles");
    CHECK(patch117_hand_settles(0.41, 0.5f) && patch117_hand_settles(1.0, 0.5f), "after 0.4 s it settles anyway");
    CHECK(hand_inside_panel(0, 0.75f) && !hand_inside_panel(1, 0.75f),
          "keypad 0.75 m away: Off aims the hand into the panel, On in front of the keys");
    CHECK(!hand_inside_panel(0, 0.85f) && !hand_inside_panel(1, 0.85f), "a keypad out of reach either way");
    CHECK(!patch117_hand_settles(0.0, 0.0f), "On: a raise into a wall panel at chest height is tucked, not pushed in");
    CHECK(patch117_held_hand_recentred(0, 1) && !patch117_held_hand_recentred(1, 1),
          "raising the hand with a gun: Off recentres the aim, On keeps it");
    CHECK(patch117_held_hand_recentred(0, 0) && patch117_held_hand_recentred(1, 0), "other held objects: kept in view");
    state = 2;
    CHECK(patch117_view_follows_hand(0, &state, 1) && state == 2, "Off: a settling hand drags the view");
    CHECK(!patch117_view_follows_hand(1, &state, 1) && state == 2, "On: it does not");
    CHECK(!patch117_view_follows_hand(1, &state, 0) && state == 3, "On: once in the view box the hand is settled");
    CHECK(patch117_view_follows_hand(1, &state, 1) && state == 3, "On: then the view follows it");
    CHECK(near_f(patch117_body_turn(0, 0.6981317f, 0.5235988f), 0.1745329f) &&
              near_f(patch117_body_turn(0, -0.6981317f, 0.5235988f), -0.1745329f),
          "Off: looking 40 deg aside turns the body 10 deg");
    CHECK(patch117_body_turn(1, 0.6981317f, 0.5235988f) == 0.6981317f, "On: the body turns onto the view");
}

static void test_fingerprint(void) {
    buf_t b = scene(1, 8.0f, 20.0f);
    CHECK(!patch117_identify("be.scn", b.data, b.size), "a file named be.scn with other contents is not the retail level");
    CHECK(!patch117_identify("be.scn", b.data, level("BE")->size > b.size ? b.size : 0), "wrong size");
    CHECK(!patch117_identify("mod.scn", b.data, b.size) && !patch117_identify(NULL, b.data, b.size), "unknown name");
    free(b.data);
}

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)n : 0;
    return data;
}

static const char* const k_disc_names[PATCH117_LEVEL_COUNT] = { "be.scn", "jr.scn", "ij.scn", "IT.scn",
                                                                "lab.scn", "as.scn", "as2.scn", "sum.scn" };

static void test_retail_scenes(void) {
    char path[MAX_PATH], official_path[MAX_PATH];
    const char* official_dir = getenv("JPT_PATCH117_DIR");
    int found = 0;
    for (int i = 0; i < PATCH117_LEVEL_COUNT; i++) {
        size_t size = 0, official_size = 0;
        uint8_t *data, *copy, *official;
        snprintf(path, sizeof path, "%s/game/data/%s", JPT_SOURCE_DIR, k_disc_names[i]);
        data = read_file(path, &size);
        if (!data) continue;
        found++;
        CHECK(patch117_identify(path, data, size) == &patch117_levels[i], "%s is the retail level", k_disc_names[i]);
        copy = (uint8_t*)malloc(size);
        memcpy(copy, data, size);
        copy[size / 2] ^= 0x40;
        CHECK(!patch117_identify(path, copy, size), "%s with one byte changed is not", k_disc_names[i]);
        free(copy);
        CHECK(patch117_fix_scene(&patch117_levels[i], data, size) == 2 + patch117_levels[i].presence_count, "%s fixed",
              k_disc_names[i]);
        CHECK(patch117_fix_scene(&patch117_levels[i], data, size) == -1, "%s fixed once", k_disc_names[i]);
        if (official_dir) {
            uint32_t a = 0, b = 0;
            uint8_t *ours, *theirs;
            snprintf(official_path, sizeof official_path, "%s/%s", official_dir, patch117_levels[i].file);
            official = read_file(official_path, &official_size);
            ours = patch117_groff_section(data, size, PATCH117_TERRAIN_SECTION, &a);
            theirs = official ? patch117_groff_section(official, official_size, PATCH117_TERRAIN_SECTION, &b) : NULL;
            CHECK(ours && theirs && a == b && !memcmp(ours, theirs, a), "%s Terrain section equals the official one",
                  k_disc_names[i]);
            for (int p = 0; p < patch117_levels[i].presence_count; p++) {
                const patch117_presence_t* m = &patch117_levels[i].presences[p];
                ours = patch117_groff_section(data, size, m->section, &a);
                theirs = official ? patch117_groff_section(official, official_size, m->section, &b) : NULL;
                CHECK(ours && theirs && a >= 32 && b >= 32 && !memcmp(ours, theirs, 32) && !memcmp(ours, m->official, 32),
                      "%s section %s holds the official placement", k_disc_names[i], m->section);
            }
            free(official);
        }
        free(data);
    }
    printf("retail scenes found: %d of %d%s\n", found, PATCH117_LEVEL_COUNT,
           official_dir ? " (compared with JPT_PATCH117_DIR)" : "");
}

static const patch117_world_t* world(const char* id) {
    for (int i = 0; i < PATCH117_WORLD_COUNT; i++)
        if (!strcmp(patch117_worlds[i].id, id)) return &patch117_worlds[i];
    return NULL;
}

static void test_world_table(void) {
    for (int w = 0; w < PATCH117_WORLD_COUNT; w++) {
        const patch117_world_t* x = &patch117_worlds[w];
        CHECK(strlen(x->sha256) == 64 && x->size > 0 && x->expanded_size > x->size && x->count > 0, "%s fingerprint", x->file);
        for (int i = 0; i < x->count; i++) {
            const patch117_place_t* p = &x->places[i];
            float a[7], b[7];
            memcpy(a, p->retail, sizeof a);
            memcpy(b, p->official, sizeof b);
            CHECK(p->offset + 28 <= x->expanded_size && (!i || p->offset >= x->places[i - 1].offset + 44),
                  "%s placement %d inside the image, one per record", x->id, i);
            CHECK(a[0] == b[0] && a[1] == b[1] && a[2] != b[2] && !memcmp(a + 3, b + 3, 16),
                  "%s placement %d only moves the object up or down", x->id, i);
        }
    }
    CHECK(patch117_world_named("data\\IJ.GRF") == world("IJ") && patch117_world_named("it.grf") == world("IT"), "names");
    CHECK(!patch117_world_named("be.grf") && !patch117_world_named("ij.scn") && !patch117_world_named(NULL),
          "only the worlds with placements");
    for (int i = 0; i < PATCH117_LEVEL_COUNT; i++)
        CHECK(!strcmp(patch117_levels[i].id, "AS") ? patch117_levels[i].presence_count == 1 : !patch117_levels[i].presence_count,
              "%s scene placements", patch117_levels[i].id);
}

static void test_fix_world(void) {
    const patch117_world_t* it = world("IT");
    uint8_t* image = (uint8_t*)calloc(it->expanded_size, 1);
    uint8_t* before = (uint8_t*)malloc(it->expanded_size);
    size_t differ = 0;
    for (int i = 0; i < it->count; i++) memcpy(image + it->places[i].offset, it->places[i].retail, 28);
    memcpy(before, image, it->expanded_size);
    CHECK(patch117_fix_world(it, image, it->expanded_size - 1) == -1, "wrong size");
    CHECK(patch117_fix_world(it, image, it->expanded_size) == it->count, "every placement written");
    for (size_t i = 0; i < it->expanded_size; i++) differ += image[i] != before[i];
    CHECK(differ > 0 && differ <= (size_t)it->count * 28, "only the placements change (%zu bytes)", differ);
    for (int i = 0; i < it->count; i++)
        CHECK(!memcmp(image + it->places[i].offset, it->places[i].official, 28), "placement %d is the official value", i);
    memcpy(before, image, it->expanded_size);
    CHECK(patch117_fix_world(it, image, it->expanded_size) == -1 && !memcmp(before, image, it->expanded_size),
          "an already fixed image is left alone: applied once");
    for (int i = 0; i < it->count; i++) memcpy(image + it->places[i].offset, it->places[i].retail, 28);
    image[it->places[it->count - 1].offset + 3] ^= 1;
    memcpy(before, image, it->expanded_size);
    CHECK(patch117_fix_world(it, image, it->expanded_size) == -1 && !memcmp(before, image, it->expanded_size),
          "a modified object is never written, nor the others");
    free(image);
    free(before);
}

static void test_presences(void) {
    const patch117_level_t* as = level("AS");
    const patch117_presence_t* rifle = &as->presences[0];
    uint8_t body[36];
    buf_t t = terrain_bytes(1, 8.0f, 20.0f);
    section_t s[2];
    buf_t b;
    uint8_t* before;
    uint32_t n = 0;
    uint8_t* at;
    memcpy(body, rifle->retail, 32);
    memset(body + 32, 0x5A, 4);
    s[0].name = PATCH117_TERRAIN_SECTION;
    s[0].bytes = t.data;
    s[0].size = (uint32_t)t.size;
    s[1].name = rifle->section;
    s[1].bytes = body;
    s[1].size = sizeof body;
    b = groff(s, 2);
    before = (uint8_t*)malloc(b.size);
    CHECK(patch117_fix_scene(as, b.data, b.size) == 3, "AS: terrain and the rifle");
    at = patch117_groff_section(b.data, b.size, rifle->section, &n);
    CHECK(at && n == 36 && !memcmp(at, rifle->official, 32) && at[32] == 0x5A, "the official placement, the rest kept");
    memcpy(before, b.data, b.size);
    CHECK(patch117_fix_scene(as, b.data, b.size) == -1 && !memcmp(before, b.data, b.size), "applied once");
    free(b.data);
    body[0] ^= 1;
    b = groff(s, 2);
    memcpy(before, b.data, b.size);
    CHECK(patch117_fix_scene(as, b.data, b.size) == -1 && !memcmp(before, b.data, b.size),
          "a moved rifle leaves the whole scene retail");
    free(b.data);
    free(before);
    free(t.data);
}

/* The bytes LZRead gives the game for a file: SZDD expanded. */
static uint8_t* expand(const char* path, size_t* size) {
    OFSTRUCT opened;
    INT h = LZOpenFileA((LPSTR)path, &opened, OF_READ);
    LONG n = h >= 0 ? LZSeek(h, 0, 2) : -1;
    uint8_t* data = n > 0 && LZSeek(h, 0, 0) == 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (data && LZRead(h, (LPSTR)data, (INT)n) != (INT)n) {
        free(data);
        data = NULL;
    }
    if (h >= 0) LZClose(h);
    *size = data ? (size_t)n : 0;
    return data;
}

static const uint8_t* find(const uint8_t* hay, size_t n, const uint8_t* needle, size_t m) {
    for (size_t i = 0; i + m <= n; i++)
        if (hay[i] == needle[0] && !memcmp(hay + i, needle, m)) return hay + i;
    return NULL;
}

/* JPT_RETAIL_DIR: a folder with the disc's ij.GRF/it.GRF (default game/data); JPT_PATCH117_DIR: the patch's levels. */
static void test_retail_worlds(void) {
    const char* dir = getenv("JPT_RETAIL_DIR");
    const char* official_dir = getenv("JPT_PATCH117_DIR");
    char path[MAX_PATH], default_dir[MAX_PATH];
    int found = 0;
    snprintf(default_dir, sizeof default_dir, "%s/game/data", JPT_SOURCE_DIR);
    for (int w = 0; w < PATCH117_WORLD_COUNT; w++) {
        const patch117_world_t* x = &patch117_worlds[w];
        size_t size = 0, expanded = 0, official_size = 0;
        uint8_t *raw, *image, *official;
        snprintf(path, sizeof path, "%s/%s", dir ? dir : default_dir, x->file);
        raw = read_file(path, &size);
        if (!raw) continue;
        found++;
        CHECK(patch117_world_identify(path, raw, size) == x, "%s is the retail world", x->file);
        raw[size / 2] ^= 0x40;
        CHECK(!patch117_world_identify(path, raw, size), "%s with one byte changed is not", x->file);
        free(raw);
        image = expand(path, &expanded);
        CHECK(image && expanded == x->expanded_size, "%s expands to %u bytes", x->file, x->expanded_size);
        if (!image || expanded != x->expanded_size) {
            free(image);
            continue;
        }
        CHECK(patch117_fix_world(x, image, expanded) == x->count, "%s placements", x->file);
        CHECK(patch117_fix_world(x, image, expanded) == -1, "%s placed once", x->file);
        if (official_dir) {
            snprintf(path, sizeof path, "%s/%s", official_dir, x->file);
            official = expand(path, &official_size);
            CHECK(official != NULL, "%s official world file read", x->id);
            for (int i = 0; official && i < x->count; i++)
                CHECK(find(official, official_size, x->places[i].official, 28) != NULL,
                      "%s placement %d is in the official world file", x->id, i);
            free(official);
        }
        free(image);
    }
    printf("retail worlds found: %d of %d%s\n", found, PATCH117_WORLD_COUNT,
           official_dir ? " (compared with JPT_PATCH117_DIR)" : "");
}

static void test_setting(void) {
    jpt_config_t c;
    char dir[MAX_PATH], path[MAX_PATH];
    FILE* f;
    jpt_config_defaults(&c);
    CHECK(c.official_patch_117 == 1, "On by default");
    CHECK(!strcmp(jpt_default_value(JPT_KEY_OFFICIAL_PATCH_117), "1"), "Reset all settings gives On");
    CHECK(jpt_config_apply(&c, JPT_KEY_OFFICIAL_PATCH_117, "off") && !c.official_patch_117, "Off");
    CHECK(jpt_config_apply(&c, JPT_KEY_OFFICIAL_PATCH_117, "1") && c.official_patch_117, "On");
    CHECK(!jpt_config_apply(&c, JPT_KEY_OFFICIAL_PATCH_117, "maybe") && c.official_patch_117, "invalid value ignored");
    GetTempPathA(sizeof dir, dir);
    snprintf(path, sizeof path, "%sjpt_patch117_%lu.ini", dir, GetCurrentProcessId());
    f = fopen(path, "w");
    if (f) {
        fputs("settings_version = 3\ndual_stow = 0\n", f);
        fclose(f);
    }
    CHECK(jpt_config_load(path, &c) && c.official_patch_117 && !c.dual_stow, "an older file without the key gets On");
    f = fopen(path, "w");
    if (f) {
        fputs("official_patch_117 = 0\n", f);
        fclose(f);
    }
    CHECK(jpt_config_load(path, &c) && !c.official_patch_117, "a saved Off is kept");
    DeleteFileA(path);
}

static char* read_source(const char* relative) {
    char path[MAX_PATH];
    size_t n = 0;
    char* text;
    uint8_t* data;
    snprintf(path, sizeof path, "%s/%s", JPT_SOURCE_DIR, relative);
    data = read_file(path, &n);
    if (!data) return NULL;
    text = (char*)realloc(data, n + 1);
    text[n] = 0;
    return text;
}

static void test_wiring(void) {
    char* launcher = read_source("launcher/launcher.cpp");
    char* pt = read_source("launcher/localization/pt_br_ui_text.cpp");
    char* main_c = read_source("src/host/main.c");
    char* io = read_source("src/host/io_trace.c");
    char* glue = read_source("src/host/patch117.c");
    char* overrides = read_source("src/host/guest_override_list.h");
    const char *ce, *heading, *toggle;
    CHECK(launcher && pt && main_c && io && glue, "sources readable");
    if (!launcher || !pt || !main_c || !io || !glue) return;
    ce = strstr(launcher, "ui.Heading(\"Trespasser CE\")");
    heading = strstr(launcher, "ui.Heading(\"Official Patch 1.1\")");
    toggle = strstr(launcher, "ui.Toggle(\"Patch 1.1 fixes\", kOfficialPatchHelp, JPT_KEY_OFFICIAL_PATCH_117, true, \"Off\", \"On\")");
    CHECK(ce && heading && toggle && ce < heading && heading < toggle, "Gameplay: its own section after Trespasser CE");
    CHECK(strstr(launcher, "\"Applies gameplay and level fixes from DreamWorks Interactive's official Trespasser v1.1 "
                           "build 117 patch\"") != NULL, "help text");
    CHECK(strstr(pt, "{\"Official Patch 1.1\", \"Patch oficial 1.1\"}") && strstr(pt, "{\"Patch 1.1 fixes\", "
                                                                                     "\"Correções do Patch 1.1\"}"),
          "Portuguese labels");
    CHECK(strstr(pt, "\"Aplica correções de jogabilidade e níveis do patch oficial Trespasser v1.1 build 117 da "
                     "DreamWorks Interactive\"") != NULL, "Portuguese help");
    CHECK(strstr(main_c, "patch117_configure(cfg.official_patch_117);") && strstr(main_c, "{ \"LZRead\", shim_LZRead }") &&
              strstr(main_c, "{ \"LZSeek\", shim_LZSeek }") && strstr(main_c, "{ \"LZClose\", shim_LZClose }"),
          "host: setting and the game's LZ reads");
    CHECK(strstr(io, "patch117_lz_opened(") != NULL, "every LZOpenFileA reaches the level check");
    CHECK(strstr(glue, "\"official Patch 1.1 fixes: %s\"") && strstr(glue, "\"applied %d official level fixes to %s\""),
          "log lines");
    CHECK(overrides && strstr(overrides, "GUEST_OVERRIDE(00475F60, REQUIRED)") &&
              strstr(overrides, "00472B60") < strstr(overrides, "00475F60") &&
              strstr(overrides, "00475F60") < strstr(overrides, "00477A40"),
          "bAddChild listed in address order");
    CHECK(strstr(glue, "patch117_add_child_flag(s_on, s_add_child_depth > 0") && strstr(main_c, "patch117_install();"),
          "bAddChild wrapper installed");
    CHECK(!strstr(glue, "Decomp_Downloads") && !strstr(glue, "Patch Dir") && !strstr(glue, "RegOpenKey"),
          "no dependency on the patch's files, folder or registry");
    free(launcher);
    free(pt);
    free(main_c);
    free(io);
    free(glue);
    free(overrides);
}

/* Every lift hook the list names is defined once by the gameplay glue, and starts by checking the setting. */
static void test_hook_wiring(void) {
    char* list = read_source("src/host/lift_hook_list.h");
    char* player = read_source("src/host/patch117_player.c");
    char* physics = read_source("src/host/patch117_physics.c");
    char* main_c = read_source("src/host/main.c");
    char* overrides = read_source("src/host/guest_override_list.h");
    int hooks = 0;
    CHECK(list && player && physics && main_c && overrides, "sources readable");
    if (!list || !player || !physics || !main_c || !overrides) return;
    for (const char* p = strstr(list, "\nLIFT_HOOK("); p; p = strstr(p + 1, "\nLIFT_HOOK(")) {
        char name[64];
        const char *a, *b;
        snprintf(name, sizeof name, "void lift_hook_%.8s(void)", p + 11);
        a = strstr(player, name);
        b = strstr(physics, name);
        CHECK((a != NULL) != (b != NULL), "%s defined once", name);
        hooks++;
    }
    CHECK(hooks == 27, "27 lift hooks (%d)", hooks);
    CHECK(strstr(overrides, "GUEST_OVERRIDE(004EDFA0, REQUIRED)") && strstr(overrides, "GUEST_OVERRIDE(004F0960, REQUIRED)"),
          "box set-up and jump hand-off overrides listed");
    CHECK(strstr(main_c, "patch117_player_configure(cfg.official_patch_117);") &&
              strstr(main_c, "patch117_physics_configure(cfg.official_patch_117);") &&
              strstr(main_c, "patch117_player_install();") && strstr(main_c, "patch117_physics_install();"),
          "player and physics fixes follow the setting");
    CHECK(!strstr(player, "jpt_config") && !strstr(physics, "jpt_config") && !strstr(player, "mouse_sens") &&
              !strstr(player, "fov"),
          "the fixes touch no input or view setting");
    free(list);
    free(player);
    free(physics);
    free(main_c);
    free(overrides);
}

int main(void) {
    test_level_table();
    test_fix_terrain();
    test_modified_scenes();
    test_fingerprint();
    test_add_child();
    test_jump();
    test_box_contact();
    test_hand();
    test_retail_scenes();
    test_world_table();
    test_fix_world();
    test_presences();
    test_retail_worlds();
    test_setting();
    test_wiring();
    test_hook_wiring();
    printf("patch117_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

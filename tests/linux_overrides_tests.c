/* The host's overrides and lift hooks of the lifted game on Linux: the same stable entries, registry and feature sources the Windows
 * host builds (guest_override_list.h, lift_hook_list.h, guest_features.h), over a lift whose bodies are recorded stubs and a guest
 * window of fake memory. Installation (fingerprint, unknown build, duplicates, rollback), the call forms (replace, around, nested,
 * original call, threaded) and what each shared feature does to guest memory through the Linux guest pointers. Headless. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "active_profile.h"
#include "ce_gameplay.h"
#include "fov_policy.h"
#include "guest_abi.h"
#include "guest_features.h"
#include "guest_memory.h"
#include "guest_ptr.h"
#include "jpt_config.h"
#include "guest_thunks.h"
#include "host_policy.h"
#include "jpt_fs.h"
#include "jpt_game_version.h"
#include "jpt_user_data.h"
#include "jpt_vpath.h"
#include "linux_advapi32.h"
#include "linux_card.h"
#include "linux_d3d.h"
#include "linux_features.h"
#include "linux_hostkeys.h"
#include "linux_hle.h"
#include "linux_log.h"
#include "jpt_l10n.h"
#include "l10n.h"
#include "linux_overrides.h"
#include "linux_spine.h"
#include "linux_text.h"
#include "override_registry.h"
#include "recomp_types.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

/* The lifted side: every listed function is a stub that records its entry and lets a test give it a body. */
static uint32_t g_called[256];
static unsigned g_called_count;
static void (*g_body)(uint32_t va);
static uint32_t g_ecx_seen, g_arg_seen[4];

static void lifted_stub(uint32_t va) {
    if (g_called_count < 256) g_called[g_called_count] = va;
    g_called_count++;
    g_ecx_seen = g_ecx;
    for (int i = 0; i < 4; i++) g_arg_seen[i] = MEM32(g_esp + 4 + 4 * i);
    if (g_body) g_body(va);
}

#define OVERRIDE_LIFT lift116_
#define GUEST_OVERRIDE(va, need) void lift116_lifted_sub_##va(void) { lifted_stub(0x##va##u); }
#include "guest_override_list.h"
#undef GUEST_OVERRIDE
#define OVERRIDE_LIST "guest_override_list.h"
#include "guest_override_lift.inc"

void lx_lift116_register_overrides(void) { lx_overrides_set_lift_registry(&lift116_registry, JPT_LIFT_116); }

static void reset_calls(void) {
    g_called_count = 0;
    g_body = NULL;
}

static int called(uint32_t va) {
    for (unsigned i = 0; i < g_called_count && i < 256; i++)
        if (g_called[i] == va) return 1;
    return 0;
}

static int installed(uint32_t va) {
    override_entry_t* e = override_registry_find(&lift116_registry, va);
    return e && e->state == OVERRIDE_INSTALLED;
}

static void install_config(jpt_config_t* c) {
    jpt_config_defaults(c);
    c->active_ai_enhanced = 1;
    c->dual_stow = 1;
    c->max_object_detail = 1;
    c->official_patch_117 = 1;
    c->fov = 90;
}

static void write32(uint32_t va, uint32_t v) { gp_write_u32(va, v); }

static uint32_t read32(uint32_t va) {
    uint32_t v = 0;
    gp_read_u32(va, &v);
    return v;
}

static void writef(uint32_t va, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    gp_write_u32(va, bits);
}

static float readf(uint32_t va) {
    uint32_t bits = read32(va);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static lx_thread_t* s_thread;

typedef void (*body_fn)(void);

typedef struct {
    body_fn fn;
} body_t;

static void trampoline(void* arg) { ((body_t*)arg)->fn(); }

static void run_on_guest(body_fn fn) {
    body_t b;
    lx_status_t status;
    b.fn = fn;
    status = lx_run(trampoline, &b);
    CHECK(status == LX_OK, "the guest body ran to its end (%d)", (int)status);
}

/* ---- the tables ---------------------------------------------------------------------------------------------------- */

static void test_tables(void) {
    CHECK(override_registry_valid(&lift116_registry), "the override list is sorted, unique and every stable entry points at its lifted body");
    CHECK(lx_lift116_override_count() == 78, "78 overrides are listed (%u)", lx_lift116_override_count());
    CHECK(lx_lift116_hook_count() == 27, "27 lift hooks are listed (%u)", lx_lift116_hook_count());
    CHECK(lift116_registry.count == lx_lift116_override_count(), "the registry holds the whole list");
}

/* ---- installation ---------------------------------------------------------------------------------------------------- */

static const uint32_t k_shared[] = {
    0x0040DA00, 0x00410540, 0x00411EA0, 0x00413040, 0x004142C0, 0x00415AD0, 0x004610D0, 0x00419330, 0x004195A0, 0x00419800, 0x004199D0, 0x00419C60, 0x00419CE0, 0x00419DB0, 0x00419E30,
    0x0041A200, 0x0041ADF0, 0x0041AE50, 0x0041AF00, 0x004213F0, 0x004218C0, 0x00418AE0, 0x00475F60, 0x0049E530, 0x004EDFA0, 0x004F0960, 0x004F3FB0,
    0x0051FD90, 0x0060FC10, 0x005F80B0, 0x005F8A20, 0x005F8B10, 0x00614F50,
};


/* F2 over gameplay: the hotkey reaches the host's overlay owner, the game's UI loop opens the controls card, a second F2 closes it,
 * and the card is the host's own (nothing is added to the game's text overlay). */
static void test_f2_card(void) {
    enum { VK_F2 = 0x71, WM_KEYDOWN = 0x100 };
    uint32_t region = gm_alloc_pages(0, 0x10000, "f2 game"), window = region, main = region + 0x100, manager = region + 0x200, list = region + 0x300;
    if (!gm_ptr_checked(0x0066E010u, 4, 1)) gm_alloc_pages(0x00660000u, 0x10000, "game state");
    gp_write_u32(window, 0x00645C3Cu);
    gp_write_u32(window + 0x18, 640);
    gp_write_u32(window + 0x1C, 480);
    gp_write_u32(list, window);
    gp_write_u32(main + 0x08, manager);
    gp_write_u32(manager + 0x20, 1);
    gp_write_u32(manager + 0x38, list);
    gp_write_u32(manager + 0x3C, list + 4);
    gp_write_u32(0x0066E010u, main);
    CHECK(!lx_card_shown(), "no controls card before F2");
    CHECK(lx_hostkeys_filter(WM_KEYDOWN, VK_F2, 0, 0, 0, 0) == 1, "F2 is a host key the game never sees");
    lx_host_ui_loop();
    CHECK(lx_card_shown(), "the game's UI loop opens the controls card for F2");
    CHECK(lx_hostkeys_filter(WM_KEYDOWN + 1, VK_F2, 0, 0, 0, 0) == 1, "F2's release is taken too");
    CHECK(lx_hostkeys_filter(WM_KEYDOWN, VK_F2, 0, 0, 0, 0) == 1, "a second F2");
    lx_host_ui_loop();
    CHECK(!lx_card_shown(), "closes the card");
    gm_release(region);
}

static void test_install(void) {
    jpt_config_t cfg;
    unsigned n = 0;
    override_counts_t c;
    install_config(&cfg);
    cfg.skip_intro = 1;
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "the host features install over lift116 without a required override missing");
    c = lx_overrides_counts();
    for (unsigned i = 0; i < sizeof k_shared / sizeof k_shared[0]; i++) {
        int ok = installed(k_shared[i]);
        CHECK(ok, "the shared override of sub_%08X is installed", k_shared[i]);
        n += (unsigned)ok;
    }
    CHECK(c.installed == n && c.required_refused == 0 && c.refused == 0, "%u installed, exactly the shared set (%u), no refusal (%u)", c.installed, n, c.refused);
    CHECK(c.installed + c.unused == c.listed, "what is not installed is unused, not failed (%u + %u of %u)", c.installed, c.unused, c.listed);
    for (unsigned i = 0; i < lift116_registry.count; i++)
        CHECK(lift116_registry.entries[i].state != OVERRIDE_FAILED, "sub_%08X did not fail", lift116_registry.entries[i].va);
    test_f2_card();
    lx_features_stop();
}

static int gpu_yes(void) { return 1; }
static unsigned gpu_generation(void) { return 0; }

/* With a GPU renderer the Direct3D path's hooks are part of the features; without one the lifted game keeps its functions. */
static void test_direct3d_hooks(void) {
    static const uint32_t hooks[] = { 0x00503400, 0x005242C0, 0x0055F540, 0x00563890, 0x005668C0, 0x00568EF0, 0x0057B7B0, 0x0057DB50, 0x0060E3F0 };
    static lx_gpu_ops_t ops;
    jpt_config_t cfg;
    override_counts_t c;
    install_config(&cfg);
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start with the Software renderer");
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) CHECK(!installed(hooks[i]), "sub_%08X stays the game's", hooks[i]);
    lx_features_stop();
    memset(&ops, 0, sizeof ops);
    ops.name = "test", ops.available = gpu_yes, ops.generation = gpu_generation;
    lx_d3d_set_gpu(&ops);
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start with a GPU renderer");
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) CHECK(installed(hooks[i]), "sub_%08X is hooked", hooks[i]);
    c = lx_overrides_counts();
    CHECK(c.refused == 0 && guest_overrides_check(), "no refusal (%u)", c.refused);
    lx_features_stop();
    lx_d3d_set_gpu(NULL);
}

/* The other lifts list the same functions at their own addresses, or the install above would refuse their run. */
static void test_direct3d_hooks_of_every_lift(void) {
    static const unsigned hooks[] = { GVA_ID(0x00503400), GVA_ID(0x005242C0), GVA_ID(0x0055F540), GVA_ID(0x00563890), GVA_ID(0x005668C0),
                                      GVA_ID(0x00568EF0), GVA_ID(0x0057B7B0), GVA_ID(0x0057DB50), GVA_ID(0x0060E3F0) };
    static const uint32_t es[] = {
#define GUEST_OVERRIDE(va, need) 0x##va##u,
#include "profiles/lift_es117/guest_override_list.h"
#undef GUEST_OVERRIDE
    };
    static const uint32_t ptbr[] = {
#define GUEST_OVERRIDE(va, need) 0x##va##u,
#include "profiles/lift_ptbr117/guest_override_list.h"
#undef GUEST_OVERRIDE
    };
    static const struct {
        jpt_lift_t lift;
        const uint32_t* list;
        size_t count;
    } k[] = { { JPT_LIFT_ES117, es, sizeof es / sizeof es[0] }, { JPT_LIFT_PTBR117, ptbr, sizeof ptbr / sizeof ptbr[0] } };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        for (size_t h = 0; h < sizeof hooks / sizeof hooks[0]; h++) {
            uint32_t va = jpt_lift_data[k[i].lift].gva[hooks[h]];
            int listed = 0;
            for (size_t n = 0; n < k[i].count; n++) listed |= k[i].list[n] == va;
            CHECK(va && listed, "lift %d lists sub_%08X (hook %u)", (int)k[i].lift, va, (unsigned)h);
        }
}

static void test_settings_choose(void) {
    jpt_config_t cfg;
    install_config(&cfg);
    cfg.active_ai_enhanced = 0;
    cfg.dual_stow = 0;
    cfg.official_patch_117 = 0;
    cfg.skip_intro = 0;
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start with the settings off");
    CHECK(!installed(0x004610D0), "Dual stow off: the stow handler stays the game's");
    CHECK(!installed(0x00418AE0), "Skip Intro off: Play stays the game's");
    CHECK(installed(0x0040DA00) && installed(0x0051FD90) && installed(0x004F3FB0), "the always-on features are installed");
    lx_features_stop();
}

static void test_fingerprints(void) {
    jpt_config_t cfg;
    static override_entry_t entries[1];
    static override_fn_t entry_slot;
    override_registry_t fresh;
    override_fn_t lifted_seen = NULL;
    install_config(&cfg);
    CHECK(!lx_features_start(&cfg, "game", "es-117", NULL, NULL), "the lift116 registry does not accept the Spanish build's addresses");
    CHECK(lx_overrides_counts().installed == 0, "no override is installed for an unknown build");
    lx_features_stop();
    CHECK(!lx_features_start(&cfg, "game", "no-such-build", NULL, NULL), "an unknown build name installs nothing");
    lx_features_stop();

    memset(&fresh, 0, sizeof fresh);
    entry_slot = lift116_lifted_sub_0040DA00;
    entries[0].va = 0x0040DA00, entries[0].entry = &entry_slot, entries[0].lifted = lift116_lifted_sub_0040DA00, entries[0].need = OVERRIDE_REQUIRED;
    fresh.entries = entries, fresh.count = 1;
    CHECK(!override_registry_install(&fresh, 0x0040DA00, lift116_lifted_sub_00410540, &lifted_seen) && fresh.refusals[0].why == OVERRIDE_REFUSED_UNVERIFIED,
          "an install before the target executable was verified is refused as unverified");
    CHECK(entry_slot == lift116_lifted_sub_0040DA00, "and the stable entry still runs the lifted body");
}

static void push_args(unsigned n, const uint32_t* a) {
    g_esp -= 4u * (n + 1);
    write32(g_esp, 0xDEAD0000u);
    for (unsigned i = 0; i < n; i++) write32(g_esp + 4 + 4 * i, a[i]);
}

static uint32_t s_esp0, s_result;

static void body_disk_space(void) {
    uint32_t args[1] = { 100 };
    s_esp0 = g_esp;
    push_args(1, args);
    lift116_sub_0040DA00();
    s_result = g_eax;
}

static void test_duplicates_and_rollback(void) {
    jpt_config_t cfg;
    recomp_func_t lifted = NULL;
    override_counts_t c;
    install_config(&cfg);
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start");
    CHECK(!guest_override_install(0x0040DA00, lift116_lifted_sub_00410540, &lifted), "a second override on the same function is refused (duplicate)");
    CHECK(!guest_override_install(0x00400001, lift116_lifted_sub_00410540, &lifted), "an unlisted address is refused");
    CHECK(!guest_override_install(0x00410540, NULL, &lifted), "no replacement is refused");
    CHECK(!guest_override_install(0x00410540, lift116_lifted_sub_00410540, &lifted), "a replacement that is the lifted body itself is refused");
    c = lx_overrides_counts();
    CHECK(c.refused >= 4, "every refusal is recorded (%u)", c.refused);
    CHECK(!guest_overrides_check(), "an unlisted address counts as a required override missing: the run is refused");
    reset_calls();
    run_on_guest(body_disk_space);
    CHECK(!called(0x0040DA00), "the installed replacement answers, the lifted body is not called");
    lx_features_stop();
    reset_calls();
    run_on_guest(body_disk_space);
    CHECK(called(0x0040DA00), "after the rollback the stable entry runs the lifted body again");
}

/* ---- call forms ---------------------------------------------------------------------------------------------------- */


static void test_replace_abi(void) {
    jpt_config_t cfg;
    install_config(&cfg);
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start");
    reset_calls();
    run_on_guest(body_disk_space);
    CHECK(s_result == 1 && !called(0x0040DA00), "the disk-space check answers 'enough' without the original (REPLACE): eax %u", s_result);
    lx_features_stop();
}

static void body_stable_entry_with_ecx(void) {
    g_ecx = 0x00A00044u;
    lift116_sub_004610D0();
}

static void test_ecx_reaches_the_lifted_body(void) {
    jpt_config_t cfg;
    install_config(&cfg);
    cfg.dual_stow = 0;
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start");
    reset_calls();
    run_on_guest(body_stable_entry_with_ecx);
    CHECK(called(0x004610D0) && g_ecx_seen == 0x00A00044u, "an entry nothing replaced reaches the lifted body with its this pointer (%08X)", g_ecx_seen);
    lx_features_stop();
}

/* ---- feature behaviour on the guest window ---------------------------------------------------------------------------- */

#define PROPS_SIZE 0x58u
#define PROPS_IN_CAMERA 0x74u

static uint32_t s_camera, s_props, s_seen_tan, s_seen_aspect, s_seen_w;

static void body_camera_set(uint32_t va) {
    uint32_t props = MEM32(g_esp + 4);
    (void)va;
    s_seen_w = read32(props);
    s_seen_tan = read32(props + 0x48);
    s_seen_aspect = read32(props + 0x4C);
}

static void body_set_properties(void) {
    uint32_t args[1];
    g_ecx = s_camera;
    args[0] = s_props;
    push_args(1, args);
    lift116_sub_0051FD90();
}

static void test_field_of_view(void) {
    jpt_config_t cfg;
    uint32_t holder = 0x00A10000u, world = 0x0073F444u;
    float tan_game = 0.75f, expected;
    install_config(&cfg);
    cfg.aspect = 0;
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start with a field of view of %d", cfg.fov);
    s_camera = 0x00A20000u;
    s_props = 0x00A30000u;
    write32(world, holder);
    write32(holder + 0x14, s_camera);
    writef(s_camera + PROPS_IN_CAMERA, 640.0f);
    writef(s_camera + PROPS_IN_CAMERA + 4, 480.0f);
    gp_write_u32(s_camera + PROPS_IN_CAMERA + 0x34, 1);
    writef(s_camera + PROPS_IN_CAMERA + 0x48, tan_game);
    writef(s_camera + PROPS_IN_CAMERA + 0x4C, 4.0f / 3.0f);
    writef(s_props, 640.0f);
    writef(s_props + 4, 480.0f);
    gp_write_u32(s_props + 0x34, 1);
    writef(s_props + 0x48, tan_game);
    writef(s_props + 0x4C, 4.0f / 3.0f);
    reset_calls();
    g_body = body_camera_set;
    run_on_guest(body_set_properties);
    expected = (float)fov_tan_half(tan_game, 90);
    CHECK(called(0x0051FD90), "the camera's SetProperties runs the original with the widened properties");
    {
        float seen;
        memcpy(&seen, &s_seen_tan, sizeof seen);
        CHECK(seen > tan_game * 1.01f && seen == expected, "the lifted body gets tan(half width) %.4f for a field of view of 90 (the game's %.4f)", (double)seen, (double)tan_game);
    }
    CHECK(readf(s_props + 0x48) == tan_game, "the caller's properties are put back: callers copy, edit and set them again");
    CHECK(readf(s_camera + PROPS_IN_CAMERA + 0x48) == tan_game, "and the camera keeps the game's own angle, savegames keep it");
    lx_features_stop();
}

#define SHADOW_CLUT 0x00A40000u
#define SHADOW_TABLE 0x00A50000u
#define SHADOW_FRAME_FROM_CAMERA 0x22Cu

static void body_terrain_update(void) {
    uint32_t args[2] = { 0, 0 };
    g_ecx = 0x00A60000u;
    push_args(2, args);
    lift116_sub_0049E530();
}

static uint32_t pixel_at(uint32_t row, uint32_t entry) { return read32(SHADOW_TABLE + (row * 256u + entry) * 2u) & 0xFFFFu; }

/* A terrain ramp as CClut builds it: 32 rows of 256 RGB555 entries from the darkest light to full light. */
static void build_ramp(float start) {
    const float scale = (32.0f - 0.02f) / (1.0f - start);
    static const uint8_t format[7] = { 16, 10, 3, 5, 3, 0, 3 };
    for (uint32_t at = 0; at < 0x6Cu; at += 4) write32(SHADOW_CLUT + at, 0);
    write32(SHADOW_CLUT + 0x14, 32), write32(SHADOW_CLUT + 0x18, 1);
    writef(SHADOW_CLUT + 0x1C, scale), writef(SHADOW_CLUT + 0x20, 0.01f - start * scale);
    write32(SHADOW_CLUT + 0x2C, 32), writef(SHADOW_CLUT + 0x30, start);
    write32(SHADOW_CLUT + 0x38, SHADOW_TABLE), write32(SHADOW_CLUT + 0x40, 2), write32(SHADOW_CLUT + 0x4C, 256);
    gp_copy_out(SHADOW_CLUT + 0x64, format, sizeof format);
    for (uint32_t row = 0; row < 32; row++)
        for (uint32_t i = 0; i < 256; i += 2) {
            uint32_t v = (uint32_t)((start + (float)row / 31.0f * (1.0f - start)) * 31.0f), px = v << 10 | v << 5 | v;
            write32(SHADOW_TABLE + (row * 256u + i) * 2u, px << 16 | px);
        }
    write32(0x0066FA0Cu, SHADOW_CLUT);
}

/* ShadowLighting's frame and the camera it builds there, with its light level. */
static void shadow_call(uint32_t level, uint32_t moving) {
    uint32_t frame = s_camera + SHADOW_FRAME_FROM_CAMERA;
    write32(frame + 0x14, level), write32(frame + 0x18, moving), write32(frame + 0x1C, 1u - moving);
    write32(0x0066FA14u, level);
    writef(s_props, 256.0f), writef(s_props + 4, 256.0f);
    write32(s_props + 0x34, 1), writef(s_props + 0x38, 1.0f), writef(s_props + 0x3C, 2000.0f);
}

static uint32_t shadow_level_after(void) {
    run_on_guest(body_set_properties);
    return read32(s_camera + SHADOW_FRAME_FROM_CAMERA + 0x14);
}

static void test_terrain_shadows(void) {
    jpt_config_t cfg;
    uint32_t built;
    install_config(&cfg);
    cfg.aspect = 0;
    CHECK(cfg.shadows_enabled && lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start with shadows on");
    g_body = NULL;
    write32(0x0073F444u, 0);
    s_camera = 0x00A20000u;
    s_props = s_camera + PROPS_IN_CAMERA;
    write32(0x0066FA0Cu, 0);
    run_on_guest(body_terrain_update);
    shadow_call(0, 1);
    CHECK(shadow_level_after() == 0, "no Direct3D ramp: nothing is changed");
    shadow_call(9, 1);
    CHECK(shadow_level_after() == 9, "nor without one is a light level");

    build_ramp(0.8685f);
    run_on_guest(body_terrain_update);
    CHECK(pixel_at(0, 4) == (19u << 10 | 19u << 5 | 19u) && pixel_at(31, 4) == 0x7FFFu && pixel_at(1, 4) == pixel_at(2, 4),
          "row 0 is the shadow's, the lit rows are the game's (%04X %04X)", pixel_at(0, 4), pixel_at(31, 4));
    CHECK(readf(SHADOW_CLUT + 0x1C) * 0.8685f + readf(SHADOW_CLUT + 0x20) >= 1.0f &&
          readf(SHADOW_CLUT + 0x1C) + readf(SHADOW_CLUT + 0x20) < 32.0f, "and no light of the terrain reaches it");
    {
        float scale = readf(SHADOW_CLUT + 0x1C);
        run_on_guest(body_terrain_update);
        CHECK(readf(SHADOW_CLUT + 0x1C) == scale, "once per ramp");
    }
    for (uint32_t moving = 0; moving < 2; moving++) {
        shadow_call(1, moving);
        CHECK(shadow_level_after() == 0, "a shadow lighter than at the default brightness draws with the shadow row (moving %u)", moving);
    }
    shadow_call(1, 1);
    writef(s_props + 0x3C, 1000.0f);
    CHECK(shadow_level_after() == 1, "another camera is left alone");
    shadow_call(1, 1);
    write32(0x0066FA14u, 2);
    CHECK(shadow_level_after() == 1, "so is a frame whose light level is not the shadow's");
    shadow_call(1, 1);
    write32(s_camera + SHADOW_FRAME_FROM_CAMERA + 0x1C, 1);
    CHECK(shadow_level_after() == 1, "or whose passes do not add up");
    shadow_call(1, 1);
    s_props = 0x00A30000u;
    for (uint32_t at = 0; at < PROPS_SIZE; at += 4) write32(s_props + at, read32(s_camera + PROPS_IN_CAMERA + at));
    CHECK(shadow_level_after() == 1, "or a camera given other properties than its own");
    s_props = s_camera + PROPS_IN_CAMERA;

    build_ramp(0.4f);
    run_on_guest(body_terrain_update);
    shadow_call(1, 1);
    CHECK(shadow_level_after() == 1, "a shadow already darker keeps its light level");
    shadow_call(26, 0);
    CHECK(shadow_level_after() == 0, "a brighter one in an older ramp does not");

    build_ramp(0.8685f);
    built = pixel_at(0, 4);
    write32(SHADOW_CLUT + 0x14, 64);
    run_on_guest(body_terrain_update);
    shadow_call(1, 1);
    CHECK(pixel_at(0, 4) == built && shadow_level_after() == 1, "a ramp of another shape is refused");
    lx_features_stop();

    install_config(&cfg);
    cfg.aspect = 0, cfg.shadows_enabled = 0;
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start with shadows off");
    build_ramp(0.8685f);
    built = pixel_at(0, 4);
    run_on_guest(body_terrain_update);
    shadow_call(1, 1);
    CHECK(pixel_at(0, 4) == built && shadow_level_after() == 1, "Shadows Off: the ramp and the light level are the game's");
    write32(0x0066FA0Cu, 0);
    lx_features_stop();
}

static void body_add_child(uint32_t va);
static unsigned s_add_child_depth_seen[4];
static uint32_t s_add_child_flags[4];
static unsigned s_add_child_calls;

static void body_add_child(uint32_t va) {
    (void)va;
    if (s_add_child_calls < 4) {
        s_add_child_depth_seen[s_add_child_calls] = s_add_child_calls;
        s_add_child_flags[s_add_child_calls] = MEM32(g_esp + 8) & 0xFFu;
    }
    s_add_child_calls++;
    if (s_add_child_calls < 3) {
        uint32_t args[2] = { 0x00A40000u, 1 };
        push_args(2, args);
        g_ecx = 0x00A41000u;
        lift116_sub_00475F60();
    }
}

static uint32_t s_outer_flag;

static void body_outer_add_child(void) {
    uint32_t args[2] = { 0x00A40000u, s_outer_flag };
    push_args(2, args);
    g_ecx = 0x00A41000u;
    lift116_sub_00475F60();
}

static void test_nested_around(void) {
    jpt_config_t cfg;
    install_config(&cfg);
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start");
    reset_calls();
    s_add_child_calls = 0;
    g_body = body_add_child;
    run_on_guest(body_outer_add_child);
    CHECK(s_add_child_calls == 3, "CPartition::bAddChild reaches itself through the children: the override wraps each nested call (%u)", s_add_child_calls);
    CHECK(s_add_child_flags[0] == 0 && s_add_child_flags[1] == 0 && s_add_child_flags[2] == 0,
          "a nested call takes its caller's flag, not its own (%u %u %u for own flags 0, 1, 1)", s_add_child_flags[0], s_add_child_flags[1], s_add_child_flags[2]);
    reset_calls();
    s_add_child_calls = 2;
    g_body = body_add_child;
    s_outer_flag = 1;
    run_on_guest(body_outer_add_child);
    CHECK(s_add_child_flags[2] == 1, "after the nest unwinds a new outer call keeps its own flag again");
    s_outer_flag = 0;
    lx_features_stop();
}

#define LIFT_HOOK(va) void lift_hook_##va(void);
#include "lift_hook_list.h"
#undef LIFT_HOOK

static uint32_t s_hook_before, s_hook_after;

static void body_hook_kill_normal(void) {
    g_esi = 0x00A50000u;
    s_hook_before = g_esi;
    lift_hook_004EB46B();
    s_hook_after = g_esi;
}

static void test_lift_hooks_are_defined(void) {
    jpt_config_t cfg;
    install_config(&cfg);
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start with the official Patch 1.1 on");
    run_on_guest(body_hook_kill_normal);
    CHECK(s_hook_after == s_hook_before, "a lift hook returns with the guest's registers as it found them where it changes none");
    lx_features_stop();
}

/* ---- Dual stow, footsteps, Safe Mode -------------------------------------------------------------------------------- */

#define PLAYER_BELT 0xE38u
#define PLAYER_BACK 0xE3Cu
#define PLAYER_PHASE 0xE44u

static uint32_t s_belt_during_stow, s_player_for_stow;

static void body_stow_lifted(uint32_t va) {
    (void)va;
    s_belt_during_stow = read32(s_player_for_stow + PLAYER_BELT);
}

static void body_stow(void) {
    uint32_t args[1] = { 0x00A61000u };
    push_args(1, args);
    g_ecx = s_player_for_stow;
    lift116_sub_004610D0();
}

static void test_dual_stow(void) {
    jpt_config_t cfg;
    install_config(&cfg);
    cfg.dual_stow = 1;
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start with Dual stow on");
    s_player_for_stow = 0x00A60000u;
    write32(s_player_for_stow + PLAYER_BELT, 0x1111u);
    write32(s_player_for_stow + PLAYER_BACK, 0x2222u);
    write32(s_player_for_stow + PLAYER_PHASE, 0);
    write32(0x00A61000u + 0x24u, 0x18u);
    write32(0x00A61000u + 0x28u, 0x100u);
    reset_calls();
    g_body = body_stow_lifted;
    run_on_guest(body_stow);
    CHECK(called(0x004610D0), "the retail stow handler runs around the host's choice (AROUND)");
    CHECK(s_belt_during_stow == 0, "Rotate Arm held with both slots full hides the belt while the handler runs, so it takes from the back (%08X)", s_belt_during_stow);
    CHECK(read32(s_player_for_stow + PLAYER_BELT) == 0x1111u, "and the belt is back in its slot afterwards");
    write32(0x00A61000u + 0x24u, 0);
    reset_calls();
    g_body = body_stow_lifted;
    run_on_guest(body_stow);
    CHECK(s_belt_during_stow == 0x1111u, "without the modifier the retail choice is untouched (%08X)", s_belt_during_stow);
    lx_features_stop();
}

#define AUDIO_DAEMON 0x0066E9F0u
#define MAP_NIL 0x0066EB00u
#define GAME_TIME 0x0073F34Cu

static void test_footstep_sounds_rearm(void) {
    jpt_config_t cfg;
    const uint32_t daemon = 0x00A70000u, effects = 0x00A70100u, map = 0x00A70200u, head = 0x00A70300u, nil = 0x00A70400u, node = 0x00A70500u,
                   effect = 0x00A70600u;
    install_config(&cfg);
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start");
    write32(AUDIO_DAEMON, daemon);
    write32(daemon + 0xC4u, effects);
    write32(effects + 0x2Cu, map);
    write32(map, head);
    write32(MAP_NIL, nil);
    write32(head + 4u, node);
    write32(node + 8u, nil);
    write32(node + 0xCu, nil);
    write32(node + 0x18u, effect);
    writef(effect + 4u, 0.5f);
    writef(effect + 8u, 95.0f);
    writef(GAME_TIME, 100.0f);
    ce_gameplay_step();
    CHECK(readf(effect + 8u) == 95.0f, "a collision sound played in the past of the game's clock is left alone");
    writef(GAME_TIME, 2.0f);
    ce_gameplay_step();
    CHECK(readf(effect + 8u) == 1.5f, "a load that sets the game's clock back re-arms the collision sounds that would stay silent (%.2f)", (double)readf(effect + 8u));
    lx_features_stop();
}

static uint32_t call_import(const char* dll, const char* name, int argc, const uint32_t* args) {
    uint32_t thunk = hle_resolve(dll, name, 0, NULL), esp = g_esp, eax = 0, edx = 0;
    if (!thunk) return 0xBAD1BAD1u;
    esp -= 4u * (uint32_t)(argc + 1);
    gp_write_u32(esp, 0x00401000u);
    for (int i = 0; i < argc; i++) gp_write_u32(esp + 4 + 4 * (uint32_t)i, args[i]);
    return ht_invoke(thunk, &esp, &eax, &edx) ? eax : 0xBAD0BAD0u;
}

static uint32_t put_text(uint32_t where, const char* text) {
    gp_copy_out(where, text, (uint32_t)strlen(text) + 1);
    return where;
}

static uint32_t query_dword(uint32_t key, const char* name) {
    const uint32_t out = 0x00A80100u, size = 0x00A80104u, type = 0x00A80108u;
    uint32_t args[6];
    write32(out, 0xFFFFFFFFu);
    write32(size, 4);
    args[0] = key, args[1] = put_text(0x00A80200u, name), args[2] = 0, args[3] = type, args[4] = out, args[5] = size;
    return call_import("advapi32.dll", "RegQueryValueExA", 6, args) == 0 ? read32(out) : 0xDEADu;
}

static void test_safe_mode_and_registry_policy(void) {
    const uint32_t key_out = 0x00A80000u;
    uint32_t key, args[6];
    jpt_make_dirs("build/linux_overrides_tests_user");
    hle_advapi32_init("build/linux_overrides_tests_user", "C:\\", "D:\\");
    args[0] = 0x80000002u, args[1] = put_text(0x00A80300u, "Software\\DreamWorks Interactive\\Trespasser"), args[2] = key_out;
    CHECK(call_import("advapi32.dll", "RegCreateKeyA", 3, args) == 0, "the game's key opens");
    key = read32(key_out);
    args[0] = key, args[1] = put_text(0x00A80400u, "Safemode"), args[2] = 0, args[3] = 4, args[4] = 0x00A80500u, args[5] = 4;
    write32(0x00A80500u, 1);
    call_import("advapi32.dll", "RegSetValueExA", 6, args);
    CHECK(query_dword(key, "Safemode") == 0, "Safe Mode is suppressed: the game's 'unstable last time' flag reads 0 even after it wrote 1");
    CHECK(safemode_value_virtual(0) == 1 && safemode_value_virtual(1) == 0, "the shared policy: virtual unless the legacy dialog is asked for");
    {
        static lx_gpu_ops_t ops;
        uint32_t guid = 0x00A80700u, size_va = 0x00A80740u, type_va = 0x00A80744u;
        CHECK(query_dword(key, "Use D3D") == 0, "Use D3D is 0 without a GPU renderer");
        ops.available = gpu_yes, ops.generation = gpu_generation;
        lx_d3d_set_gpu(&ops);
        CHECK(query_dword(key, "Use D3D") == 1 && query_dword(key, "Hardware Water") == 1 && query_dword(key, "RenderQuality") == ENGINE_RENDER_QUALITY_TOP,
              "with one the game takes its Direct3D path, its hardware water and its top quality");
        args[0] = key, args[1] = put_text(0x00A80400u, "D3D GUID"), args[2] = 0, args[3] = type_va, args[4] = guid, args[5] = size_va;
        write32(size_va, 16);
        CHECK(call_import("advapi32.dll", "RegQueryValueExA", 6, args) == 0 && read32(type_va) == 3 && read32(size_va) == 16 && read32(guid) == 0x84E63DE0u,
              "and the hardware device's GUID");
        lx_d3d_set_gpu(NULL);
        CHECK(query_dword(key, "Use D3D") == 0, "nothing of it is stored");
    }
    CHECK(query_dword(key, "AudioEnable3D") == 0, "AudioEnable3D stays 0 whatever is stored");
    for (uint32_t level = 5; level <= 9; level += 2) {
        args[0] = key, args[1] = put_text(0x00A80400u, "Gamma"), args[2] = 0, args[3] = 4, args[4] = 0x00A80500u, args[5] = 4;
        write32(0x00A80500u, level);
        call_import("advapi32.dll", "RegSetValueExA", 6, args);
        CHECK(query_dword(key, "Gamma") == (level > ENGINE_GAMMA_TOP ? ENGINE_GAMMA_TOP : level),
              "a brightness level without shading reads as the last one that has it");
    }
    CHECK(query_dword(key, "UseSubTitles") == 1, "subtitles are On until the game's options store a choice");
    guest_skip_video(1);
    CHECK(query_dword(key, "NoVideo") == 1, "Skip Intro: NoVideo reads 1 while an opening is skipped");
    guest_skip_video(0);
    {
        uint32_t out = 0x00A80600u, len_va = 0x00A80640u, type_va = 0x00A80644u;
        args[0] = key, args[1] = put_text(0x00A80400u, "Data Drive"), args[2] = 0, args[3] = type_va, args[4] = out, args[5] = len_va;
        write32(len_va, 64);
        CHECK(call_import("advapi32.dll", "RegQueryValueExA", 6, args) == 0 && !strcmp((const char*)gm_ptr(out, 64), "C:\\"),
              "virtual registry preserves Data Drive = C:\\");
        args[1] = put_text(0x00A80400u, "Installed Directory");
        write32(len_va, 64);
        CHECK(call_import("advapi32.dll", "RegQueryValueExA", 6, args) == 0 && !strcmp((const char*)gm_ptr(out, 64), "D:\\"),
              "virtual registry preserves Installed Directory = D:\\");
    }
}

static void test_language_vs_version_codepage(void) {
    const jpt_game_version_t* en = jpt_game_version_by_id("en-116");
    const jpt_game_version_t* ru = jpt_game_version_by_id("ru-116");
    jpt_config_t cfg;

    jpt_config_defaults(&cfg);
    jpt_config_apply(&cfg, "language_en-116", "ru");
    jpt_config_select_language(&cfg, en);
    CHECK(!strcmp(cfg.language, "ru"), "en-116 can select Russian interface language");
    l10n_init("build/linux_overrides_l10n_cache", "C:", "game", cfg.language, en);
    lx_text_set_codepage(jpt_l10n_display_codepage());
    CHECK(jpt_l10n_display_codepage() == 1251 && lx_text_active_codepage() == 1251,
          "en-116 with Russian UI activates CP1251 display codepage");

    jpt_config_defaults(&cfg);
    jpt_config_apply(&cfg, "language_ru-116", "en");
    jpt_config_select_language(&cfg, ru);
    CHECK(!strcmp(cfg.language, "en"), "ru-116 can select English interface language");
    l10n_init("build/linux_overrides_l10n_cache", "C:", "game", cfg.language, ru);
    lx_text_set_codepage(jpt_l10n_display_codepage());
    CHECK(jpt_l10n_display_codepage() == 0 && lx_text_active_codepage() == 1252,
          "ru-116 with English UI keeps CP1252 display codepage");

    jpt_l10n_set_display_codepage(0);
    lx_text_set_codepage(1252);
}

/* ---- menu DDF text --------------------------------------------------------------------------------------------------- */

typedef struct {
    int files, translated, same_path, saves_same, refused;
    char first[512], any[512];
} ddf_scan_t;

static ddf_scan_t s_ddf;

static char* slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    char* data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char*)calloc((size_t)n + 1, 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) data[0] = 0;
    fclose(f);
    return data;
}

static void visit_ddf(const char* name, int is_dir, void* user) {
    char guest[128], saves[128], out[512], again[512], via_saves[512];
    size_t n = strlen(name);
    (void)user;
    if (is_dir || n < 5 || strcmp(name + n - 4, ".ddf")) return;
    s_ddf.files++;
    snprintf(guest, sizeof guest, "C:\\data\\menu\\%s", name);
    snprintf(saves, sizeof saves, "D:\\menu\\%s", name);
    if (!l10n_menu_file(guest, out, sizeof out)) return;
    s_ddf.translated++;
    if (!s_ddf.first[0]) snprintf(s_ddf.first, sizeof s_ddf.first, "%s", out);
    s_ddf.same_path += l10n_menu_file(guest, again, sizeof again) && !strcmp(out, again);
    s_ddf.saves_same += l10n_menu_file(saves, via_saves, sizeof via_saves) && !strcmp(out, via_saves);
}

static void test_menu_ddf_translation(void) {
    jpt_vpath_t* vp = jpt_vpath_new();
    char out[512];
    if (!jpt_fs_is_dir("game/data/menu") || !vp) {
        printf("  menu DDF translation: skipped, no game data\n");
        return;
    }
    jpt_vpath_mount(vp, 'C', "game");
    hle_shutdown();
    hle_init(vp, 0x00400000u, "C:\trespass.exe");
    jpt_make_dirs("build/linux_overrides_tests_user/cache");
    l10n_init("build/linux_overrides_tests_user/cache", "C:", "game", "fr", jpt_game_version_by_id("en-116"));
    l10n_set_user_folder("D:\\");
    memset(&s_ddf, 0, sizeof s_ddf);
    jpt_fs_list("game/data/menu", visit_ddf, NULL);
    CHECK(s_ddf.files > 3 && s_ddf.translated >= 1, "French labels are put into the DDF files that have them (%d of %d)", s_ddf.translated, s_ddf.files);
    CHECK(s_ddf.translated && s_ddf.same_path == s_ddf.translated, "a second request answers with the same cache file, translated once");
    CHECK(s_ddf.translated && s_ddf.saves_same == s_ddf.translated, "saves\\menu is never the source: its name resolves to the game's own DDF and the same cache file");
    if (s_ddf.first[0]) {
        char* text = slurp(s_ddf.first);
        CHECK(text && strstr(s_ddf.first, "menu-text") && strstr(s_ddf.first, "fr"), "the translated copy lives in the cache folder of the language (%s)", s_ddf.first);
        free(text);
    }
    CHECK(!l10n_menu_file("C:\\data\\menu\\..\\x.ddf", out, sizeof out) && !l10n_menu_file("C:\\data\\other\\x.ddf", out, sizeof out) &&
              !l10n_menu_file("C:\\data\\menu\\a.tga", out, sizeof out),
          "a path outside the menu folder, a path with .. and a file that is not a DDF are refused");
    hle_shutdown();
    hle_init(NULL, 0x00400000u, "C:\trespass.exe");
    jpt_vpath_free(vp);
}

/* ---- threads --------------------------------------------------------------------------------------------------------- */

#define THREAD_CALLS 500

static pthread_t s_threads[2];
static int s_thread_ok[2];

static void thread_body(void* arg) {
    int* ok = (int*)arg;
    for (int i = 0; i < THREAD_CALLS; i++) {
        uint32_t args[1] = { 100 };
        push_args(1, args);
        lift116_sub_0040DA00();
        if (g_eax != 1) *ok = 0;
    }
}

static void* thread_main(void* arg) {
    lx_thread_t* t = lx_thread_new(256 * 1024);
    if (!t) return NULL;
    lx_thread_attach(t);
    lx_run(thread_body, arg);
    lx_thread_detach();
    lx_thread_free(t);
    return NULL;
}

static void test_threads(void) {
    jpt_config_t cfg;
    install_config(&cfg);
    lx_thread_detach();
    CHECK(lx_features_start(&cfg, "game", "en-116", NULL, NULL), "features start");
    for (int i = 0; i < 2; i++) {
        s_thread_ok[i] = 1;
        pthread_create(&s_threads[i], NULL, thread_main, &s_thread_ok[i]);
    }
    for (int i = 0; i < 2; i++) pthread_join(s_threads[i], NULL);
    CHECK(s_thread_ok[0] && s_thread_ok[1], "two guest threads calling an override at once each get its answer: the machine lock serialises them");
    lx_features_stop();
    lx_thread_attach(s_thread);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    hle_init(NULL, 0x00400000u, "C:\\trespass.exe");
    lx_spine_init((lx_table_t){ NULL, 0 }, (lx_table_t){ NULL, 0 });
    if (!gm_alloc_pages(0x00A00000u, 0x00100000u, "fake guest data")) {
        printf("cannot allocate the fake guest data\n");
        return 2;
    }
    if (!gm_alloc_pages(0x00660000u, 0x00200000u, "fake guest statics")) {
        printf("cannot allocate the fake guest statics\n");
        return 2;
    }
    s_thread = lx_thread_new(2 * 1024 * 1024);
    lx_thread_attach(s_thread);
    lx_lift116_register_overrides();
    test_tables();
    test_install();
    test_direct3d_hooks();
    test_direct3d_hooks_of_every_lift();
    test_settings_choose();
    test_fingerprints();
    test_duplicates_and_rollback();
    test_replace_abi();
    test_ecx_reaches_the_lifted_body();
    test_field_of_view();
    test_terrain_shadows();
    test_nested_around();
    test_lift_hooks_are_defined();
    test_dual_stow();
    test_footstep_sounds_rearm();
    test_safe_mode_and_registry_policy();
    test_language_vs_version_codepage();
    test_menu_ddf_translation();
    test_threads();
    lx_thread_detach();
    lx_thread_free(s_thread);
    hle_shutdown();
    gm_shutdown();
    printf("linux_overrides_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

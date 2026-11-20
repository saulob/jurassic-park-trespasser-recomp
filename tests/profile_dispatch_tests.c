/*
 * Which lift each game version runs and what the one program knows of it, headless: no window, no game, no lifted
 * code. The profile tables are the generated ones the host links (src/host/profiles); the lifted sources' own metadata
 * is checked with synthetic descriptions of a stale build.
 */
#include <stdio.h>
#include <string.h>

#include "active_profile.h"
#include "guest_va.h"

static int g_failures;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            g_failures++;                                                              \
        }                                                                              \
    } while (0)

static const jpt_game_version_t* V(const char* id) { return jpt_game_version_by_id(id); }

static int activate(const char* id) {
    char error[256];
    jpt_profile_reset();
    return jpt_profile_activate(V(id), error, sizeof error);
}

static void each_profile_selects_its_lift(void) {
    static const struct {
        const char* profile;
        const char* lift;
        jpt_lift_t index;
    } rows[] = {
        { "en-116", "lift116", JPT_LIFT_116 },       { "ru-116", "lift116", JPT_LIFT_116 },
        { "es-117", "lift_es117", JPT_LIFT_ES117 },  { "ptbr-117", "lift_ptbr117", JPT_LIFT_PTBR117 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        jpt_lift_t lift = JPT_LIFT_NONE;
        CHECK(V(rows[i].profile) != NULL);
        CHECK(!strcmp(V(rows[i].profile)->lift, rows[i].lift));
        CHECK(jpt_lift_find(rows[i].lift, &lift) && lift == rows[i].index);
        CHECK(activate(rows[i].profile));
        CHECK(jpt_active_profile() == V(rows[i].profile));
        CHECK(jpt_active_lift() == rows[i].index);
        CHECK(jpt_active_lift_data() == &jpt_lift_data[rows[i].index]);
        CHECK(!strcmp(jpt_active_lift_data()->id, rows[i].lift));
        CHECK(!strcmp(jpt_active_lift_data()->expected.lift_id, rows[i].lift));
        CHECK(strlen(jpt_active_lift_data()->expected.source_sha256) == 64);
    }
    CHECK(!jpt_lift_find("lift_other", NULL) && !jpt_lift_find(NULL, NULL));
    jpt_profile_reset();
    CHECK(jpt_active_profile() == NULL && jpt_active_lift() == JPT_LIFT_NONE && jpt_active_lift_data() == NULL);
}

static void each_lift_has_its_entry_point_and_source(void) {
    CHECK(jpt_lift_data[JPT_LIFT_116].expected.entry_va == 0x00626BE2u);
    CHECK(jpt_lift_data[JPT_LIFT_ES117].expected.entry_va == 0x0060D302u);
    CHECK(jpt_lift_data[JPT_LIFT_PTBR117].expected.entry_va == 0x0060F812u);
    CHECK(jpt_lift_data[JPT_LIFT_116].expected.selfmod_start == 0x0063B000u);
    CHECK(jpt_lift_data[JPT_LIFT_ES117].expected.selfmod_start == 0x0063D000u);
    CHECK(jpt_lift_data[JPT_LIFT_PTBR117].expected.selfmod_start == 0x0063F000u);
    CHECK(!strcmp(jpt_lift_data[JPT_LIFT_116].expected.source_profile, "en-116"));
    CHECK(!strcmp(jpt_lift_data[JPT_LIFT_ES117].expected.source_profile, "es-117"));
    CHECK(!strcmp(jpt_lift_data[JPT_LIFT_PTBR117].expected.source_profile, "ptbr-117"));
    for (int i = 0; i < JPT_LIFT_COUNT; i++) {
        CHECK(jpt_lift_data[i].expected.image_base == JPT_GUEST_IMAGE_BASE);
        CHECK(jpt_lift_data[i].expected.format == JPT_LIFT_FORMAT);
        for (int j = i + 1; j < JPT_LIFT_COUNT; j++)
            CHECK(strcmp(jpt_lift_data[i].id, jpt_lift_data[j].id) && strcmp(jpt_lift_data[i].expected.source_sha256,
                                                                          jpt_lift_data[j].expected.source_sha256));
    }
}

static void gva_follows_the_active_profile(void) {
    static const struct {
        uint32_t id;
        uint32_t en, es, ptbr; /* 0: absent */
    } rows[] = {
        { GVAI_0x00403520, 0x00403520u, 0x00403560u, 0x00403530u },
        { GVAI_0x0040BCC0, 0x0040BCC0u, 0x0040BDE0u, 0x0040BFD0u },
        { GVAI_0x0040C780, 0x0040C780u, 0x0040C8A0u, 0x0040CAC0u },
        { GVAI_0x00475F60, 0x00475F60u, 0x0045B6F0u, 0u },
        { GVAI_0x004F0960, 0x004F0960u, 0u, 0u },
        { GVAI_0x0066ED67, 0x0066ED67u, 0u, 0u },
        { GVAI_0x00699D40, 0x00699D40u, 0x0069A928u, 0x0069C970u },
        { GVAI_0x0073DC40, 0x0073DC40u, 0x007405B0u, 0x00741D60u },
    };
    static const char* const profiles[] = { "en-116", "ru-116", "es-117", "ptbr-117" };
    for (size_t p = 0; p < 4; p++) {
        CHECK(activate(profiles[p]));
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
            uint32_t want = p < 2 ? rows[i].en : p == 2 ? rows[i].es : rows[i].ptbr;
            CHECK(jpt_gva[rows[i].id] == want);
            CHECK(GVA_AT(rows[i].id) == want);
        }
    }
    CHECK(activate("es-117"));
    CHECK(GVA(0x0040BCC0) == 0x0040BDE0u && GVA(0x00699D40) == 0x0069A928u);
    CHECK(activate("ptbr-117"));
    CHECK(GVA(0x0040BCC0) == 0x0040BFD0u);
}

static void gva_ids_are_canonical(void) {
    for (int i = 1; i < JPT_GVA_COUNT; i++) CHECK(jpt_gva_canonical[i - 1] < jpt_gva_canonical[i]);
    for (int i = 0; i < JPT_GVA_COUNT; i++) {
        CHECK(jpt_lift_data[JPT_LIFT_116].gva[i] == jpt_gva_canonical[i]);
        CHECK(jpt_lift_data[JPT_LIFT_116].gva[i] != 0);
    }
    CHECK(JPT_GVA_COUNT > 300);
}

static void an_absent_address_belongs_to_an_unapplied_patch_group(void) {
    for (int lift = 0; lift < JPT_LIFT_COUNT; lift++) {
        CHECK(jpt_lift_missing_address(&jpt_lift_data[lift]) == -1);
        for (int i = 0; i < JPT_GVA_COUNT; i++) {
            if (jpt_lift_data[lift].gva[i]) continue;
            CHECK(jpt_gva_gate[i] != 0);
            CHECK((jpt_gva_gate[i] & jpt_lift_data[lift].patch_groups) == 0);
        }
    }
}

static void a_required_address_that_is_missing_refuses_before_the_guest(void) {
    static jpt_lift_data_t broken;
    char error[256];
    int required = -1, gated = -1;
    broken = jpt_lift_data[JPT_LIFT_ES117];
    for (int i = 0; i < JPT_GVA_COUNT; i++) {
        if (!jpt_gva_gate[i] && required < 0) required = i;
        if (jpt_gva_gate[i] && (jpt_gva_gate[i] & broken.patch_groups) && gated < 0) gated = i;
    }
    CHECK(required >= 0 && gated >= 0);
    broken.gva[required] = 0;
    CHECK(jpt_lift_missing_address(&broken) == required);
    broken = jpt_lift_data[JPT_LIFT_ES117];
    broken.gva[gated] = 0;
    CHECK(jpt_lift_missing_address(&broken) == gated);
    jpt_profile_reset();
    CHECK(!jpt_profile_activate(NULL, error, sizeof error) && strstr(error, "no game version"));
    CHECK(jpt_active_lift() == JPT_LIFT_NONE);
}

static void layouts_follow_the_active_profile(void) {
    static const struct {
        const char* profile;
        uint32_t stride;
    } rows[] = { { "en-116", 0x1090u }, { "ru-116", 0x1090u }, { "es-117", 0x1090u }, { "ptbr-117", 0x1064u } };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        CHECK(activate(rows[i].profile));
        CHECK(GLAYOUT_BOX_POOL_STRIDE == rows[i].stride);
        CHECK(jpt_layout.box_pool_stride == rows[i].stride);
    }
}

static void patch_groups_follow_the_active_profile(void) {
    static const struct {
        const char* profile;
        int hand_jump, add_child, box_box, wake;
    } rows[] = {
        { "en-116", 1, 1, 1, 1 }, { "ru-116", 1, 1, 1, 1 }, { "es-117", 0, 1, 1, 0 }, { "ptbr-117", 0, 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        CHECK(activate(rows[i].profile));
        CHECK(JPT_P117_HAND_JUMP == rows[i].hand_jump);
        CHECK(JPT_P117_ADD_CHILD == rows[i].add_child);
        CHECK(JPT_P117_BOX_BOX == rows[i].box_box);
        CHECK(JPT_P117_WAKE == rows[i].wake);
    }
}

static void the_profile_is_chosen_once(void) {
    char error[256];
    CHECK(activate("es-117"));
    CHECK(jpt_profile_activate(V("es-117"), error, sizeof error));
    CHECK(!jpt_profile_activate(V("ptbr-117"), error, sizeof error) && strstr(error, "chosen once"));
    CHECK(jpt_active_profile() == V("es-117") && GVA(0x0040BCC0) == 0x0040BDE0u);
    {
        jpt_game_version_t other = *V("es-117");
        other.lift = "lift_other";
        jpt_profile_reset();
        CHECK(!jpt_profile_activate(&other, error, sizeof error) && strstr(error, "does not carry"));
        CHECK(jpt_active_profile() == NULL && GVA(0x0040BCC0) == 0u);
    }
}

static void a_stale_lift_is_refused_by_its_metadata(void) {
    const jpt_lift_meta_t expected = jpt_lift_data[JPT_LIFT_ES117].expected;
    jpt_lift_meta_t actual = expected;
    actual.function_count = 17832;
    CHECK(jpt_lift_meta_check(&expected, &actual) == JPT_LIFT_META_OK);
    CHECK(jpt_lift_meta_check(NULL, &actual) == JPT_LIFT_META_MISSING && jpt_lift_meta_check(&expected, NULL) == JPT_LIFT_META_MISSING);
    actual.lift_id = "lift116";
    CHECK(jpt_lift_meta_check(&expected, &actual) == JPT_LIFT_META_WRONG_LIFT);
    actual = expected;
    actual.function_count = 1;
    actual.source_profile = "en-116";
    CHECK(jpt_lift_meta_check(&expected, &actual) == JPT_LIFT_META_WRONG_PROFILE);
    actual.source_profile = expected.source_profile;
    actual.source_sha256 = jpt_lift_data[JPT_LIFT_PTBR117].expected.source_sha256;
    CHECK(jpt_lift_meta_check(&expected, &actual) == JPT_LIFT_META_WRONG_EXECUTABLE);
    actual.source_sha256 = expected.source_sha256;
    actual.format = expected.format - 1;
    CHECK(jpt_lift_meta_check(&expected, &actual) == JPT_LIFT_META_WRONG_FORMAT);
    actual.format = expected.format;
    actual.entry_va ^= 0x10u;
    CHECK(jpt_lift_meta_check(&expected, &actual) == JPT_LIFT_META_WRONG_IMAGE);
    actual.entry_va = expected.entry_va;
    actual.image_base = 0x00500000u;
    CHECK(jpt_lift_meta_check(&expected, &actual) == JPT_LIFT_META_WRONG_IMAGE);
    actual = expected;
    CHECK(jpt_lift_meta_check(&expected, &actual) == JPT_LIFT_META_EMPTY);
    for (int r = JPT_LIFT_META_MISSING; r <= JPT_LIFT_META_EMPTY; r++) CHECK(strlen(jpt_lift_meta_result_text((jpt_lift_meta_result_t)r)) > 4);
}

static void every_version_names_a_lift_the_program_carries(void) {
    for (int i = 0; i < JPT_GAME_VERSION_COUNT; i++) {
        jpt_lift_t lift;
        CHECK(jpt_lift_find(jpt_game_versions[i].lift, &lift));
    }
}

/* The reservation made at start, before the launcher may import a version: only the builds installed then (what the
 * host did) against the largest build the program carries. */
static uint32_t reservation_of_installed(const char* const* installed, size_t count) {
    uint32_t span = 0;
    for (size_t i = 0; i < count; i++) {
        jpt_lift_t lift;
        if (jpt_lift_find(V(installed[i])->lift, &lift) && jpt_lift_data[lift].image_size > span) span = jpt_lift_data[lift].image_size;
    }
    return span;
}

static uint32_t image_size_of(const char* profile) {
    jpt_lift_t lift;
    return jpt_lift_find(V(profile)->lift, &lift) ? jpt_lift_data[lift].image_size : 0;
}

static void the_start_up_reservation_fits_every_build_whatever_is_installed(void) {
    static const char* const only_english[] = { "en-116" };
    static const char* const profiles[] = { "en-116", "ru-116", "es-117", "ptbr-117" };
    const uint32_t reserved = jpt_guest_reservation_size();
    CHECK(image_size_of("en-116") == 0x00347000u && image_size_of("ru-116") == 0x00347000u);
    CHECK(image_size_of("es-117") == 0x0034C000u && image_size_of("ptbr-117") == 0x0034D000u);
    CHECK(reservation_of_installed(only_english, 1) == 0x00347000u);
    CHECK(reservation_of_installed(only_english, 1) < image_size_of("ptbr-117"));
    CHECK(reservation_of_installed(only_english, 1) < image_size_of("es-117"));
    CHECK(JPT_GUEST_IMAGE_BASE == 0x00400000u);
    for (size_t i = 0; i < 4; i++) CHECK(reserved >= image_size_of(profiles[i]));
    for (int i = 0; i < JPT_LIFT_COUNT; i++) CHECK(reserved >= jpt_lift_data[i].image_size && jpt_lift_data[i].image_size > 0);
    CHECK(reserved == 0x0034D000u);
    CHECK(JPT_GUEST_IMAGE_BASE + reserved == 0x0074D000u);
    CHECK(reservation_of_installed(NULL, 0) == 0 && reserved > 0);
}

int main(void) {
    each_profile_selects_its_lift();
    each_lift_has_its_entry_point_and_source();
    gva_follows_the_active_profile();
    gva_ids_are_canonical();
    an_absent_address_belongs_to_an_unapplied_patch_group();
    a_required_address_that_is_missing_refuses_before_the_guest();
    layouts_follow_the_active_profile();
    patch_groups_follow_the_active_profile();
    the_profile_is_chosen_once();
    a_stale_lift_is_refused_by_its_metadata();
    every_version_names_a_lift_the_program_carries();
    the_start_up_reservation_fits_every_build_whatever_is_installed();
    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "profile dispatch tests passed", g_failures);
    return g_failures ? 1 : 0;
}

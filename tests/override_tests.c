/*
 * The override registry and its stable entries, headless, with the lifted side (tests/override_fixture.c) compiled
 * as the lifted code is. CMake links this twice, with /INCREMENTAL and /INCREMENTAL:NO
 * (OVERRIDE_TESTS_INCREMENTAL): every path must reach an installed override in both.
 */
#include "override_registry.h"

#include <stdio.h>
#include <string.h>

#ifndef OVERRIDE_TESTS_INCREMENTAL
#error "build through CMake: override_tests_incremental or override_tests_non_incremental"
#endif

#define GUEST_OVERRIDE OVERRIDE_LIFTED_DECL
#include "override_fixture_list.h"
#undef GUEST_OVERRIDE
#define GUEST_OVERRIDE OVERRIDE_STABLE_ENTRY
#include "override_fixture_list.h"
#undef GUEST_OVERRIDE

static override_entry_t s_entries[] = {
#define GUEST_OVERRIDE OVERRIDE_ROW
#include "override_fixture_list.h"
#undef GUEST_OVERRIDE
};

extern uint32_t g_fixture_alias_entry;
extern uint32_t g_fixture_log[16];
extern unsigned g_fixture_log_count;
void fixture_note(uint32_t what);
void fixture_call_same_file(void);
void fixture_call_alias(void);
void fixture_call_all(void);
override_fn_t fixture_lookup(uint32_t va);

static int s_failures;
static override_fn_t s_required_lifted, s_optional_lifted, s_second_lifted;
static int s_required_fallback;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            s_failures++;                                              \
        }                                                              \
    } while (0)

static void reset_log(void) {
    g_fixture_log_count = 0;
    memset(g_fixture_log, 0, sizeof g_fixture_log);
}

static int ran(uint32_t what) { return g_fixture_log_count == 1 && g_fixture_log[0] == what; }

static void required_override(void) {
    fixture_note(0xA0401000u);
    if (s_required_fallback) s_required_lifted();
}

static void optional_kernel(void) {
    if (g_fixture_alias_entry) {
        s_optional_lifted();
        return;
    }
    fixture_note(0xA0402000u);
}

static void second_override(void) { fixture_note(0xB0401000u); }

static override_registry_t fresh_registry(void) {
    override_registry_t r;
    memset(&r, 0, sizeof r);
    r.entries = s_entries;
    r.count = sizeof s_entries / sizeof s_entries[0];
    return r;
}

static void test_link_model(void) {
    const unsigned char* lifted = (const unsigned char*)s_entries[0].lifted;
    int thunk = lifted[0] == 0xE9;
    printf("link model: %s, lifted bodies reached %s\n", OVERRIDE_TESTS_INCREMENTAL ? "/INCREMENTAL" : "/INCREMENTAL:NO",
           thunk ? "through incremental-link thunks" : "directly");
    CHECK(thunk == OVERRIDE_TESTS_INCREMENTAL);
}

static void test_tables(void) {
    override_fn_t a = s_entries[0].lifted, b = s_entries[1].lifted;
    override_entry_t unsorted[2] = { { 0x2000u, &a, s_entries[0].lifted }, { 0x1000u, &b, s_entries[1].lifted } };
    override_entry_t repeated[2] = { { 0x1000u, &a, s_entries[0].lifted }, { 0x1000u, &b, s_entries[1].lifted } };
    override_entry_t unset[1] = { { 0x1000u, &b, s_entries[0].lifted } };
    override_registry_t r = fresh_registry();
    CHECK(override_registry_valid(&r));
    for (unsigned i = 0; i < r.count; i++) CHECK(override_registry_find(&r, s_entries[i].va) == &s_entries[i]);
    CHECK(!override_registry_find(&r, 0x00401001u));
    CHECK(!override_registry_find(&r, 0x00400000u));
    CHECK(!override_registry_find(&r, 0x00500000u));
    r.entries = unsorted;
    r.count = 2;
    CHECK(!override_registry_valid(&r));
    r.entries = repeated;
    CHECK(!override_registry_valid(&r));
    r.entries = unset;
    r.count = 1;
    CHECK(!override_registry_valid(&r));
}

static void test_before_install(void) {
    override_registry_t r = fresh_registry();
    override_fn_t lifted = NULL;
    CHECK(!override_registry_install(&r, 0x00401000u, required_override, &lifted));
    CHECK(r.refusal_count == 1 && r.refusals[0].why == OVERRIDE_REFUSED_UNVERIFIED && r.refusals[0].required);
    CHECK(s_entries[0].state == OVERRIDE_FAILED);
    s_entries[0].state = OVERRIDE_UNUSED;
    reset_log();
    fixture_call_same_file();
    CHECK(ran(0x00401000u));
    reset_log();
    fixture_lookup(0x00401000u)();
    CHECK(ran(0x00401000u));
}

static void test_installed(void) {
    override_registry_t r = fresh_registry();
    override_fn_t cached = fixture_lookup(0x00401000u), dummy = NULL;
    override_counts_t c;
    CHECK(override_registry_valid(&r));
    r.target_verified = 1;

    CHECK(override_registry_install(&r, 0x00401000u, required_override, &s_required_lifted));
    CHECK(s_required_lifted == s_entries[0].lifted);
    reset_log();
    fixture_call_same_file();
    CHECK(ran(0xA0401000u));
    reset_log();
    sub_00401000();
    CHECK(ran(0xA0401000u));
    reset_log();
    cached();
    CHECK(ran(0xA0401000u));
    reset_log();
    fixture_lookup(0x00401000u)();
    CHECK(ran(0xA0401000u));
    reset_log();
    s_required_fallback = 1;
    sub_00401000();
    s_required_fallback = 0;
    CHECK(g_fixture_log_count == 2 && g_fixture_log[0] == 0xA0401000u && g_fixture_log[1] == 0x00401000u);

    CHECK(override_registry_install(&r, 0x00402000u, optional_kernel, &s_optional_lifted));
    reset_log();
    fixture_lookup(0x00402000u)();
    CHECK(ran(0xA0402000u));
    reset_log();
    fixture_call_alias();
    CHECK(ran(0x00402010u));
    reset_log();
    fixture_lookup(0x00402010u)();
    CHECK(ran(0x00402010u));

    CHECK(!override_registry_install(&r, 0x00401000u, second_override, &s_second_lifted));
    CHECK(r.refusals[r.refusal_count - 1].why == OVERRIDE_REFUSED_TWICE);
    CHECK(s_entries[0].state == OVERRIDE_INSTALLED && !s_second_lifted);
    CHECK(!override_registry_install(&r, 0x00403000u, NULL, &dummy));
    CHECK(!override_registry_install(&r, 0x00403000u, s_entries[2].lifted, &dummy));
    CHECK(r.refusals[r.refusal_count - 1].why == OVERRIDE_REFUSED_NO_REPLACEMENT);
    CHECK(!override_registry_install(&r, 0x00404000u, second_override, &dummy));
    CHECK(r.refusals[r.refusal_count - 1].why == OVERRIDE_REFUSED_UNLISTED && r.refusals[r.refusal_count - 1].required);
    CHECK(!dummy);

    reset_log();
    fixture_call_all();
    CHECK(g_fixture_log_count == 3 && g_fixture_log[0] == 0xA0401000u && g_fixture_log[1] == 0xA0402000u &&
          g_fixture_log[2] == 0x00403000u);

    c = override_registry_counts(&r);
    CHECK(c.listed == 3 && c.installed == 2 && c.required_installed == 1 && c.optional_installed == 1);
    CHECK(c.unused == 0 && c.required_failed == 1 && c.optional_failed == 0);
    CHECK(c.refused == 4 && c.required_refused == 4);
}

static void test_optional_refusal(void) {
    override_registry_t r = fresh_registry();
    override_fn_t dummy = NULL;
    override_counts_t c;
    for (unsigned i = 0; i < r.count; i++) {
        *s_entries[i].entry = s_entries[i].lifted;
        s_entries[i].state = OVERRIDE_UNUSED;
    }
    r.target_verified = 1;
    CHECK(!override_registry_install(&r, 0x00402000u, NULL, &dummy));
    c = override_registry_counts(&r);
    CHECK(c.refused == 1 && c.required_refused == 0 && c.optional_failed == 1 && c.unused == 2);
    reset_log();
    fixture_call_all();
    CHECK(g_fixture_log_count == 3 && g_fixture_log[0] == 0x00401000u && g_fixture_log[1] == 0x00402000u &&
          g_fixture_log[2] == 0x00403000u);
}

int main(void) {
    test_link_model();
    test_tables();
    test_before_install();
    test_installed();
    test_optional_refusal();
    if (s_failures) {
        printf("override_tests: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("override_tests: all passed\n");
    return 0;
}

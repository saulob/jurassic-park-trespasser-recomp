/* The lifted side of tests/override_tests.c, in the form tools/run_lift.py writes it: listed functions defined as
 * lifted_sub_<va>, every call and dispatch row naming sub_<va>. Compiled with the lifted code's options. */
#include <stdint.h>

typedef void (*fixture_fn_t)(void);
typedef struct {
    uint32_t va;
    fixture_fn_t fn;
} fixture_entry_t;

uint32_t g_fixture_alias_entry;
volatile uint32_t g_fixture_work;
uint32_t g_fixture_log[16];
unsigned g_fixture_log_count;

void fixture_note(uint32_t what) {
    if (g_fixture_log_count < 16) g_fixture_log[g_fixture_log_count] = what;
    g_fixture_log_count++;
}

void sub_00401000(void);
void sub_00402000(void);
void sub_00403000(void);
void sub_00402010(void);

void lifted_sub_00401000(void) {
    g_fixture_work += 3;
    fixture_note(0x00401000u);
}

void lifted_sub_00402000(void) {
    uint32_t alias = g_fixture_alias_entry;
    g_fixture_work += 5;
    g_fixture_alias_entry = 0;
    fixture_note(alias ? alias : 0x00402000u);
}

void lifted_sub_00403000(void) {
    g_fixture_work += 7;
    fixture_note(0x00403000u);
}

void sub_00402010(void) { g_fixture_alias_entry = 0x00402010u; sub_00402000(); }

void fixture_call_same_file(void) {
    g_fixture_work++;
    sub_00401000();
}

void fixture_call_alias(void) {
    g_fixture_work++;
    sub_00402010();
}

void fixture_call_all(void) {
    sub_00401000();
    sub_00402000();
    sub_00403000();
}

static const fixture_entry_t k_dispatch[] = {
    { 0x00401000u, sub_00401000 },
    { 0x00402000u, sub_00402000 },
    { 0x00402010u, sub_00402010 },
    { 0x00403000u, sub_00403000 },
};

fixture_fn_t fixture_lookup(uint32_t va) {
    for (unsigned i = 0; i < sizeof k_dispatch / sizeof k_dispatch[0]; i++)
        if (k_dispatch[i].va == va) return k_dispatch[i].fn;
    return 0;
}

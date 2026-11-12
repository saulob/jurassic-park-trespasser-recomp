/*
 * Headless checks of the patched pcrecomp runtime as the game compiles it (tests/CMake target x87_tests): the x87 ring
 * and its depth counter, the inline rounding against the C library it replaces, and the per-site indirect-call cache.
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp_types.h"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double g_st[8];
int g_fp_top;
uint16_t g_fpu_cw = 0x027F;
uint32_t g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx, g_icall_count;
uint32_t g_recomp_site_lo = 0x00401000u, g_recomp_site_hi = 0x0063ADB0u;
uint32_t g_recomp_fixed_lo = 0x02710000u, g_recomp_fixed_span = 0x10000u;
double g_stbuf[X87_RING], *g_stp = g_stbuf + X87_RING / 2;
int64_t g_sti64buf[X87_RING], *g_sti64p = g_sti64buf + X87_RING / 2;

void recomp_x87_recentre(void) {
    double v[8];
    int64_t w[8];
    memcpy(v, g_stp, sizeof v), memcpy(w, g_sti64p, sizeof w);
    g_stp = g_stbuf + X87_RING / 2, g_sti64p = g_sti64buf + X87_RING / 2;
    memcpy(g_stp, v, sizeof v), memcpy(g_sti64p, w, sizeof w);
}

static int s_failures, s_checks;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static unsigned s_resolves;
static void target_a(void) { g_eax = 0xA; g_esp += 4; }
static void target_b(void) { g_eax = 0xB; g_esp += 4; }
recomp_func_t recomp_lookup_manual(uint32_t va) {
    s_resolves++;
    return va == 0x00500000u || va == 0x00700000u || va == 0x02710320u ? target_a : va == 0x00500010u ? target_b : NULL;
}
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_import(uint32_t va) { (void)va; return NULL; }

static uint64_t bits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static double library_round(double v, int rc) {
    switch (rc) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static uint32_t s_seed = 0x2545F491u;
static uint32_t rnd(void) {
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

/* Every RC mode, and nearest under every SSE rounding mode (nearbyint follows it): bit for bit, signed zeros and
 * NaNs included. */
static void test_rounding(void) {
    static const double edges[] = { 0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.49999999999999994, -0.49999999999999994,
                                    2147483647.5, -2147483648.5, 2147483648.0, -2147483648.0, 2147483647.0,
                                    4503599627370495.5, -4503599627370495.5, 4503599627370496.0, 9007199254740993.0,
                                    1e300, -1e300, DBL_MIN, -DBL_MIN, 5e-324, -5e-324, 0.7, -0.7, 0.3, -0.3 };
    static const unsigned modes[] = { _RC_NEAR, _RC_DOWN, _RC_UP, _RC_CHOP };
    unsigned long mismatches = 0, cases = 0;
    double inf = HUGE_VAL, nan;
    memcpy(&nan, &(uint64_t){ 0x7FF8000000001234ull }, 8);
    for (int m = 0; m < 4; m++) {
        unsigned old = 0;
        _controlfp_s(&old, modes[m], _MCW_RC);
        for (int rc = 0; rc < 4; rc++) {
            if (m && rc) continue;
            for (int i = 0; i < 200000 + (int)(sizeof edges / sizeof edges[0]) + 4; i++) {
                double v;
                if (i < (int)(sizeof edges / sizeof edges[0])) v = edges[i];
                else if (i < (int)(sizeof edges / sizeof edges[0]) + 4) {
                    int k = i - (int)(sizeof edges / sizeof edges[0]);
                    v = k == 0 ? inf : k == 1 ? -inf : k == 2 ? nan : -nan;
                } else if (i & 1) {
                    v = ((int32_t)rnd() + (rnd() & 0xFFFF) / 65536.0) * ((i & 6) == 6 ? 1e-3 : 1.0);
                } else {
                    uint64_t u = ((uint64_t)rnd() << 32) | rnd();
                    memcpy(&v, &u, 8);
                }
                cases++;
                if (bits(fp_round_cw(v, (uint16_t)(0x027F | (rc << 10)))) != bits(library_round(v, rc))) mismatches++;
            }
        }
        _controlfp_s(&old, _RC_NEAR, _MCW_RC);
    }
    printf("rounding: %lu cases, %lu differ from the C library\n", cases, mismatches);
    CHECK(mismatches == 0);
    /* _ftol's chop on a fild-exact value and on a computed one. */
    CHECK(fp_st0_to_i64(-2.75, 0, 0x0C7F) == -2 && fp_st0_to_i64(9007199254740993.0, 9007199254740993ll, 0x0C7F) ==
                                                         9007199254740993ll);
}

/* A push moves the pointer down, a pop up; g_fp_top counts as the shifting model did; near either end of the buffer
 * the eight top values (all an FPU holds) move back to the middle with the pointer. */
static void test_ring(void) {
    int top = g_fp_top;
    double* middle = g_stbuf + X87_RING / 2;
    g_stp = middle, g_sti64p = g_sti64buf + X87_RING / 2;
    for (int i = 0; i < 12; i++) RECOMP_X87_PUSH((double)i);
    CHECK(g_fp_top == top + 12 && g_stp == middle - 12);
    for (int i = 0; i < 8; i++) CHECK(RECOMP_X87_ST(i) == (double)(11 - i));
    for (int i = 0; i < 12; i++) CHECK(RECOMP_X87_POP() == (double)(11 - i));
    CHECK(g_fp_top == top && g_stp == middle);

    g_stp = g_stbuf + 4, g_sti64p = g_sti64buf + 4;
    for (int i = 0; i < 8; i++) g_stp[i] = 100.0 + i;
    for (int i = 0; i < 6; i++) RECOMP_X87_PUSH(200.0 + i);
    CHECK(g_stp > g_stbuf + 8 && g_fp_top == top + 6);
    CHECK(RECOMP_X87_ST(0) == 205.0 && RECOMP_X87_ST(5) == 200.0 && RECOMP_X87_ST(6) == 100.0 &&
          RECOMP_X87_ST(7) == 101.0);
    for (int i = 0; i < 6; i++) (void)RECOMP_X87_POP();
    CHECK(RECOMP_X87_ST(0) == 100.0 && RECOMP_X87_ST(3) == 103.0 && g_fp_top == top);

    g_stp = g_stbuf + X87_RING - 10, g_sti64p = g_sti64buf + X87_RING - 10;
    for (int i = 0; i < 10; i++) g_stp[i] = 300.0 + i;
    (void)RECOMP_X87_POP();
    (void)RECOMP_X87_POP();
    CHECK(g_stp < g_stbuf + X87_RING - 8 && RECOMP_X87_ST(0) == 302.0 && RECOMP_X87_ST(7) == 309.0);
    g_fp_top = top;

    g_stp = middle, g_sti64p = g_sti64buf + X87_RING / 2;
    fp_push_i64(9007199254740993ll);
    CHECK(_STI(0) == 9007199254740993ll && fp_st0_to_i64(_ST(0), _STI(0), 0x027F) == 9007199254740993ll);
    fp_push(1.0);
    fp_xch(1);
    CHECK(_STI(0) == 9007199254740993ll && _ST(0) == (double)9007199254740993ll && _ST(1) == 1.0 && _STI(1) == 0);
    (void)fp_pop();
    (void)fp_pop();
    CHECK(g_stp == middle && g_fp_top == top);
}

static void call_site(uint32_t va) {
    uint32_t _flag_k = 0, _flag_a = 0, _flag_b = 0, _cf = 0;
    RECOMP_ICALL(va);
}

/* Inside the lifted range or the shim page a site resolves once and keeps the target, two of them at a time;
 * elsewhere every call resolves again. */
static void test_site_cache(void) {
    static uint8_t stack[64];
    g_esp = (uint32_t)(uintptr_t)(stack + 32);
    s_resolves = 0;
    for (int i = 0; i < 10; i++) call_site(0x00500000u);
    CHECK(s_resolves == 1 && g_eax == 0xA);
    for (int i = 0; i < 10; i++) call_site(0x00700000u);
    CHECK(s_resolves == 11 && g_eax == 0xA);
    call_site(0x00500010u);
    CHECK(s_resolves == 12 && g_eax == 0xB);
    for (int i = 0; i < 10; i++) {
        call_site(0x00500000u);
        CHECK(g_eax == 0xA);
        call_site(0x00500010u);
        CHECK(g_eax == 0xB);
    }
    CHECK(s_resolves == 12);
    for (int i = 0; i < 10; i++) call_site(0x02710320u);
    CHECK(s_resolves == 13 && g_eax == 0xA);
    call_site(0x00500000u);
    call_site(0x00500010u);
    CHECK(s_resolves == 15);
}

int main(void) {
    test_rounding();
    test_ring();
    test_site_cache();
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures != 0;
}

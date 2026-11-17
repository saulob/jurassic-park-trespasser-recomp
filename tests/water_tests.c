/*
 * Byte-exact comparison of the host kernels (water, occlusion) with the lifted functions they replace, which the
 * build copies out of src/recomp/gen (lifted_sub_*) and compiles with the generated code's options.
 * Each case runs both from the same guest state; everything the guest can observe afterwards must be
 * identical: memory, registers, flags, the x87 model, the backedge counter and every yield.
 * The guest addresses the code uses live in g_window, which the fixed image base puts there.
 */
#include <windows.h>
#include "test_profile.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bump_spans.h"
#include "occlusion_kernel.h"
#include "polygon_submit.h"
#include "water_kernels.h"

void lifted_sub_0060E3F0(void);
void lifted_sub_00559240(void);
void lifted_alt_sub_00559240(void);
void lifted_sub_00638670(void);
void lifted_sub_00630D90(void);
void lifted_alt_sub_0060E3F0(void);
void lifted_alt_sub_00638670(void);
void lifted_alt_sub_00630D90(void);

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double g_st[8];
int g_fp_top;
#ifdef RECOMP_X87_RING
double g_stbuf[X87_RING], *g_stp = g_stbuf + X87_RING / 2;
int64_t g_sti64buf[X87_RING], *g_sti64p = g_sti64buf + X87_RING / 2;
void recomp_x87_recentre(void) {
    double v[8];
    int64_t w[8];
    memcpy(v, g_stp, sizeof v), memcpy(w, g_sti64p, sizeof w);
    g_stp = g_stbuf + X87_RING / 2, g_sti64p = g_sti64buf + X87_RING / 2;
    memcpy(g_stp, v, sizeof v), memcpy(g_sti64p, w, sizeof w);
}
#define X87_TOP g_stp
#define X87_TOP_I64 g_sti64p
#else
#define X87_TOP g_st
#define X87_TOP_I64 g_st_i64
#endif
uint16_t g_fpu_cw;
uint32_t g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx, g_icall_count;
static void stub_set_render_state(void);
static void stub_draw_primitive(void);
static void stub_set_stage_state(void);
static void stub_ftol(void);
#define FAKE_SET_RENDER_STATE 0x7E000058u
#define FAKE_DRAW_PRIMITIVE 0x7E000070u
#define FAKE_SET_STAGE_STATE 0x7E0000A0u
#define FAKE_FTOL 0x7E000100u
recomp_func_t recomp_lookup(uint32_t va) {
    switch (va) {
    case FAKE_SET_RENDER_STATE: return stub_set_render_state;
    case FAKE_DRAW_PRIMITIVE: return stub_draw_primitive;
    case FAKE_SET_STAGE_STATE: return stub_set_stage_state;
    case FAKE_FTOL: return stub_ftol;
    }
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}
recomp_func_t recomp_lookup_import(uint32_t va) {
    (void)va;
    return NULL;
}

#define REGION_LO 0x00640000u
#define REGION_HI 0x00810000u
#define ARENA 0x00760000u
#define ARENA_END 0x007F0000u
#define STACK_TOP 0x0080F000u
#define DAMPING 0x006474D4u
#define NUMERATOR 0x00647B54u
#define INV_SQRT 0x006715A4u
#define ALPHA_OFFSET 0x0073F708u
#define ALPHA_SCALE 0x0073F70Cu
#define ALPHA_TABLE 0x0073F710u
#define ALPHA_ENTRIES 0x8000u
#define EPSILON_ON 0x0066CC98u
#define EPSILON_VALUE 0x00651C20u
#define EPSILON_USED 0x00741110u
#define OCCLUDED_COUNT 0x007410F8u
#define VISIBLE_COUNT 0x007410FCu
#define P_DEVICE 0x006D0E9Cu
#define P_WRAP_U 0x006D2108u
#define P_WRAP_V 0x006D2109u
#define P_FOG_SENT 0x006D2101u
#define P_SHADE_SENT 0x006D2103u
#define P_SPECULAR_SENT 0x006D2104u
#define P_FOG_ENTRIES 0x00732874u
#define P_FOG_TABLE 0x00732878u
#define P_OFFSET_X 0x007382B8u
#define P_OFFSET_Y 0x007382BCu
#define P_FTOL_IMPORT 0x00645290u
#define P_MODE_TABLE 0x00559C5Cu /* the address-mode switch's jump table, in .text */
/* The vertex fan, the calls' arguments and saved registers: below the stack pointer the function returns with. */
#define DEAD_STACK (STACK_TOP - 0x8000u)

static uint8_t g_window[0x600000];
static uint8_t g_initial[REGION_HI - REGION_LO];
static uint8_t g_after[REGION_HI - REGION_LO];

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, cur_func;
} yield_t;

typedef struct {
    uint32_t regs[8], flags[4], cur_func, backedges, alias;
    double st[8];
    int64_t st_i64[8];
    int fp_top, x87_position;
    uint16_t cw;
    unsigned yields;
    yield_t yield[64];
} machine_t;

static machine_t g_run;
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

static float rnd_range(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

static float as_float(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* Inputs: ordinary values; with finite extremes also zero, negative zero, denormals, FLT_MAX and the
 * smallest normal; with non-finite ones also infinities and NaNs. */
enum { ORDINARY, FINITE_EXTREMES, NON_FINITE };

static float rnd_float(float lo, float hi, int extremes) {
    static const uint32_t special[] = { 0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x7F7FFFFFu,
                                        0x00800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u };
    unsigned finite = 6, all = sizeof special / sizeof special[0];
    if (extremes != ORDINARY && rnd_below(64) == 0)
        return as_float(special[rnd_below(extremes == NON_FINITE ? all : finite)]);
    return rnd_range(lo, hi);
}

/* Divisors stay ordinary unless the group allows non-finite results. */
static float rnd_divisor(float lo, float hi, int extremes) {
    return rnd_float(lo, hi, extremes == NON_FINITE ? NON_FINITE : ORDINARY);
}

static void put32(uint32_t va, uint32_t v) { memcpy((void*)(uintptr_t)va, &v, 4); }
static void putf(uint32_t va, float v) { memcpy((void*)(uintptr_t)va, &v, 4); }

static uint32_t get32(uint32_t va) {
    uint32_t v;
    memcpy(&v, (const void*)(uintptr_t)va, 4);
    return v;
}

/* Calls the polygon code makes through the device, logged in guest memory (so compared): arguments, registers and
 * flags at the call, and DrawPrimitive's vertices. */
static uint32_t g_log_base, g_log_at, g_log_end;
static void log32(uint32_t v) {
    if (g_log_at + 4 > g_log_end) return;
    put32(g_log_at, v);
    g_log_at += 4;
}

static void log_call(uint32_t kind, int args) {
    log32(kind);
    log32(g_eax), log32(g_ecx), log32(g_edx), log32(g_ebx), log32(g_esp), log32(g_ebp), log32(g_esi), log32(g_edi);
    log32(g_flag_k), log32(g_flag_a), log32(g_flag_b), log32(g_flag_cf), log32(g_cur_func);
    for (int i = 0; i < args; i++) log32(get32(g_esp + 4 + 4 * i));
}

static void stub_return(uint32_t kind, int args) {
    g_eax = 0x5EED0000u ^ (kind << 8) ^ g_log_at;
    g_ecx = 0xC0C00000u ^ g_log_at;
    g_edx = 0xD0D00000u ^ g_log_at;
    g_esp += 4 + 4 * args;
}

static void stub_set_render_state(void) {
    log_call(1, 3);
    stub_return(1, 3);
}

static void stub_set_stage_state(void) {
    log_call(2, 4);
    stub_return(2, 4);
}

static void stub_draw_primitive(void) {
    uint32_t verts = get32(g_esp + 16), n = get32(g_esp + 20);
    log_call(3, 6);
    for (uint32_t i = 0; i < n * 8 && i < 256 * 8; i++) log32(get32(verts + i * 4));
    stub_return(3, 6);
}

/* msvcrt's _ftol as native32 runs it. */
static void stub_ftol(void) {
    int64_t q = fp_st0_to_i64(RECOMP_X87_ST(0), X87_TOP_I64[0], 0x0C7F);
    (void)RECOMP_X87_POP();
    g_eax = (uint32_t)q;
    g_edx = (uint32_t)((uint64_t)q >> 32);
    g_esp += 4;
}

/* The material's lighting table, allocated on first use. */
void sub_0050C9C0(void) {
    uint32_t self = g_ecx;
    log_call(4, 0);
    if (!get32(self + 0x54)) put32(self + 0x54, 0x7A000000u | (g_log_at & 0xFFFFu));
    g_eax = get32(self + 0x54);
    g_esp += 4;
}

static uint32_t g_arena;
static uint32_t alloc(uint32_t bytes) {
    uint32_t at = (g_arena + 15) & ~15u;
    g_arena = at + bytes;
    if (g_arena > ARENA_END) {
        fprintf(stderr, "arena exhausted\n");
        exit(2);
    }
    return at;
}

static void record_yield(void) {
    yield_t* y;
    if (g_run.yields < 64) {
        y = &g_run.yield[g_run.yields];
        y->eax = g_eax, y->ecx = g_ecx, y->edx = g_edx, y->ebx = g_ebx;
        y->esp = g_esp, y->ebp = g_ebp, y->esi = g_esi, y->edi = g_edi, y->cur_func = g_cur_func;
    }
    g_run.yields++;
}

typedef struct {
    uint32_t regs[8], flags[4], backedges, cur_func;
    double st[8];
    int64_t st_i64[8];
    int fp_top;
    uint16_t cw;
    int hook;
} start_t;

static void random_start(start_t* s, int hook) {
    for (int i = 0; i < 8; i++) s->regs[i] = rnd();
    s->regs[4] = STACK_TOP;
    for (int i = 0; i < 4; i++) s->flags[i] = rnd();
    for (int i = 0; i < 8; i++) {
        uint64_t b = ((uint64_t)rnd() << 32) | rnd();
        memcpy(&s->st[i], &b, 8);
        s->st_i64[i] = (int64_t)(((uint64_t)rnd() << 32) | rnd());
    }
    s->fp_top = (int)rnd_below(8);
    s->backedges = hook && rnd_below(2) ? 0x10000u - rnd_below(600) : rnd_below(0x8000);
    s->cur_func = rnd();
    s->hook = hook;
}

static void enter(const start_t* s) {
    memcpy((void*)(uintptr_t)REGION_LO, g_initial, sizeof g_initial);
    g_eax = s->regs[0], g_ecx = s->regs[1], g_edx = s->regs[2], g_ebx = s->regs[3];
    g_esp = s->regs[4], g_ebp = s->regs[5], g_esi = s->regs[6], g_edi = s->regs[7];
    g_flag_k = s->flags[0], g_flag_a = s->flags[1], g_flag_b = s->flags[2], g_flag_cf = s->flags[3];
#ifdef RECOMP_X87_RING
    g_stp = g_stbuf + X87_RING / 2, g_sti64p = g_sti64buf + X87_RING / 2;
#endif
    memcpy(X87_TOP, s->st, sizeof s->st);
    memcpy(X87_TOP_I64, s->st_i64, sizeof s->st_i64);
    g_fp_top = s->fp_top;
    g_fpu_cw = s->cw;
    g_backedges = s->backedges;
    g_cur_func = s->cur_func;
    g_alias_entry = 0;
    recomp_yield_hook = s->hook ? record_yield : NULL;
    memset(&g_run, 0, sizeof g_run);
    g_log_at = g_log_base;
}

static void leave(machine_t* m) {
    uint32_t regs[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    unsigned yields = g_run.yields;
    yield_t yield[64];
    memcpy(yield, g_run.yield, sizeof yield);
    memset(m, 0, sizeof *m);
    memcpy(m->regs, regs, sizeof regs);
    m->flags[0] = g_flag_k, m->flags[1] = g_flag_a, m->flags[2] = g_flag_b, m->flags[3] = g_flag_cf;
    m->cur_func = g_cur_func;
    m->backedges = g_backedges;
    m->alias = g_alias_entry;
    memcpy(m->st, X87_TOP, sizeof m->st);
    memcpy(m->st_i64, X87_TOP_I64, sizeof m->st_i64);
    m->fp_top = g_fp_top;
#ifdef RECOMP_X87_RING
    m->x87_position = (int)(g_stp - g_stbuf);
#endif
    m->cw = g_fpu_cw;
    m->yields = yields;
    memcpy(m->yield, yield, sizeof yield);
}

enum { SAME, NAN_PAYLOAD, DIFFERENT };

typedef struct {
    unsigned long same, nan_payload, different;
} tally_t;

/* [0] finite inputs, where the lifted code has one answer; [1] inputs with infinities or NaNs. */
static tally_t g_kernel[2], g_compiler[2];
static int g_non_finite;
static uint8_t g_alt[REGION_HI - REGION_LO];

static int is_nan(uint32_t x) { return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu); }

/* Memory the function leaves below the stack pointer is dead: only compared when g_dead_hi is 0. */
static uint32_t g_dead_lo, g_dead_hi;
static int dead(size_t i) { return REGION_LO + i >= g_dead_lo && REGION_LO + i < g_dead_hi; }

static void run(recomp_func_t f, const start_t* s, machine_t* m, uint8_t* memory) {
    enter(s);
    f();
    leave(m);
    if (memory) memcpy(memory, (void*)(uintptr_t)REGION_LO, REGION_HI - REGION_LO);
}

/* NAN_PAYLOAD: the machines agree and every differing word is a NaN in both, only its payload or sign
 * differs: which NaN operand an SSE2 operation returns follows the operand order the C compiler chose. */
static int classify(const machine_t* a, const uint8_t* ma, const machine_t* b, const uint8_t* mb) {
    int nan = 0;
    if (memcmp(a, b, sizeof *a)) return DIFFERENT;
    for (size_t i = 0; i < REGION_HI - REGION_LO; i += 4) {
        uint32_t x, y;
        memcpy(&x, ma + i, 4);
        memcpy(&y, mb + i, 4);
        if (x == y || dead(i)) continue;
        if (!is_nan(x) || !is_nan(y)) return DIFFERENT;
        nan = 1;
    }
    return nan ? NAN_PAYLOAD : SAME;
}

static void tally(tally_t* t, int result) {
    if (result == SAME) t->same++;
    else if (result == NAN_PAYLOAD) t->nan_payload++;
    else t->different++;
}

static void report(const char* what, unsigned long n, const machine_t* a, const uint8_t* ma, const machine_t* b,
                   const uint8_t* mb) {
    static unsigned reported;
    if (reported++ >= 10) return;
    printf("FAIL %s case %lu:", what, n);
    for (size_t i = 0; i < REGION_HI - REGION_LO; i += 4) {
        uint32_t x, y;
        memcpy(&x, ma + i, 4);
        memcpy(&y, mb + i, 4);
        if (x != y && !dead(i)) {
            printf(" memory at 0x%08X: lifted %08X, kernel %08X;", (unsigned)(REGION_LO + i), x, y);
            break;
        }
    }
    for (int i = 0; i < 8; i++)
        if (a->regs[i] != b->regs[i]) printf(" reg%d %08X/%08X", i, a->regs[i], b->regs[i]);
    for (int i = 0; i < 4; i++)
        if (a->flags[i] != b->flags[i]) printf(" flag%d %08X/%08X", i, a->flags[i], b->flags[i]);
    if (memcmp(a->st, b->st, sizeof a->st) || memcmp(a->st_i64, b->st_i64, sizeof a->st_i64) || a->fp_top != b->fp_top ||
        a->x87_position != b->x87_position)
        printf(" x87 state");
    if (a->backedges != b->backedges || a->yields != b->yields || memcmp(a->yield, b->yield, sizeof a->yield))
        printf(" backedges %u/%u yields %u/%u", a->backedges, b->backedges, a->yields, b->yields);
    if (a->cur_func != b->cur_func) printf(" g_cur_func %08X/%08X", a->cur_func, b->cur_func);
    printf("\n");
}

static void compare(const char* what, unsigned long n, recomp_func_t lifted, recomp_func_t lifted_alt,
                    recomp_func_t kernel, const start_t* s) {
    static machine_t a, b, c;
    int result;
    run(lifted, s, &a, g_after);
    run(lifted_alt, s, &c, g_alt);
    run(kernel, s, &b, NULL);
    tally(&g_compiler[g_non_finite], classify(&a, g_after, &c, g_alt));
    result = classify(&a, g_after, &b, (const uint8_t*)(uintptr_t)REGION_LO);
    tally(&g_kernel[g_non_finite], result);
    if (result != SAME && !g_non_finite) report(what, n, &a, g_after, &b, (const uint8_t*)(uintptr_t)REGION_LO);
    if (result == NAN_PAYLOAD && getenv("WATER_NAN_DETAIL")) {
        const uint8_t* mb = (const uint8_t*)(uintptr_t)REGION_LO;
        for (size_t i = 0; i < REGION_HI - REGION_LO; i += 4)
            if (memcmp(g_after + i, mb + i, 4)) {
                uint32_t x, y;
                memcpy(&x, g_after + i, 4);
                memcpy(&y, mb + i, 4);
                printf("NaN %s case %lu at 0x%08X: lifted %08X kernel %08X\n", what, n, (unsigned)(REGION_LO + i), x, y);
                break;
            }
    }
}

static const uint16_t k_control_words[] = { 0x0F7F, 0x027F, 0x067F, 0x0A7F };

static void shared_tables(int realistic) {
    for (uint32_t i = 0; i < 4096; i++) put32(INV_SQRT + i * 4, realistic ? rnd() & 0x007FFFFFu : rnd());
    for (uint32_t i = 0; i < ALPHA_ENTRIES; i++) *(uint16_t*)(uintptr_t)(ALPHA_TABLE + i * 2) = (uint16_t)rnd();
}

/* One CEntityWater row call: the node, its CSimWaveVar2D, heights, spans, the texture row, the arguments. */
static void rows_case(unsigned long n, int extremes, int hook, int width_max) {
    g_non_finite = extremes == NON_FINITE;
    uint32_t width = 3 + rnd_below((uint32_t)width_max), rows = 3 + rnd_below(12), stride = width + rnd_below(3);
    uint32_t pitch = rnd_below(4) ? stride : stride + rnd_below(5);
    uint32_t heights, spans, sim, node, dest, row;
    start_t s;
    g_arena = ARENA;
    heights = alloc((rows + 2) * (pitch > stride ? pitch : stride) * 4 + 64);
    for (uint32_t i = 0; i < (rows + 2) * (pitch > stride ? pitch : stride) + 16; i++)
        putf(heights + i * 4, rnd_float(-4.0f, 4.0f, extremes));
    spans = alloc(rows * 8);
    for (uint32_t r = 0; r < rows; r++) {
        int32_t first = (int32_t)rnd_below(width + 6) - 2, last = first + (int32_t)rnd_below(width + 6) - 4;
        put32(spans + r * 8, (uint32_t)first);
        put32(spans + r * 8 + 4, (uint32_t)last);
    }
    sim = alloc(0x100);
    for (uint32_t i = 0; i < 0x100; i += 4) put32(sim + i, rnd());
    putf(sim + 0x18, rnd_float(0.05f, 2.0f, extremes));
    putf(sim + 0x2C, rnd_float(0.05f, 2.0f, extremes));
    put32(sim + 0x38, width + 2 - rnd_below(3));
    put32(sim + 0x3C, stride);
    put32(sim + 0x44, heights);
    put32(sim + 0x50, pitch);
    put32(sim + 0x98, spans);
    node = alloc(16);
    put32(node, sim);
    dest = alloc((width + 16) * 2);
    for (uint32_t i = 0; i < width + 16; i++) *(uint16_t*)(uintptr_t)(dest + i * 2) = (uint16_t)rnd();
    row = rnd_below(rows);
    putf(ALPHA_OFFSET, extremes == NON_FINITE && rnd_below(40) == 0 ? as_float(0x7FC00000u) : rnd_range(-3.0f, 3.0f));
    putf(ALPHA_SCALE, extremes == NON_FINITE && rnd_below(40) == 0 ? as_float(0x7F800000u) : rnd_range(-5000.0f, 5000.0f));
    if (extremes == ORDINARY) {
        putf(ALPHA_OFFSET, 1.0f);
        putf(ALPHA_SCALE, rnd_range(1.0f, 30.0f));
    }
    for (uint32_t i = 0; i < 16; i++) put32(STACK_TOP - 0x100 + i * 4, rnd());
    put32(STACK_TOP, RECOMP_RETADDR);
    put32(STACK_TOP + 4, rnd());
    put32(STACK_TOP + 8, dest + 2);
    put32(STACK_TOP + 0xC, row);
    putf(STACK_TOP + 0x10, rnd_float(-50.0f, 50.0f, extremes));
    putf(STACK_TOP + 0x14, rnd_float(-1.0f, 1.0f, extremes));
    putf(STACK_TOP + 0x18, rnd_float(-1.0f, 1.0f, extremes));
    for (uint32_t i = 0x1C; i <= 0x28; i += 4) put32(STACK_TOP + i, rnd());
    memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
    random_start(&s, hook);
    s.regs[1] = node;
    s.cw = k_control_words[rnd_below(4)];
    compare("water_rows", n, lifted_sub_0060E3F0, lifted_alt_sub_0060E3F0, water_rows, &s);
}

/* One CSimWaveVar2D step: amplitudes, previous amplitudes (sometimes the same buffer), coefficients. */
static void wave_case(unsigned long n, int extremes, int hook, int width_max) {
    g_non_finite = extremes == NON_FINITE;
    uint32_t width = 3 + rnd_below((uint32_t)width_max), rows = 1 + rnd_below(10), stride = width + 2;
    uint32_t cells = (rows + 4) * stride, amp, prev, coef, spans, sim;
    int32_t first_row = 1 + (int32_t)rnd_below(2), last_row = first_row + (int32_t)rnd_below(rows + 1) - 1;
    start_t s;
    g_arena = ARENA;
    amp = alloc(cells * 4 + 32) + 16;
    prev = rnd_below(8) == 0 ? amp : alloc(cells * 4 + 32) + 16;
    coef = alloc(cells * 4 + 32) + 16;
    for (uint32_t i = 0; i < cells; i++) {
        putf(amp + i * 4, rnd_float(-2.0f, 2.0f, extremes));
        if (prev != amp) putf(prev + i * 4, rnd_float(-2.0f, 2.0f, extremes));
        putf(coef + i * 4, rnd_float(0.0f, 1.0f, extremes));
    }
    spans = alloc((rows + 4) * 8);
    for (uint32_t r = 0; r < rows + 4; r++) {
        int32_t first = 1 + (int32_t)rnd_below(width), last = first + (int32_t)rnd_below(width) - 2;
        if (last > (int32_t)width) last = (int32_t)width;
        put32(spans + r * 8, (uint32_t)first);
        put32(spans + r * 8 + 4, (uint32_t)last);
    }
    sim = alloc(0x100);
    for (uint32_t i = 0; i < 0x100; i += 4) put32(sim + i, rnd());
    putf(sim + 0x08, rnd_float(0.0f, 3.0f, extremes));
    putf(sim + 0x0C, rnd_float(0.0f, 1.0f, extremes));
    putf(sim + 0x20, rnd_divisor(0.01f, 1.0f, extremes));
    put32(sim + 0x44, amp);
    put32(sim + 0x50, stride);
    put32(sim + 0x58, prev);
    put32(sim + 0x7C, coef);
    put32(sim + 0x8C, (uint32_t)first_row);
    put32(sim + 0x90, (uint32_t)last_row);
    put32(sim + 0x98, spans);
    putf(DAMPING, rnd_float(0.1f, 2.0f, extremes));
    putf(NUMERATOR, rnd_float(0.5f, 4.0f, extremes));
    for (uint32_t i = 0; i < 16; i++) put32(STACK_TOP - 0x100 + i * 4, rnd());
    put32(STACK_TOP, RECOMP_RETADDR);
    putf(STACK_TOP + 4, rnd_divisor(0.001f, 0.1f, extremes));
    memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
    random_start(&s, hook);
    s.regs[1] = sim;
    s.cw = k_control_words[rnd_below(4)];
    compare("water_wave_step", n, lifted_sub_00638670, lifted_alt_sub_00638670, water_wave_step, &s);
}

/* An occluder set and a point array: planes either random or placed just behind every point (the result then
 * hangs on the last rounding, the epsilon and the sign of zero), so both answers come up. */
static unsigned long g_occluded, g_visible;

static uint32_t occluder_planes(uint32_t data, uint32_t points, uint32_t planes, int extremes) {
    uint32_t table = alloc(planes * 16 + 16);
    for (uint32_t p = 0; p < planes; p++) {
        float nx = rnd_float(-1.0f, 1.0f, extremes), ny = rnd_float(-1.0f, 1.0f, extremes),
              nz = rnd_float(-1.0f, 1.0f, extremes), d = rnd_float(-40.0f, 10.0f, extremes);
        if (rnd_below(3)) {
            float most = -1e30f;
            for (uint32_t i = 0; i < points; i++) {
                float px, py, pz, dot;
                memcpy(&px, (void*)(uintptr_t)(data + i * 12), 4);
                memcpy(&py, (void*)(uintptr_t)(data + i * 12 + 4), 4);
                memcpy(&pz, (void*)(uintptr_t)(data + i * 12 + 8), 4);
                dot = nz * pz + ny * py + nx * px;
                if (dot > most) most = dot;
            }
            d = -most - (rnd_below(4) ? rnd_range(0.0f, 0.5f) : 0.0f);
        }
        putf(table + p * 16, nx);
        putf(table + p * 16 + 4, ny);
        putf(table + p * 16 + 8, nz);
        putf(table + p * 16 + 12, d);
    }
    return table;
}

static void occlusion_setup(uint32_t occluders, uint32_t points, uint32_t planes_max, int extremes, int empty) {
    uint32_t data, array, items, set;
    g_arena = ARENA;
    data = alloc(points * 12 + 16);
    for (uint32_t i = 0; i < points * 3; i++) putf(data + i * 4, rnd_float(-10.0f, 10.0f, extremes));
    array = alloc(16);
    put32(array, points);
    put32(array + 4, data);
    items = alloc(occluders * 4 + 16);
    for (uint32_t o = 0; o < occluders; o++) {
        uint32_t occluder = alloc(0x40), planes = empty && rnd_below(6) == 0 ? 0 : 1 + rnd_below(planes_max);
        for (uint32_t k = 0; k < 0x40; k += 4) put32(occluder + k, rnd());
        put32(occluder + 0x28, planes);
        put32(occluder + 0x2C, occluder_planes(data, points, planes, extremes));
        put32(items + o * 4, occluder);
    }
    set = alloc(16);
    put32(set, occluders);
    put32(set + 4, items);
    *(uint8_t*)(uintptr_t)EPSILON_ON = (uint8_t)(rnd_below(2) ? rnd() : 0);
    putf(EPSILON_VALUE, rnd_float(-0.01f, 0.01f, extremes));
    put32(EPSILON_USED, rnd());
    put32(OCCLUDED_COUNT, rnd());
    put32(VISIBLE_COUNT, rnd());
    for (uint32_t i = 0; i < 16; i++) put32(STACK_TOP - 0x40 + i * 4, rnd());
    put32(STACK_TOP, RECOMP_RETADDR);
    put32(STACK_TOP + 4, set);
    put32(STACK_TOP + 8, array);
    put32(STACK_TOP + 0xC, rnd());
}

static void occlusion_case(unsigned long n, int extremes, int hook) {
    uint32_t occluders = rnd_below(10) ? 1 + rnd_below(6) : 0, points = rnd_below(10) ? 1 + rnd_below(12) : 0;
    start_t s;
    g_non_finite = extremes == NON_FINITE;
    occlusion_setup(occluders, points, 9, extremes, 1);
    memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
    random_start(&s, hook);
    s.cw = 0x027F;
    compare("occlusion_test", n, lifted_sub_00630D90, lifted_alt_sub_00630D90, occlusion_test, &s);
    if ((g_eax & 0xFF) == 1) g_occluded++;
    else g_visible++;
}

/* A polygon for the per-polygon submission: material, fog table, vertices and the states last sent, with the cases
 * that change its path (address modes, clamped z, lit above or below the threshold, fog saturated or negative). */
static unsigned long g_polygon_paths[8];
static uint32_t g_polygon_self, g_polygon_vertices;

static void polygon_setup(int extremes, uint32_t max_vertices) {
    uint32_t dev, vtable, mat, holder, owner, poly, list, self, table, entries, n;
    float threshold;
    g_arena = ARENA;
    g_log_base = alloc(0x4000);
    g_log_end = g_log_base + 0x4000;
    vtable = alloc(0xB0);
    for (uint32_t k = 0; k < 0xB0; k += 4) put32(vtable + k, rnd());
    put32(vtable + 0x58, FAKE_SET_RENDER_STATE);
    put32(vtable + 0x70, FAKE_DRAW_PRIMITIVE);
    put32(vtable + 0xA0, FAKE_SET_STAGE_STATE);
    dev = alloc(16);
    put32(dev, vtable);
    put32(P_DEVICE, dev);
    put32(P_FTOL_IMPORT, FAKE_FTOL);
    put32(P_MODE_TABLE, 0x00559268u);
    put32(P_MODE_TABLE + 4, 0x005592A5u);
    put32(P_MODE_TABLE + 8, 0x005592D6u);
    put32(P_MODE_TABLE + 12, 0x00559315u);
    *(uint8_t*)(uintptr_t)P_WRAP_U = (uint8_t)(rnd_below(4) ? rnd_below(2) : rnd());
    *(uint8_t*)(uintptr_t)P_WRAP_V = (uint8_t)(rnd_below(4) ? rnd_below(2) : rnd());
    *(uint8_t*)(uintptr_t)P_FOG_SENT = (uint8_t)(rnd_below(4) ? rnd_below(2) : rnd());
    *(uint8_t*)(uintptr_t)P_SHADE_SENT = (uint8_t)(rnd_below(4) ? 1 : rnd());
    *(uint8_t*)(uintptr_t)P_SPECULAR_SENT = (uint8_t)(rnd_below(4) ? 0 : rnd());
    putf(0x006474D0u, 0.0f);
    putf(0x006474D4u, 0.5f);
    putf(0x00645C34u, 1.0f);
    put32(0x0064C49Cu, 0x3F7FFFFEu);
    { double one = 1.0; memcpy((void*)(uintptr_t)0x00646570u, &one, 8); }
    put32(0x0064E70Cu, 0x3F800000u);
    putf(0x0064E710u, 0.15384616f);
    putf(0x0064DBCCu, 255.0f);
    putf(0x0064E708u, 0.99f);
    entries = rnd_below(16) ? 1 + rnd_below(64) : rnd_below(2);
    table = alloc(entries * 4 + 8);
    for (uint32_t i = 0; i < entries + 2; i++) putf(table + i * 4, rnd_float(0.0f, 1.2f, extremes));
    put32(P_FOG_ENTRIES, entries);
    put32(P_FOG_TABLE, table);
    putf(P_OFFSET_X, rnd_float(-1.0f, 1.0f, extremes));
    putf(P_OFFSET_Y, rnd_float(-1.0f, 1.0f, extremes));
    mat = alloc(0x60);
    for (uint32_t k = 0; k < 0x60; k += 4) put32(mat + k, rnd());
    put32(mat + 0x2C, rnd_below(8) ? rnd_below(200) : rnd_below(2));
    threshold = (float)(int32_t)get32(mat + 0x2C);
    put32(mat + 0x14, rnd_below(16) ? get32(mat + 0x2C) + 1 + rnd_below(300) : get32(mat + 0x2C) + rnd_below(2));
    putf(mat + 0x30, rnd_float(0.0f, 1.0f, extremes));
    put32(mat + 0x54, rnd_below(6) ? 0x7B000000u | rnd_below(0x10000) : 0);
    holder = alloc(16);
    put32(holder + 8, mat);
    owner = alloc(0x30);
    put32(owner + 0x2C, holder);
    n = g_polygon_vertices ? g_polygon_vertices : rnd_below(12) ? 3 + rnd_below(10) : rnd_below(max_vertices + 1);
    list = alloc(n * 4 + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = alloc(0x24);
        put32(list + i * 4, v);
        putf(v + 4, rnd_below(8) ? rnd_float(0.0f, 1.0f, extremes) : rnd_float(-0.5f, 1.5f, extremes));
        putf(v + 0xC, rnd_float(0.0f, 3440.0f, extremes));
        putf(v + 0x10, rnd_float(0.0f, 1440.0f, extremes));
        putf(v + 0x14, rnd_below(8) ? rnd_float(0.0f, 1.0f, extremes) : rnd_float(-0.5f, 1.5f, extremes));
        putf(v + 0x18, rnd_float(-0.2f * threshold, threshold * 2.0f + 2.0f, extremes));
        put32(v + 0x1C, rnd());
        put32(v + 0x20, rnd());
    }
    poly = alloc(0x80);
    for (uint32_t k = 0; k < 0x80; k += 4) put32(poly + k, rnd());
    put32(poly, n);
    put32(poly + 4, list);
    put32(poly + 0xC, owner);
    put32(poly + 0x78, rnd_below(8) ? rnd_below(4) : rnd_below(8));
    self = alloc(0x20);
    putf(self + 0x10, rnd_float(0.0f, 1.0f, extremes));
    for (uint32_t i = 0; i < 64; i++) put32(STACK_TOP - 0x100 + i * 4, rnd());
    put32(STACK_TOP, RECOMP_RETADDR);
    put32(STACK_TOP + 4, poly);
    put32(STACK_TOP + 8, rnd());
    g_polygon_paths[get32(poly + 0x78) & 7]++;
    g_polygon_self = self;
}

static void polygon_case(unsigned long n, int extremes, int hook, uint32_t max_vertices) {
    start_t s;
    uint32_t self;
    g_non_finite = extremes == NON_FINITE;
    polygon_setup(extremes, max_vertices);
    self = g_polygon_self;
    memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
    random_start(&s, hook);
    s.regs[1] = self;
    s.cw = 0x027F;
    g_dead_lo = DEAD_STACK, g_dead_hi = STACK_TOP;
    compare("polygon_submit", n, lifted_sub_00559240, lifted_alt_sub_00559240, polygon_submit, &s);
    g_dead_lo = g_dead_hi = 0;
    g_log_base = g_log_end = 0;
}

static double seconds(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* Time with the game's water texture shape: a 256-texel row, and 256x256 cells of simulation (eight
 * 256x128 steps). */
static float getf(uint32_t va) {
    float v;
    memcpy(&v, (const void*)(uintptr_t)va, 4);
    return v;
}

/* The wave model over a minute of game time at several frame rates: the real kernel stepped as CWaveData::Step (0x00636FB0)
 * steps it, with the physics' disturbances landing between water steps as in the game (10 ms steps plus the rest above
 * 2.5 ms, CXob::PD_Integrate; -0.015 times a box's vertical speed under each submerged element,
 * CSimWave2D::CreateDisturbance). Stepped once a frame its energy grows with the frame rate; paced by water_cadence it
 * stays what the 20 Hz frames the model was built for give it, at any rate. */
#define CAD_W 220
#define CAD_H 144
#define CAD_DX 0.184f
#define CAD_SOURCES 6
#define CAD_CHECKS 3

typedef struct {
    uint32_t sim;
    float step_max, required, timestore;
    uint64_t rng;
} cad_water_t;

typedef struct {
    double rms, grid, turning;
    float most;
    unsigned nonfinite;
} cad_stats_t;

static uint32_t cad_amp(const cad_water_t* w) { return get32(w->sim + 0x44); }
static uint32_t cad_vel(const cad_water_t* w) { return get32(w->sim + 0x7C); }
static float cad_get(uint32_t a, int x, int y) { return getf(a + (uint32_t)(y * CAD_W + x) * 4); }

static double cad_noise(cad_water_t* w) {
    double sum = 0;
    for (int i = 0; i < 12; i++) {
        w->rng ^= w->rng << 13, w->rng ^= w->rng >> 7, w->rng ^= w->rng << 17;
        sum += (double)(w->rng >> 11) / 9007199254740992.0;
    }
    return sum - 6.0;
}

static void cad_setup(cad_water_t* w) {
    uint32_t a, b, v, spans;
    float vmax = 0;
    g_arena = ARENA;
    a = alloc(CAD_W * CAD_H * 4);
    b = alloc(CAD_W * CAD_H * 4);
    v = alloc(CAD_W * CAD_H * 4);
    spans = alloc(CAD_H * 8);
    w->sim = alloc(0x100);
    memset((void*)(uintptr_t)w->sim, 0, 0x100);
    for (int y = 0; y < CAD_H; y++) {
        put32(spans + (uint32_t)y * 8, 1);
        put32(spans + (uint32_t)y * 8 + 4, CAD_W - 2);
        for (int x = 0; x < CAD_W; x++) {
            uint32_t i = (uint32_t)(y * CAD_W + x) * 4;
            float depth = (x < 30 ? x / 30.0f : 1.0f) * 0.3f, speed = sqrtf(9.8f * depth) / CAD_DX;
            float factor = x < 2 || y == 0 || x == CAD_W - 1 || y == CAD_H - 1 ? 0.0f : speed * speed;
            putf(a + i, 0.0f);
            putf(b + i, 0.0f);
            putf(v + i, factor);
            if (factor > vmax) vmax = factor;
        }
    }
    w->step_max = sqrtf(1.8f / (8.0f * vmax + 25.0f));
    putf(w->sim + 0x08, 0.1f);
    putf(w->sim + 0x0C, 25.0f);
    putf(w->sim + 0x1C, w->step_max);
    putf(w->sim + 0x20, w->step_max);
    put32(w->sim + 0x44, a);
    put32(w->sim + 0x50, CAD_W);
    put32(w->sim + 0x58, b);
    put32(w->sim + 0x7C, v);
    put32(w->sim + 0x8C, 1);
    put32(w->sim + 0x90, CAD_H - 2);
    put32(w->sim + 0x98, spans);
    putf(DAMPING, 0.5f);
    putf(NUMERATOR, 2.0f);
    w->required = w->timestore = 0.0f;
    w->rng = 0x9E3779B97F4A7C15ull;
}

/* CSimWaveVar2D::Iterate (the kernel), SwapPrev, sStepPrev. */
static void cad_iterate(cad_water_t* w, float dt) {
    uint32_t current = get32(w->sim + 0x44);
    recomp_yield_hook = NULL;
    put32(STACK_TOP, RECOMP_RETADDR);
    putf(STACK_TOP + 4, dt);
    g_esp = STACK_TOP;
    g_ecx = w->sim;
    water_wave_step();
    put32(w->sim + 0x44, get32(w->sim + 0x58));
    put32(w->sim + 0x58, current);
    putf(w->sim + 0x20, dt);
}

static void cad_step(cad_water_t* w, float s) {
    int n;
    if (s <= w->step_max * 0.001f) return;
    n = (int)(s / w->step_max) + 1;
    for (int i = 0; i < n; i++) cad_iterate(w, s / (float)n);
}

static void cad_disturb(cad_water_t* w, float mx, float my, float height) {
    const float radius = 0.25f, inc = CAD_DX / radius;
    uint32_t a = cad_amp(w), v = cad_vel(w);
    int x1 = (int)((mx - radius) / CAD_DX), x2 = (int)((mx + radius) / CAD_DX), y1 = (int)((my - radius) / CAD_DX),
        y2 = (int)((my + radius) / CAD_DX);
    float most = 0;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++)
            if (getf(v + (uint32_t)(y * CAD_W + x) * 4) != 0.0f) most = fmaxf(most, fabsf(cad_get(a, x, y)));
    if (0.4f - most <= 0.0f) return;
    if (height > 0.4f - most) height = 0.4f - most;
    if (height < most - 0.4f) height = most - 0.4f;
    for (int x = x1; x <= x2; x++) {
        double rx = (x * CAD_DX - mx) / radius;
        double vx = ((sin(3.14159265358979 * rx) - sin(3.14159265358979 * (rx + inc))) / 3.14159265358979 - inc) /
                    (4.0 * inc * inc);
        for (int y = y1; y <= y2; y++) {
            double ry = (y * CAD_DX - my) / radius, val;
            uint32_t at = a + (uint32_t)(y * CAD_W + x) * 4;
            val = vx * ((sin(3.14159265358979 * ry) - sin(3.14159265358979 * (ry + inc))) / 3.14159265358979 - inc);
            if (val <= 0 || getf(v + (uint32_t)(y * CAD_W + x) * 4) == 0.0f) continue;
            putf(at, getf(at) + (float)(val * height));
        }
    }
}

static void cad_physics(cad_water_t* w, float dt) {
    float t = dt + w->timestore;
    int steps = 0;
    w->timestore = 0.0f;
    while (t > 0.01f) {
        t -= 0.01f;
        steps++;
    }
    if (t > 0.0025f) steps++;
    else w->timestore = t;
    while (steps--)
        for (int s = 0; s < CAD_SOURCES; s++)
            cad_disturb(w, (8 + s * 4) * CAD_DX * 2.2f, (40 + s * 11) * CAD_DX, (float)(-0.015 * 2.0 * cad_noise(w)));
}

static cad_stats_t cad_stats(const cad_water_t* w) {
    cad_stats_t st = { 0, 0, 0, 0, 0 };
    uint32_t a = cad_amp(w);
    double sum2 = 0, lap2 = 0, turns = 0, inner = 0;
    for (int y = 2; y < CAD_H - 2; y++)
        for (int x = 32; x < CAD_W - 2; x++) {
            float h = cad_get(a, x, y), l = cad_get(a, x - 1, y), r = cad_get(a, x + 1, y);
            double lap = h - 0.25 * ((double)l + r + cad_get(a, x, y - 1) + cad_get(a, x, y + 1));
            if (!isfinite(h)) {
                st.nonfinite++;
                continue;
            }
            inner++;
            sum2 += (double)h * h;
            lap2 += lap * lap;
            if ((h - l) * (r - h) < 0) turns++;
            if (fabsf(h) > st.most) st.most = fabsf(h);
        }
    st.rms = inner ? sqrt(sum2 / inner) : 0;
    st.grid = inner ? sqrt(lap2 / inner) : 0;
    st.turning = inner ? turns / inner : 0;
    return st;
}

static uint32_t cad_hash(const cad_water_t* w) {
    const uint8_t* p = (const uint8_t*)(uintptr_t)cad_amp(w);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)CAD_W * CAD_H * 4; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* frames_ms: the frame times, repeated. paced: water_cadence decides the steps; else one Step per frame, as before. */
static uint32_t cad_run(const int* frames_ms, int count, int paced, double seconds, cad_stats_t at[CAD_CHECKS]) {
    cad_water_t w;
    double now = 0;
    int f = 0, check = 0;
    cad_setup(&w);
    while (check < CAD_CHECKS) {
        float dt = (float)frames_ms[f++ % count] / 1000.0f;
        cad_physics(&w, dt);
        if (paced) {
            water_cadence_t c;
            w.required += dt;
            c = water_cadence(w.required, getf(w.sim + 0x1C));
            if (c.steps > 0) cad_step(&w, (float)c.steps * c.step);
            w.required = c.carry;
        } else {
            cad_step(&w, dt);
        }
        now += dt;
        if (now >= seconds * (check + 1) / CAD_CHECKS) at[check++] = cad_stats(&w);
    }
    return cad_hash(&w);
}

static int cad_report(const char* name, const cad_stats_t* s) {
    printf("  %-24s rms %.5f, grid-scale %.5f, max |h| %.4f, turning %.2f, non-finite %u (at the end; grid-scale at "
           "1/3 %.5f)\n", name, s[CAD_CHECKS - 1].rms, s[CAD_CHECKS - 1].grid, s[CAD_CHECKS - 1].most,
           s[CAD_CHECKS - 1].turning, s[CAD_CHECKS - 1].nonfinite, s[0].grid);
    for (int i = 0; i < CAD_CHECKS; i++)
        if (s[i].nonfinite || !(s[i].most < 1.0f)) return 0;
    return 1;
}

static int within(double a, double b, double ratio) { return a <= b * ratio && b <= a * ratio; }

static int wave_cadence_cases(void) {
    static const int f20[] = { 50 }, f30[] = { 33, 33, 34 }, f60[] = { 15, 16, 16, 15, 16, 16, 15, 16, 16, 16 },
                     f120[] = { 8, 8, 9, 8, 8 }, f144[] = { 7, 7, 7, 6, 7, 7 };
    const double seconds = 60.0;
    cad_stats_t frame20[CAD_CHECKS], frame60[CAD_CHECKS], frame144[CAD_CHECKS], paced30[CAD_CHECKS],
        paced60[CAD_CHECKS], paced120[CAD_CHECKS], paced144[CAD_CHECKS], again[CAD_CHECKS];
    uint32_t h60, h60_again;
    int ok = 1;
    water_cadence_t c = water_cadence(0.016f, 0.05007f);
    /* the policy: whole steps of just under sStepMax, the rest carried, a long stall capped */
    ok &= c.steps == 0 && c.carry == 0.016f;
    c = water_cadence(0.06f, 0.05007f);
    ok &= c.steps == 1 && fabsf(c.step - 0.05007f * 0.999f) < 1e-7f && fabsf(c.carry - (0.06f - c.step)) < 1e-6f;
    c = water_cadence(10.0f, 0.05007f);
    ok &= c.steps == WATER_CATCH_UP_STEPS && c.carry == 0.0f;
    ok &= water_cadence(0.0f, 0.05f).steps < 0 && water_cadence(0.05f, 0.0f).steps < 0;
    printf("wave model, %.0f s of game time with the physics' disturbances (real kernel):\n", seconds);
    cad_run(f20, 1, 0, seconds, frame20);
    cad_run(f60, 10, 0, seconds, frame60);
    cad_run(f144, 6, 0, seconds, frame144);
    cad_run(f30, 3, 1, seconds, paced30);
    h60 = cad_run(f60, 10, 1, seconds, paced60);
    cad_run(f120, 5, 1, seconds, paced120);
    cad_run(f144, 6, 1, seconds, paced144);
    h60_again = cad_run(f60, 10, 1, seconds, again);
    ok &= cad_report("a step per frame, 20 Hz", frame20);
    ok &= cad_report("a step per frame, 60 Hz", frame60);
    ok &= cad_report("a step per frame, 144 Hz", frame144);
    ok &= cad_report("paced, 30 Hz", paced30);
    ok &= cad_report("paced, 60 Hz", paced60);
    ok &= cad_report("paced, 120 Hz", paced120);
    ok &= cad_report("paced, 144 Hz", paced144);
    /* the defect: per frame, the energy follows the frame rate */
    ok &= frame60[CAD_CHECKS - 1].rms > 2.0 * frame20[CAD_CHECKS - 1].rms;
    ok &= frame144[CAD_CHECKS - 1].rms > frame60[CAD_CHECKS - 1].rms;
    /* paced: the same water at every rate, the 20 Hz frames' water, and no slow build-up at the grid's scale */
    ok &= within(paced30[CAD_CHECKS - 1].rms, paced60[CAD_CHECKS - 1].rms, 1.3);
    ok &= within(paced120[CAD_CHECKS - 1].rms, paced60[CAD_CHECKS - 1].rms, 1.3);
    ok &= within(paced144[CAD_CHECKS - 1].rms, paced60[CAD_CHECKS - 1].rms, 1.3);
    ok &= within(paced60[CAD_CHECKS - 1].rms, frame20[CAD_CHECKS - 1].rms, 1.5);
    ok &= within(paced60[CAD_CHECKS - 1].grid, frame20[CAD_CHECKS - 1].grid, 1.5);
    for (int i = 1; i < CAD_CHECKS; i++) ok &= paced60[i].grid <= paced60[0].grid * 1.3;
    ok &= h60 == h60_again;
    printf("wave model pacing: %s (paced 60 Hz twice: %08X %08X)\n", ok ? "ok" : "FAIL", h60, h60_again);
    return ok;
}

static void benchmark(void) {
    double t0, lifted_rows, kernel_rows, lifted_wave, kernel_wave;
    start_t s;
    int calls = 400;
    {
        uint32_t width = 256, heights, spans, sim, node, dest;
        g_arena = ARENA;
        heights = alloc(260 * 260 * 4);
        for (uint32_t i = 0; i < 260 * 260; i++) putf(heights + i * 4, rnd_range(-1.0f, 1.0f));
        spans = alloc(8);
        put32(spans, 4);
        put32(spans + 4, width - 6);
        sim = alloc(0x100);
        putf(sim + 0x18, 0.5f);
        putf(sim + 0x2C, 0.5f);
        put32(sim + 0x38, width);
        put32(sim + 0x3C, 260);
        put32(sim + 0x44, heights + 260 * 4);
        put32(sim + 0x50, 260);
        put32(sim + 0x98, spans);
        node = alloc(16);
        put32(node, sim);
        dest = alloc(600);
        putf(ALPHA_OFFSET, 1.0f);
        putf(ALPHA_SCALE, 15.5f);
        put32(STACK_TOP + 8, dest);
        put32(STACK_TOP + 0xC, 0);
        putf(STACK_TOP + 0x10, 10.0f);
        putf(STACK_TOP + 0x14, 0.3f);
        putf(STACK_TOP + 0x18, -0.2f);
        memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
        random_start(&s, 0);
        s.regs[1] = node;
        s.cw = 0x0F7F;
        enter(&s);
        t0 = seconds();
        for (int i = 0; i < calls; i++) {
            g_esp = STACK_TOP;
            g_ecx = node;
            put32(STACK_TOP + 8, dest);
            put32(STACK_TOP + 0xC, 0);
            lifted_sub_0060E3F0();
        }
        lifted_rows = (seconds() - t0) / calls;
        enter(&s);
        t0 = seconds();
        for (int i = 0; i < calls; i++) {
            g_esp = STACK_TOP;
            g_ecx = node;
            put32(STACK_TOP + 8, dest);
            put32(STACK_TOP + 0xC, 0);
            water_rows();
        }
        kernel_rows = (seconds() - t0) / calls;
    }
    {
        uint32_t amp, prev, coef, spans, sim, stride = 258;
        g_arena = ARENA;
        amp = alloc(stride * 132 * 4);
        prev = alloc(stride * 132 * 4);
        coef = alloc(stride * 132 * 4);
        for (uint32_t i = 0; i < stride * 132; i++) {
            putf(amp + i * 4, rnd_range(-0.1f, 0.1f));
            putf(prev + i * 4, rnd_range(-0.1f, 0.1f));
            putf(coef + i * 4, rnd_range(0.0f, 1.0f));
        }
        spans = alloc(132 * 8);
        for (uint32_t r = 0; r < 132; r++) {
            put32(spans + r * 8, 1);
            put32(spans + r * 8 + 4, 256);
        }
        sim = alloc(0x100);
        putf(sim + 0x08, 1.0f);
        putf(sim + 0x0C, 0.1f);
        putf(sim + 0x20, 0.2f);
        put32(sim + 0x44, amp);
        put32(sim + 0x50, stride);
        put32(sim + 0x58, prev);
        put32(sim + 0x7C, coef);
        put32(sim + 0x8C, 1);
        put32(sim + 0x90, 128);
        put32(sim + 0x98, spans);
        putf(DAMPING, 1.0f);
        putf(NUMERATOR, 2.0f);
        putf(STACK_TOP + 4, 0.03f);
        memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
        random_start(&s, 0);
        s.regs[1] = sim;
        enter(&s);
        t0 = seconds();
        for (int i = 0; i < 8; i++) {
            g_esp = STACK_TOP;
            g_ecx = sim;
            putf(STACK_TOP + 4, 0.03f);
            lifted_sub_00638670();
        }
        lifted_wave = (seconds() - t0) / 4;
        enter(&s);
        t0 = seconds();
        for (int i = 0; i < 8; i++) {
            g_esp = STACK_TOP;
            g_ecx = sim;
            putf(STACK_TOP + 4, 0.03f);
            water_wave_step();
        }
        kernel_wave = (seconds() - t0) / 4;
    }
    printf("benchmark: 256-texel row lifted %.2f us, kernel %.2f us (x%.1f); 256x256 step lifted %.2f ms, kernel %.2f ms "
           "(x%.1f)\n", lifted_rows * 1e6, kernel_rows * 1e6, lifted_rows / kernel_rows, lifted_wave * 1e3,
           kernel_wave * 1e3, lifted_wave / kernel_wave);
}

/* A box's eight corners behind the six planes of the first occluder: every plane of every point is tested. */
static void occlusion_benchmark(void) {
    double t0, lifted, kernel;
    start_t s;
    int calls = 20000;
    uint32_t array;
    occlusion_setup(4, 8, 6, ORDINARY, 0);
    array = MEM32(STACK_TOP + 8);
    for (uint32_t i = 0; i < 8 * 3; i++) putf(MEM32(MEM32(STACK_TOP + 8) + 4) + i * 4, (float)(i % 5) - 2.0f);
    for (uint32_t o = 0; o < 4; o++) {
        uint32_t occluder = MEM32(MEM32(MEM32(STACK_TOP + 4) + 4) + o * 4), table = alloc(6 * 16);
        put32(occluder + 0x28, 6);
        put32(occluder + 0x2C, table);
        for (uint32_t p = 0; p < 6; p++) {
            putf(table + p * 16, p == 0 ? 1.0f : 0.0f);
            putf(table + p * 16 + 4, p == 1 ? 1.0f : 0.0f);
            putf(table + p * 16 + 8, p == 2 ? 1.0f : 0.0f);
            putf(table + p * 16 + 12, -100.0f);
        }
    }
    memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
    random_start(&s, 0);
    s.cw = 0x027F;
    enter(&s);
    t0 = seconds();
    for (int i = 0; i < calls; i++) {
        g_esp = STACK_TOP;
        put32(STACK_TOP + 8, array);
        lifted_sub_00630D90();
    }
    lifted = (seconds() - t0) / calls;
    enter(&s);
    t0 = seconds();
    for (int i = 0; i < calls; i++) {
        g_esp = STACK_TOP;
        put32(STACK_TOP + 8, array);
        occlusion_test();
    }
    kernel = (seconds() - t0) / calls;
    printf("benchmark: occlusion test (8 corners x 6 planes) lifted %.3f us, kernel %.3f us (x%.1f)\n",
           lifted * 1e6, kernel * 1e6, lifted / kernel);
}

static unsigned long g_polygon_fallbacks;

static void counted_polygon_fallback(void) {
    g_polygon_fallbacks++;
    lifted_sub_00559240();
}

/* A ten-vertex fan, lit below the threshold, with fog: the common case in the game. */
static void polygon_benchmark(void) {
    double t0, lifted, kernel;
    start_t s;
    int calls = 20000;
    uint32_t self, poly;
    g_polygon_vertices = 10;
    polygon_setup(ORDINARY, 10);
    g_polygon_vertices = 0;
    poly = get32(STACK_TOP + 4);
    if (get32(poly + 0x78) > 3) put32(poly + 0x78, 1);
    self = g_polygon_self;
    memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
    random_start(&s, 0);
    s.regs[1] = self;
    s.cw = 0x027F;
    polygon_submit_lifted = lifted_sub_00559240;
    enter(&s);
    t0 = seconds();
    for (int i = 0; i < calls; i++) {
        g_esp = STACK_TOP, g_ecx = self, g_log_at = g_log_base;
        lifted_sub_00559240();
    }
    lifted = (seconds() - t0) / calls;
    enter(&s);
    g_polygon_fallbacks = 0;
    polygon_submit_lifted = counted_polygon_fallback;
    t0 = seconds();
    for (int i = 0; i < calls; i++) {
        g_esp = STACK_TOP, g_ecx = self, g_log_at = g_log_base;
        polygon_submit();
    }
    kernel = (seconds() - t0) / calls;
    polygon_submit_lifted = lifted_sub_00559240;
    printf("benchmark: polygon submission (10 vertices) lifted %.3f us, host %.3f us (x%.1f); host path %d of %d\n",
           lifted * 1e6, kernel * 1e6, lifted / kernel, calls - (int)g_polygon_fallbacks, calls);
    g_log_base = g_log_end = 0;
}

/* The bump scanline routine (SelfMod 0x00640FE0) run natively from the game's own bytes, against the span model
 * (bump_linear_spans), the guest-order texel walk and the shader's closed form, pixel for pixel. */
#define SELFMOD_VA 0x0063B000u
#define B_TEXTURE_BITMAP 0x00737E88u
#define B_SUB_BUMP 0x00737E98u
#define B_LIGHT_THETA 0x00737410u
#define B_CLUT 0x00737F00u
#define B_TILE_MASK 0x006699F8u
#define B_TILE_STEP_U 0x006699FCu
#define B_TEXTURE_LINE 0x00737F34u
#define B_DELTA 0x00737EA0u
#define B_NEG_DU 0x00737E90u
#define B_NEG_DV 0x00737E40u
#define B_SCREEN 0x00737EC0u
#define B_SCREEN_LINE 0x00737EC4u
#define B_EVEN_ROWS 0x00737F68u
#define B_CLAMP_UV 0x00737F64u
#define B_SCREEN_W 640u
#define B_ROWS 48u

static int load_selfmod(void) {
    FILE* f = fopen(WATER_GAME_EXE, "rb");
    uint8_t head[0x400];
    uint32_t pe, sections, optional;
    DWORD old;
    int ok = 0;
    if (!f) return 0;
    if (fread(head, 1, sizeof head, f) == sizeof head) {
        memcpy(&pe, head + 0x3C, 4);
        sections = *(uint16_t*)(head + pe + 6);
        optional = *(uint16_t*)(head + pe + 20);
        for (uint32_t i = 0; i < sections && pe + 24 + optional + 40 * (i + 1) <= sizeof head; i++) {
            const uint8_t* s = head + pe + 24 + optional + 40 * i;
            uint32_t va, raw_size, raw;
            memcpy(&va, s + 12, 4);
            memcpy(&raw_size, s + 16, 4);
            memcpy(&raw, s + 20, 4);
            if (memcmp(s, "SelfMod", 7) || 0x00400000u + va != SELFMOD_VA) continue;
            ok = !fseek(f, (long)raw, SEEK_SET) && fread((void*)(uintptr_t)SELFMOD_VA, 1, raw_size, f) == raw_size &&
                 VirtualProtect((void*)(uintptr_t)SELFMOD_VA, raw_size, PAGE_EXECUTE_READWRITE, &old);
        }
    }
    fclose(f);
    return ok;
}

typedef struct {
    uint32_t screen, texels, texel_count, width;
    int bytes_texels; /* 1: the 8-bit textured routine */
    bump_walk_t walk;
    bump_pixel_t pixel;
    int written_outside;
    unsigned long spans, pixels, shader_differ;
} bump_case_t;

static void bump_reference_span(void* context, const bump_span_t* s) {
    bump_case_t* c = (bump_case_t*)context;
    bump_walk_state_t w = s->start;
    for (int32_t k = 0; k < s->n; k++) {
        uint16_t out;
        uint32_t texel, shader;
        if (w.index >= c->texel_count || s->x0 + k < 0 || s->x0 + k >= (int32_t)c->width) {
            c->written_outside = 1;
            return;
        }
        if (c->bytes_texels) {
            out = c->pixel.clut[c->pixel.clut_base + ((const uint8_t*)(uintptr_t)c->texels)[w.index]];
            *(uint16_t*)(uintptr_t)(c->screen + 2u * (s->dst_index + (uint32_t)k)) = out;
            shader = texture_shader_pixel(s, s->x0 + k, &c->walk, (const uint8_t*)(uintptr_t)c->texels, c->pixel.clut,
                                          c->pixel.clut_base);
        } else {
            texel = ((const uint16_t*)(uintptr_t)c->texels)[w.index];
            if (bump_texel_rgb565(&c->pixel, (uint16_t)texel, 0, &out))
                *(uint16_t*)(uintptr_t)(c->screen + 2u * (s->dst_index + (uint32_t)k)) = out;
            shader = bump_shader_pixel(s, s->x0 + k, &c->walk, (const uint16_t*)(uintptr_t)c->texels, &c->pixel, 0);
        }
        if (shader != out) c->shader_differ++;
        c->pixels++;
        bump_walk_step(&w, &c->walk);
    }
    c->spans++;
}

static void bump_bounds_span(void* context, const bump_span_t* s) {
    bump_case_t* c = (bump_case_t*)context;
    bump_walk_state_t w = s->start;
    if (s->x0 < 0 || s->x0 + s->n > (int32_t)c->width) c->written_outside = 1;
    for (int32_t k = 0; k < s->n && !c->written_outside; k++) {
        if (w.index >= c->texel_count) c->written_outside = 1;
        bump_walk_step(&w, &c->walk);
    }
}

/* A sub-triangle the primitive could hand over: edges, the U/V walk, and a tiled or untiled texel page. */
static void bump_setup(bump_case_t* c, uint32_t* pscan, uint32_t* pdtri) {
    uint32_t edge, clut, sub, w = 8u << rnd_below(6), h = 8u << rnd_below(6), tiled = rnd_below(3) != 0;
    uint32_t rows = 1 + rnd_below(B_ROWS - 1), y0 = rnd_below(4);
    g_arena = ARENA;
    c->width = B_SCREEN_W;
    c->screen = alloc(B_SCREEN_W * (B_ROWS + 8) * 2);
    for (uint32_t i = 0; i < B_SCREEN_W * (B_ROWS + 8) / 2; i++) put32(c->screen + i * 4, rnd());
    c->texel_count = 512u * (tiled ? h : 256u);
    c->texels = alloc(c->texel_count * 2);
    for (uint32_t i = 0; i < c->texel_count / 2; i++) put32(c->texels + i * 4, rnd());
    clut = alloc(0x5000 * 2);
    for (uint32_t i = 0; i < 0x5000 / 2; i++) put32(clut + i * 4, rnd());
    sub = alloc(1024);
    for (uint32_t i = 0; i < 1024; i++) *(uint8_t*)(uintptr_t)(sub + i) = (uint8_t)(rnd_below(8) ? rnd_below(32) : rnd_below(64));
    c->pixel.clut = (const uint16_t*)(uintptr_t)clut;
    c->pixel.clut_base = (rnd_below(8) << 11);
    c->pixel.bump = (const uint8_t*)(uintptr_t)sub;
    c->pixel.light_theta = rnd() & 0x3F0u;
    put32(B_TEXTURE_BITMAP, c->texels);
    put32(B_SUB_BUMP, sub);
    put32(B_LIGHT_THETA, c->pixel.light_theta);
    put32(B_CLUT, clut + 2 * c->pixel.clut_base);
    put32(B_TILE_MASK, tiled ? (h - 1) << 9 | (w - 1) : 0xFFFFFFFFu);
    put32(B_TILE_STEP_U, tiled ? w - 1 : 0xFFFFFFFFu);
    put32(B_TEXTURE_LINE, 512);
    put32(B_CLAMP_UV, tiled ? 0 : 1);
    put32(B_SCREEN, c->screen);
    put32(B_SCREEN_LINE, B_SCREEN_W);
    put32(B_EVEN_ROWS, rnd_below(6) ? 0 : 1);
    {
        int32_t du_int = (int32_t)rnd_below(5) - 2, dv_int = (int32_t)rnd_below(3) - 1;
        uint32_t du_frac = rnd() & 0xFFFF0000u, dv_frac = rnd() & 0xFFFF0000u;
        int32_t step = (tiled ? (du_int & (int32_t)(w - 1)) : du_int) + dv_int * 512;
        put32(B_DELTA, (uint32_t)(step + 512));
        put32(B_DELTA + 4, (uint32_t)step);
        put32(B_DELTA + 8, du_frac);
        put32(B_DELTA + 12, dv_frac);
        put32(B_NEG_DU, rnd());
        put32(B_NEG_DU + 4, (uint32_t)((int32_t)rnd_below(5) - 2));
        put32(B_NEG_DV, rnd());
        put32(B_NEG_DV + 4, (uint32_t)(((int32_t)rnd_below(3) - 1) * 512));
    }
    edge = alloc(0x60);
    for (uint32_t i = 0; i < 0x60; i += 4) put32(edge + i, rnd());
    put32(edge + 0x38, (uint32_t)((int32_t)rnd_below(0x30000) - 0x18000));
    put32(edge + 0x3C, (uint32_t)((int32_t)rnd_below(0x20000) - 0x10000));
    put32(edge + 0x38 + 0x10, rnd());
    put32(edge + 0x38 + 0x14, (uint32_t)((int32_t)rnd_below(3) - 1));
    put32(edge + 0x38 + 0x18, rnd());
    put32(edge + 0x38 + 0x1C, (uint32_t)(((int32_t)rnd_below(3) - 1) * 512));
    *pscan = alloc(0x40);
    for (uint32_t i = 0; i < 0x40; i += 4) put32(*pscan + i, rnd());
    put32(*pscan, (200u + rnd_below(200)) << 16 | rnd_below(0x10000));
    put32(*pscan + 4, rnd_below(0x10000));
    put32(*pscan + 0x10, rnd());
    put32(*pscan + 0x14, tiled ? rnd_below(1024) : 4096u + rnd_below(256));
    put32(*pscan + 0x18, rnd());
    put32(*pscan + 0x1C, tiled ? rnd_below(h) * 512u : (64u + rnd_below(64)) * 512u);
    put32(*pscan + 0x20, 512);
    *pdtri = alloc(0x60);
    for (uint32_t i = 0; i < 0x60; i += 4) put32(*pdtri + i, rnd());
    put32(*pdtri, y0);
    put32(*pdtri + 4, y0 + rows);
    put32(*pdtri + 8, y0 * B_SCREEN_W);
    put32(*pdtri + 0xC, (uint32_t)((int32_t)rnd_below(0x500000) - 0x80000));
    put32(*pdtri + 0x10, (uint32_t)((int32_t)rnd_below(0x40000) - 0x20000));
    put32(*pdtri + 0x58, edge);
    bump_walk_from_guest(&c->walk);
}

static int bump_native_cases(unsigned long cases, int bytes_texels) {
    typedef void(__cdecl * scanline_fn)(uint32_t, uint32_t);
    static uint8_t before[0x80000], native[0x80000];
    uint32_t va = bytes_texels ? TEXTURE_LINEAR_SCANLINE_VA : BUMP_LINEAR_SCANLINE_VA;
    scanline_fn native_fn = (scanline_fn)(uintptr_t)va;
    unsigned long ran = 0, differ = 0, state_differ = 0, skipped = 0, spans = 0, pixels = 0, shader_differ = 0;
    if (!load_selfmod()) {
        printf("bump scanlines: SKIP (%s not readable)\n", WATER_GAME_EXE);
        return 1;
    }
    while (ran < cases && skipped < cases * 20) {
        bump_case_t c;
        uint32_t pscan, pdtri, bytes;
        memset(&c, 0, sizeof c);
        c.bytes_texels = bytes_texels;
        bump_setup(&c, &pscan, &pdtri);
        bytes = g_arena - ARENA;
        memcpy(before, (void*)(uintptr_t)ARENA, bytes);
        bump_linear_spans(pscan, pdtri, &c.walk, bump_bounds_span, &c);
        memcpy((void*)(uintptr_t)ARENA, before, bytes);
        if (c.written_outside) {
            skipped++;
            continue;
        }
        native_fn(pscan, pdtri);
        memcpy(native, (void*)(uintptr_t)ARENA, bytes);
        memcpy((void*)(uintptr_t)ARENA, before, bytes);
        bump_linear_spans(pscan, pdtri, &c.walk, bump_reference_span, &c);
        if (memcmp((void*)(uintptr_t)c.screen, native + (c.screen - ARENA), B_SCREEN_W * (B_ROWS + 8) * 2)) {
            if (differ++ < 5) printf("FAIL bump case %lu: pixels differ from the native routine\n", ran);
        }
        if (memcmp((void*)(uintptr_t)pscan, native + (pscan - ARENA), 0x40) ||
            memcmp((void*)(uintptr_t)pdtri, native + (pdtri - ARENA), 0x60)) {
            if (state_differ++ < 5) printf("FAIL bump case %lu: scanline state differs from the native routine\n", ran);
        }
        spans += c.spans;
        pixels += c.pixels;
        shader_differ += c.shader_differ;
        ran++;
    }
    printf("%s scanlines vs native 0x%08X: %lu cases (%lu spans, %lu pixels): pixels differ in %lu, state in %lu; "
           "shader closed form differs in %lu pixels\n", bytes_texels ? "8-bit textured" : "bump", va, ran, spans,
           pixels, differ, state_differ, shader_differ);
    return !differ && !state_differ && !shader_differ && ran == cases;
}

/* An alias entry (a lifted label entered from outside) runs the lifted body. */
static int alias_reaches_lifted(void) {
    int called;
    water_rows_lifted = record_yield;
    g_run.yields = 0;
    g_alias_entry = 0x0060E42Fu;
    water_rows();
    called = g_run.yields == 1;
    g_alias_entry = 0;
    water_rows_lifted = NULL;
    if (!called) printf("FAIL alias entry did not reach the lifted body\n");
    occlusion_test_lifted = record_yield;
    g_run.yields = 0;
    g_alias_entry = 0x00630DAFu;
    occlusion_test();
    if (g_run.yields != 1) {
        printf("FAIL occlusion alias entry did not reach the lifted body\n");
        called = 0;
    }
    g_alias_entry = 0;
    occlusion_test_lifted = NULL;
    polygon_submit_lifted = record_yield;
    g_run.yields = 0;
    g_alias_entry = 0x005593A3u;
    polygon_submit();
    if (g_run.yields != 1) {
        printf("FAIL polygon submission alias entry did not reach the lifted body\n");
        called = 0;
    }
    g_alias_entry = 0;
    polygon_submit_lifted = NULL;
    return called;
}

/* The numbers the real kernel produces for a known wave field: what changes in the texture, and what the observer is given
 * (the wave heights' differences), so the refraction's input can be read off exactly. */
static struct {
    unsigned long n;
    double a_min, a_max, b_min, b_max;
} g_dump;

static void dump_observer(uintptr_t texel, double a, double b) {
    (void)texel;
    if (!g_dump.n || a < g_dump.a_min) g_dump.a_min = a;
    if (!g_dump.n || a > g_dump.a_max) g_dump.a_max = a;
    if (!g_dump.n || b < g_dump.b_min) g_dump.b_min = b;
    if (!g_dump.n || b > g_dump.b_max) g_dump.b_max = b;
    g_dump.n++;
}

static int water_dump(void) {
    enum { W = 256, H = 256, STRIDE = 260, FIRST = 4, LAST = W - 6 };
    const float amplitude = 0.05f, wavelength = 24.0f;
    uint32_t heights, spans, sim, node, dest;
    double want_a_min = 1e30, want_a_max = -1e30, want_b_min = 1e30, want_b_max = -1e30;
    unsigned long changed = 0, distinct = 0, hist[65536 / 64] = { 0 };
    int ok = 1;
    start_t st;
    g_arena = ARENA;
    heights = alloc(STRIDE * STRIDE * 4);
    for (int y = 0; y < STRIDE; y++)
        for (int x = 0; x < STRIDE; x++)
            putf(heights + (uint32_t)(y * STRIDE + x) * 4,
                 amplitude * sinf(6.2831853f * (float)x / wavelength) * cosf(6.2831853f * (float)y / (wavelength * 1.5f)));
    spans = alloc(H * 8);
    for (uint32_t r = 0; r < H; r++) {
        put32(spans + r * 8, FIRST);
        put32(spans + r * 8 + 4, LAST);
    }
    sim = alloc(0x100);
    putf(sim + 0x18, 0.5f);
    putf(sim + 0x2C, 0.5f);
    put32(sim + 0x38, W);
    put32(sim + 0x3C, STRIDE);
    put32(sim + 0x44, heights);
    put32(sim + 0x50, STRIDE);
    put32(sim + 0x98, spans);
    node = alloc(16);
    put32(node, sim);
    dest = alloc(H * W * 2 + 64);
    memset((void*)(uintptr_t)dest, 0xFF, H * W * 2);
    putf(ALPHA_OFFSET, 1.0f);
    putf(ALPHA_SCALE, 7.5f);
    putf(STACK_TOP + 0x10, 10.0f);
    memcpy(g_initial, (void*)(uintptr_t)REGION_LO, sizeof g_initial);
    random_start(&st, 0);
    st.regs[1] = node;
    st.cw = 0x0F7F;
    enter(&st);
    water_rows_observer = dump_observer;
    memset(&g_dump, 0, sizeof g_dump);
    for (uint32_t row = 0; row < H - 6; row++) {
        g_esp = STACK_TOP;
        g_ecx = node;
        put32(STACK_TOP + 8, dest + row * W * 2 + 2);
        put32(STACK_TOP + 0xC, row);
        putf(STACK_TOP + 0x10, 10.0f);
        putf(STACK_TOP + 0x14, 0.3f);
        putf(STACK_TOP + 0x18, -0.2f);
        water_rows();
        for (int x = FIRST; x < LAST; x++) {
            double h = *(const float*)(uintptr_t)(heights + ((uint32_t)row * STRIDE + (uint32_t)x) * 4);
            double ha = *(const float*)(uintptr_t)(heights + ((uint32_t)row * STRIDE + (uint32_t)x + 1) * 4);
            double hb = *(const float*)(uintptr_t)(heights + ((uint32_t)(row + 1) * STRIDE + (uint32_t)x) * 4);
            if (h - ha < want_a_min) want_a_min = h - ha;
            if (h - ha > want_a_max) want_a_max = h - ha;
            if (h - hb < want_b_min) want_b_min = h - hb;
            if (h - hb > want_b_max) want_b_max = h - hb;
        }
    }
    water_rows_observer = NULL;
    for (unsigned long i = 0; i < (unsigned long)H * W; i++) {
        uint16_t word = *(const uint16_t*)(uintptr_t)(dest + i * 2);
        if (word != 0xFFFF) {
            changed++;
            if (!hist[word / 64]++) distinct++;
        }
    }
    printf("water_rows on a known wave field (%.2f amplitude, %.0f cells): %lu texels changed, %lu distinct 64-code buckets, "
           "observer %lu texels\n  a %.9g..%.9g (expected %.9g..%.9g)\n  b %.9g..%.9g (expected %.9g..%.9g)\n", amplitude,
           wavelength, changed, distinct, g_dump.n, g_dump.a_min, g_dump.a_max, want_a_min, want_a_max, g_dump.b_min,
           g_dump.b_max, want_b_min, want_b_max);
    ok = changed > 0 && g_dump.n > 0 && fabs(g_dump.a_min - want_a_min) < 1e-9 && fabs(g_dump.a_max - want_a_max) < 1e-9 &&
         fabs(g_dump.b_min - want_b_min) < 1e-9 && fabs(g_dump.b_max - want_b_max) < 1e-9 && want_a_max > 0.0;
    if (!ok) printf("water_rows numeric dump: FAILED\n");
    return ok;
}

void sub_00626BA0(void);

int main(int argc, char** argv) {
    int quick = argc > 1 && !strcmp(argv[1], "--quick"), alias, bump, paced, dump;
    unsigned long per_kind = quick ? 300 : 3000;
    uintptr_t lo = (uintptr_t)g_window, hi = lo + sizeof g_window;
    test_profile_activate();
    GSUB(0x00626BA0) = sub_00626BA0;
    GSUB(0x0050C9C0) = sub_0050C9C0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (lo > P_MODE_TABLE || hi < REGION_HI) {
        printf("guest window 0x%08X-0x%08X does not cover 0x%08X-0x%08X: link at /BASE:0x00400000 /FIXED\n",
               (unsigned)lo, (unsigned)hi, REGION_LO, REGION_HI);
        return 2;
    }
    shared_tables(1);
    for (unsigned long n = 0; n < per_kind; n++) rows_case(n, ORDINARY, 0, 40);
    for (unsigned long n = 0; n < per_kind; n++) rows_case(n, FINITE_EXTREMES, (int)(n & 1), 300);
    for (unsigned long n = 0; n < per_kind; n++) rows_case(n, NON_FINITE, (int)(n & 1), 300);
    shared_tables(0);
    for (unsigned long n = 0; n < per_kind / 4; n++) rows_case(n, NON_FINITE, 1, 64);
    for (unsigned long n = 0; n < per_kind; n++) wave_case(n, ORDINARY, 0, 40);
    for (unsigned long n = 0; n < per_kind; n++) wave_case(n, FINITE_EXTREMES, (int)(n & 1), 300);
    for (unsigned long n = 0; n < per_kind; n++) wave_case(n, NON_FINITE, (int)(n & 1), 300);
    for (unsigned long n = 0; n < per_kind * 2; n++) occlusion_case(n, ORDINARY, (int)(n & 1));
    for (unsigned long n = 0; n < per_kind; n++) occlusion_case(n, FINITE_EXTREMES, (int)(n & 1));
    for (unsigned long n = 0; n < per_kind; n++) occlusion_case(n, NON_FINITE, (int)(n & 1));
    polygon_submit_lifted = lifted_sub_00559240;
    for (unsigned long n = 0; n < per_kind * 2; n++) polygon_case(n, ORDINARY, (int)(n & 1), 24);
    for (unsigned long n = 0; n < per_kind; n++) polygon_case(n, FINITE_EXTREMES, (int)(n & 1), 200);
    for (unsigned long n = 0; n < per_kind; n++) polygon_case(n, NON_FINITE, (int)(n & 1), 24);
    alias = alias_reaches_lifted();
    printf("occlusion cases: %lu occluded, %lu visible\n", g_occluded, g_visible);
    printf("polygon cases by address mode: %lu %lu %lu %lu, other %lu\n", g_polygon_paths[0], g_polygon_paths[1],
           g_polygon_paths[2], g_polygon_paths[3],
           g_polygon_paths[4] + g_polygon_paths[5] + g_polygon_paths[6] + g_polygon_paths[7]);
    printf("finite inputs, kernels vs lifted (%s): %lu identical, %lu different\n", WATER_GEN_OPT, g_kernel[0].same,
           g_kernel[0].nan_payload + g_kernel[0].different);
    printf("finite inputs, lifted %s vs the same lifted C at %s: %lu identical, %lu different\n", WATER_GEN_OPT,
           WATER_ALT_OPT, g_compiler[0].same, g_compiler[0].nan_payload + g_compiler[0].different);
    printf("inputs with infinities/NaNs, where the result depends on which NaN an operation returns, a choice the C "
           "compiler makes:\n");
    printf("  kernels vs lifted %s: %lu identical, %lu differ only in a NaN's payload/sign, %lu differ beyond it\n",
           WATER_GEN_OPT, g_kernel[1].same, g_kernel[1].nan_payload, g_kernel[1].different);
    printf("  lifted %s vs lifted %s: %lu identical, %lu differ only in a NaN's payload/sign, %lu differ beyond it\n",
           WATER_GEN_OPT, WATER_ALT_OPT, g_compiler[1].same, g_compiler[1].nan_payload, g_compiler[1].different);
    shared_tables(1);
    benchmark();
    occlusion_benchmark();
    polygon_benchmark();
    bump = bump_native_cases(quick ? 200 : 2000, 0);
    bump = bump_native_cases(quick ? 200 : 2000, 1) && bump;
    paced = wave_cadence_cases();
    dump = water_dump();
    return g_kernel[0].nan_payload || g_kernel[0].different || !alias || !bump || !paced || !dump ? 1 : 0;
}

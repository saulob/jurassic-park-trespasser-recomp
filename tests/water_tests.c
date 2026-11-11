/*
 * Byte-exact comparison of the host water kernels with the lifted functions they replace, which the
 * build copies out of src/recomp/gen (lifted_sub_*) and compiles with the generated code's options.
 * Each case runs both from the same guest state; everything the guest can observe afterwards must be
 * identical: memory, registers, flags, the x87 model, the backedge counter and every yield.
 * The guest addresses the code uses live in g_window, which the fixed image base puts there.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "water_kernels.h"

void lifted_sub_0060E3F0(void);
void lifted_sub_00638670(void);
void lifted_alt_sub_0060E3F0(void);
void lifted_alt_sub_00638670(void);

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double g_st[8];
int g_fp_top;
uint16_t g_fpu_cw;
uint32_t g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx, g_icall_count;
recomp_func_t recomp_lookup(uint32_t va) {
    (void)va;
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
    int fp_top;
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
    memcpy(g_st, s->st, sizeof g_st);
    memcpy(g_st_i64, s->st_i64, sizeof g_st_i64);
    g_fp_top = s->fp_top;
    g_fpu_cw = s->cw;
    g_backedges = s->backedges;
    g_cur_func = s->cur_func;
    g_alias_entry = 0;
    recomp_yield_hook = s->hook ? record_yield : NULL;
    memset(&g_run, 0, sizeof g_run);
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
    memcpy(m->st, g_st, sizeof m->st);
    memcpy(m->st_i64, g_st_i64, sizeof m->st_i64);
    m->fp_top = g_fp_top;
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
        if (x == y) continue;
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
        if (x != y) {
            printf(" memory at 0x%08X: lifted %08X, kernel %08X;", (unsigned)(REGION_LO + i), x, y);
            break;
        }
    }
    for (int i = 0; i < 8; i++)
        if (a->regs[i] != b->regs[i]) printf(" reg%d %08X/%08X", i, a->regs[i], b->regs[i]);
    for (int i = 0; i < 4; i++)
        if (a->flags[i] != b->flags[i]) printf(" flag%d %08X/%08X", i, a->flags[i], b->flags[i]);
    if (memcmp(a->st, b->st, sizeof a->st) || memcmp(a->st_i64, b->st_i64, sizeof a->st_i64) || a->fp_top != b->fp_top)
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

static double seconds(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* Time with the game's water texture shape: a 256-texel row, and 256x256 cells of simulation (eight
 * 256x128 steps). */
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
    return called;
}

int main(int argc, char** argv) {
    int quick = argc > 1 && !strcmp(argv[1], "--quick"), alias;
    unsigned long per_kind = quick ? 300 : 3000;
    uintptr_t lo = (uintptr_t)g_window, hi = lo + sizeof g_window;
    if (lo > REGION_LO || hi < REGION_HI) {
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
    alias = alias_reaches_lifted();
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
    return g_kernel[0].nan_payload || g_kernel[0].different || !alias ? 1 : 0;
}

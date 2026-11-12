#include "water_kernels.h"

#include <string.h>

#ifdef RECOMP_X87_LOCAL
#error "the kernels reproduce the x87 model the MSVC build lifts with: shifting (fp_push_impl) or the ring"
#endif

#define INV_SQRT_TABLE 0x006715A4u
#define ALPHA_OFFSET 0x0073F708u
#define ALPHA_SCALE 0x0073F70Cu
#define ALPHA_TABLE 0x0073F710u
#define ONE_BITS 0x3F800000u
#define FOUR_BITS 0x40800000u
#define WAVE_DAMPING 0x006474D4u
#define WAVE_NUMERATOR 0x00647B54u

recomp_func_t water_rows_lifted, water_wave_step_lifted;

static uint32_t rd32(uint32_t va) { return *(const uint32_t*)ADDR(va); }
static double rdf(uint32_t va) { return *(const float*)ADDR(va); }
static void wr32(uint32_t va, uint32_t v) { *(uint32_t*)ADDR(va) = v; }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float bits_float(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* cvttsd2si, which the lifted (int32_t) conversion compiles to: NaN and out of range give 0x80000000. */
static uint32_t to_int32(double v) {
    return v > -2147483649.0 && v < 2147483648.0 ? (uint32_t)(int32_t)v : 0x80000000u;
}

/* A balanced sequence that reached depth d: the shifting x87 model fills its d bottom registers with zero on the
 * pops; the ring leaves the stack as it found it. */
static void x87_balanced(int depth) {
#ifdef RECOMP_X87_RING
    (void)depth;
#else
    for (int i = 8 - depth; i < 8; i++) {
        g_st[i] = 0.0;
        g_st_i64[i] = 0;
    }
#endif
}

static void flags_cmp(uint32_t a, uint32_t b) {
    g_flag_k = FK_CMP;
    g_flag_a = a;
    g_flag_b = b;
    g_flag_cf = CMP_B(a, b);
}

/* RECOMP_BACKEDGE: every so many loop iterations the machine is offered to other guest threads, with
 * the registers the lifted loop holds at that point. */
static void backedge(uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp, uint32_t esi,
                     uint32_t edi) {
    if (++g_backedges < RECOMP_YIELD_EVERY || !recomp_yield_hook) return;
    g_backedges = 0;
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
    g_ebx = ebx;
    g_esp = esp;
    g_ebp = ebp;
    g_esi = esi;
    g_edi = edi;
    recomp_yield_hook();
}

/* The game's approximate 1/sqrt: exponent by halving, mantissa from a 4096-entry table. */
static uint32_t inverse_sqrt_bits(uint32_t x) {
    return ((0x5F000000u - ((x & 0x7F800000u) >> 1)) & 0x7F800000u) | rd32(INV_SQRT_TABLE + ((x & 0x00FFFFFFu) >> 12) * 4);
}

/* __thiscall(sim) (u32, u16* dest, int row, float vx, float vy, float vz, ...), ret 0x24. */
void water_rows(void) {
    uint32_t esp0, ebp, sim, span, dest, row, start, end, limit, heights, below, stride, eax;
    float d, f, c0, c1, k40, k3c, k38, p14, p18, p1c;
    double z, ad;
    if (g_alias_entry && water_rows_lifted) {
        water_rows_lifted();
        return;
    }
    g_cur_func = WATER_ROWS_VA;
    esp0 = g_esp;
    ebp = esp0 - 4;
    wr32(ebp, g_ebp);
    sim = rd32(g_ecx);
    span = rd32(sim + 0x98);
    wr32(ebp - 0x44, g_ebx);
    wr32(ebp - 0x48, g_esi);
    dest = rd32(ebp + 0xC);
    wr32(ebp - 0x4C, g_edi);
    row = rd32(ebp + 0x10);
    span += row * 8;
    start = rd32(span) - 4;
    wr32(ebp + 0xC, start);
    if ((int32_t)start < 1) wr32(ebp + 0xC, 1);
    limit = rd32(sim + 0x38) - 2;
    end = rd32(span + 4) + 4;
    wr32(ebp + 0x10, end);
    if ((int32_t)end > (int32_t)limit) wr32(ebp + 0x10, limit);
    heights = rd32(sim + 0x44) + rd32(sim + 0x50) * row * 4;
    stride = rd32(sim + 0x3C);
    wr32(ebp - 0x20, heights);
    wr32(ebp - 0x24, stride);
    wr32(ebp - 0x14, rd32(sim + 0x2C));
    wr32(ebp - 0x10, rd32(sim + 0x18));
    eax = rd32(ALPHA_OFFSET);
    wr32(ebp - 0xC, eax);
    wr32(ebp - 0x4, rd32(ALPHA_SCALE));
    wr32(ebp - 0x18, ALPHA_TABLE);
    wr32(ebp - 0x1C, dest - 2);
    wr32(ebp - 0x8, ONE_BITS);
    start = rd32(ebp + 0xC);
    end = rd32(ebp + 0x10);
    flags_cmp(start, end);
    if ((int32_t)start < (int32_t)end) {
        uint32_t k = start, texel_at = dest - 2;
        below = heights + stride * 4;
        p14 = (float)rdf(ebp + 0x14);
        p18 = (float)rdf(ebp + 0x18);
        p1c = (float)rdf(ebp + 0x1C);
        d = (float)rdf(ebp - 0x14);
        f = (float)rdf(ebp - 0x10);
        ad = (double)(int32_t)start * (double)d;
        z = (double)p14 - ad;
        k40 = (float)((double)p18 * (double)p18 + (double)p1c * (double)p1c);
        k3c = (float)((double)f * (double)f);
        k38 = (float)((double)f * (double)p1c);
        wrf(ebp - 0x40, k40);
        wrf(ebp - 0x3C, k3c);
        wrf(ebp - 0x38, k38);
        c0 = bits_float(rd32(ebp - 0xC));
        c1 = bits_float(rd32(ebp - 0x4));
        do {
            double h = rdf(heights + k * 4);
            double a = h - rdf(heights + k * 4 + 4);
            double b = h - rdf(below + k * 4);
            double s = b * b + a * a;
            double t = z * z + (double)k40;
            double az = a * z;
            double ts = t * (s + (double)k3c);
            double n = b * (double)p18 + az;
            uint32_t ts_bits = float_bits((float)ts), r = inverse_sqrt_bits(ts_bits), dot, index;
            float dot_f = (float)((n + (double)k38) * (double)bits_float(r));
            uint16_t texel;
            dot = float_bits(dot_f);
            if ((dot & 0x7FFFFFFFu) > ONE_BITS) dot = ONE_BITS | (dot & 0x80000000u);
            index = to_int32(fp_round_cw(((double)c0 + (double)bits_float(dot)) * (double)c1, g_fpu_cw));
            wr32(ebp - 0x34, ts_bits);
            wr32(ebp - 0x30, r);
            wr32(ebp - 0x2C, dot);
            wr32(ebp - 0x28, index);
            texel = *(const uint16_t*)ADDR(ALPHA_TABLE + index * 2);
            eax = (index & 0xFFFF0000u) | texel;
            k++;
            *(uint16_t*)ADDR(texel_at + k * 2 - 2) = texel;
            z = z - (double)d;
            flags_cmp(k, end);
            backedge(eax, k, below, ALPHA_TABLE, ebp - 0x4C, ebp, heights, texel_at);
        } while ((int32_t)k < (int32_t)end);
        x87_balanced(6);
        g_ecx = k;
        g_edx = below;
    } else {
        g_ecx = start;
        g_edx = end;
    }
    g_eax = eax;
    g_edi = rd32(ebp - 0x4C);
    g_esi = rd32(ebp - 0x48);
    g_ebx = rd32(ebp - 0x44);
    g_ebp = rd32(ebp);
    g_esp = esp0 + 4 + 0x24;
}

/* __thiscall(sim) (float dt), ret 4. Each row's span is integrated in a software-pipelined loop whose
 * operand order differs between its first cell and the rest; both orders are kept. */
void water_wave_step(void) {
    uint32_t esp0, ebp, sim, row, last, stride, offset, spans;
    float c4, c14, c10;
    double x, g, w, gc, dk;
    int depth = 2;
    if (g_alias_entry && water_wave_step_lifted) {
        water_wave_step_lifted();
        return;
    }
    g_cur_func = WATER_WAVE_STEP_VA;
    esp0 = g_esp;
    ebp = esp0 - 4;
    wr32(ebp, g_ebp);
    sim = g_ecx;
    row = rd32(sim + 0x8C);
    x = rdf(ebp + 8) + rdf(sim + 0x20);
    wr32(ebp - 0x20, sim);
    wr32(ebp - 0x24, g_ebx);
    wr32(ebp - 0x28, g_esi);
    x = x * rdf(ebp + 8);
    wr32(ebp - 0x2C, g_edi);
    wr32(ebp - 0x18, FOUR_BITS);
    x = x * rdf(WAVE_DAMPING);
    wr32(ebp - 0x8, row);
    c4 = (float)x;
    wrf(ebp - 0x4, c4);
    g = rdf(sim + 8) / rdf(sim + 0x20);
    w = rdf(WAVE_NUMERATOR) / (rdf(ebp + 8) * rdf(sim + 0x20));
    w = (w - g - rdf(sim + 0xC)) * (double)c4;
    c14 = (float)w;
    wrf(ebp - 0x14, c14);
    gc = g * (double)c4;
    dk = rdf(ebp + 8) / rdf(sim + 0x20);
    stride = rd32(sim + 0x50);
    wr32(ebp - 0x1C, stride);
    offset = stride * row;
    c10 = (float)(gc - dk);
    wrf(ebp - 0x10, c10);
    wr32(ebp + 8, offset);
    last = rd32(sim + 0x90);
    spans = rd32(sim + 0x98);
    for (;;) {
        uint32_t first, end;
        flags_cmp(row, last);
        backedge(last, offset, stride, row, ebp - 0x2C, ebp, sim, spans);
        if ((int32_t)row > (int32_t)last) break;
        first = rd32(spans + row * 8);
        end = rd32(spans + row * 8 + 4);
        if ((int32_t)first <= (int32_t)end) {
            uint32_t i = first + offset, heights = rd32(sim + 0x44), above, below, v, p;
            double t, u, h, k, s, cp, ka, q_x, q_y;
            end += offset;
            wr32(ebp - 0xC, end);
            above = heights - stride * 4;
            below = stride * 4 + heights;
            v = rd32(sim + 0x7C);
            t = (double)c4 * rdf(v + i * 4);
            u = rdf(above + i * 4) + rdf(below + i * 4);
            h = rdf(heights + i * 4 - 4) + rdf(heights + i * 4 + 4);
            k = (double)c14 - 4.0 * t;
            s = u + h;
            ka = k * rdf(heights + i * 4);
            p = rd32(sim + 0x58);
            cp = (double)c10 * rdf(p + i * 4);
            v = rd32(sim + 0x7C);
            q_y = t * s;
            q_x = cp + ka;
            i++;
            if (depth < 4) depth = 4;
            for (;;) {
                double cell;
                flags_cmp(i, end);
                backedge(i, v, below, heights, ebp - 0x2C, ebp, sim, above);
                if ((int32_t)i >= (int32_t)end) break;
                t = (double)c4 * rdf(v + i * 4);
                u = rdf(above + i * 4) + rdf(below + i * 4);
                h = rdf(heights + i * 4 - 4) + rdf(heights + i * 4 + 4);
                k = (double)c14 - 4.0 * t;
                cell = q_y + q_x;
                s = u + h;
                ka = k * rdf(heights + i * 4);
                p = rd32(sim + 0x58);
                cp = (double)c10 * rdf(p + i * 4);
                wrf(p + i * 4 - 4, (float)cell);
                q_y = cp + ka;
                q_x = s * t;
                i++;
                v = rd32(sim + 0x7C);
                depth = 6;
            }
            p = rd32(sim + 0x58);
            stride = rd32(ebp - 0x1C);
            offset = rd32(ebp + 8);
            spans = rd32(sim + 0x98);
            wrf(p + i * 4 - 4, (float)(q_y + q_x));
        }
        last = rd32(sim + 0x90);
        row = rd32(ebp - 0x8) + 1;
        offset += stride;
        wr32(ebp - 0x8, row);
        wr32(ebp + 8, offset);
    }
    x87_balanced(depth);
    g_eax = last;
    g_ecx = offset;
    g_edx = stride;
    g_edi = rd32(ebp - 0x2C);
    g_esi = rd32(ebp - 0x28);
    g_ebx = rd32(ebp - 0x24);
    g_ebp = rd32(ebp);
    g_esp = esp0 + 4 + 4;
}

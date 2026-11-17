#include "guest_va.h"
#include "polygon_submit.h"

#include <stdio.h>
#include <string.h>

#ifdef RECOMP_X87_LOCAL
#error "the host path reproduces the x87 model the MSVC build lifts with: shifting (fp_push_impl) or the ring"
#endif

#define DEVICE GVA(0x006D0E9C) /* IDirect3DDevice3* */
#define WRAP_U GVA(0x006D2108) /* the stage-0 address modes last sent: 1 wrap, 0 clamp */
#define WRAP_V GVA(0x006D2109)
#define FOG_SENT GVA(0x006D2101)
#define SHADE_SENT GVA(0x006D2103)
#define SPECULAR_SENT GVA(0x006D2104)
#define FOG_ENTRIES GVA(0x00732874)
#define FOG_TABLE GVA(0x00732878)
#define OFFSET_X GVA(0x007382B8)
#define OFFSET_Y GVA(0x007382BC)
#define K_ZERO GVA(0x006474D0)
#define K_HALF GVA(0x006474D4)
#define K_ONE GVA(0x00645C34)
#define K_BELOW_ONE GVA(0x0064C49C)
#define K_ONE_DOUBLE GVA(0x00646570)
#define FOG_SATURATED GVA(0x0064E70C)
#define FOG_SCALE GVA(0x0064E710)
#define K_255 GVA(0x0064DBCC)
#define Z_LIMIT GVA(0x0064E708)
#define SET_RENDER_STATE 0x58u
#define DRAW_PRIMITIVE 0x70u
#define SET_STAGE_STATE 0xA0u
#define MAX_VERTICES 4096u

recomp_func_t polygon_submit_lifted;

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t k, a, b, cf;
} cpu_t;

static __forceinline uint32_t rd32(uint32_t va) { return *(const uint32_t*)ADDR(va); }
static __forceinline uint8_t rd8(uint32_t va) { return *(const uint8_t*)ADDR(va); }
static __forceinline double rdf(uint32_t va) { return *(const float*)ADDR(va); }
static __forceinline void wr32(uint32_t va, uint32_t v) { *(uint32_t*)ADDR(va) = v; }
static __forceinline void wr8(uint32_t va, uint8_t v) { *(uint8_t*)ADDR(va) = v; }

static __forceinline uint32_t bits_of(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static __forceinline void push(cpu_t* c, uint32_t v) {
    c->esp -= 4;
    wr32(c->esp, v);
}

static __forceinline void set_flags(cpu_t* c, uint32_t k, uint32_t a, uint32_t b, uint32_t cf) {
    c->k = k, c->a = a, c->b = b, c->cf = cf;
}

/* `test r8, r8` and `cmp byte, imm` as the lifter records them. */
static __forceinline void test8(cpu_t* c, uint8_t v) { set_flags(c, FK_NARROW(FK_TEST, 24), (uint32_t)v << 24, (uint32_t)v << 24, 0); }

static __forceinline void cmp8(cpu_t* c, uint8_t x, uint8_t y) {
    set_flags(c, FK_NARROW(FK_CMP, 24), (uint32_t)x << 24, (uint32_t)y << 24, CMP_B((uint32_t)x << 24, (uint32_t)y << 24));
}

static __forceinline void low8(uint32_t* r, uint8_t v) { *r = (*r & 0xFFFFFF00u) | v; }

static void cpu_out(const cpu_t* c) {
    g_eax = c->eax, g_ecx = c->ecx, g_edx = c->edx, g_ebx = c->ebx;
    g_esp = c->esp, g_ebp = c->ebp, g_esi = c->esi, g_edi = c->edi;
    g_flag_k = c->k, g_flag_a = c->a, g_flag_b = c->b, g_flag_cf = c->cf;
}

static void cpu_in(cpu_t* c) {
    c->eax = g_eax, c->ecx = g_ecx, c->edx = g_edx, c->ebx = g_ebx;
    c->esp = g_esp, c->ebp = g_ebp, c->esi = g_esi, c->edi = g_edi;
    c->k = g_flag_k, c->a = g_flag_a, c->b = g_flag_b, c->cf = g_flag_cf;
}

static void call(cpu_t* c, recomp_func_t fn) {
    push(c, RECOMP_RETADDR);
    cpu_out(c);
    fn();
    cpu_in(c);
    g_cur_func = POLYGON_SUBMIT_VA;
}

/* `call dword ptr [vtable + slot]` with the arguments already pushed (RECOMP_ICALL). */
static void method(cpu_t* c, uint32_t vtable, uint32_t slot) {
    uint32_t va = rd32(vtable + slot);
    recomp_func_t fn;
    RECOMP_ICALL_TRACE(va);
    fn = recomp_resolve(va);
    if (!fn) {
        fprintf(stderr, "ICALL: unresolved VA 0x%08X from 0x%08X\n", va, g_cur_func);
        c->eax = 0;
        return;
    }
    call(c, fn);
}

static void yield(const cpu_t* c) {
    g_backedges = 0;
    g_eax = c->eax, g_ecx = c->ecx, g_edx = c->edx, g_ebx = c->ebx;
    g_esp = c->esp, g_ebp = c->ebp, g_esi = c->esi, g_edi = c->edi;
    recomp_yield_hook();
}

/* The engine's cast of a float to an integer by its exponent, without the x87. */
static __forceinline uint32_t float_to_int(uint32_t bits) {
    uint32_t shift = 0x7Eu - (bits >> 23);
    return (((bits | 0x800000u) << 8) & shift) >> (shift & 31u);
}

/* A material colour byte times an intensity, rounded, as at 0x005594E3 and its eight copies. */
static __forceinline uint32_t channel(uint8_t byte, float scale) {
    double v = (double)(int32_t)byte * (double)scale + rdf(K_HALF);
    if (v >= rdf(K_ZERO)) return float_to_int(bits_of((float)v));
    return 0u - float_to_int(bits_of((float)(rdf(K_BELOW_ONE) - v)));
}

static __forceinline uint32_t clamp_sum(uint32_t x, uint32_t y) {
    uint32_t s = (x & 0xFFu) + (y & 0xFFu);
    return s > 0xFFu ? 0xFFu : s;
}

static void stage_state(cpu_t* c, uint32_t dev, uint32_t state, uint32_t value) {
    push(c, value);
    push(c, state);
    push(c, 0);
    push(c, dev);
    method(c, rd32(dev), SET_STAGE_STATE);
}

/* D3DTSS_ADDRESS/ADDRESSU/ADDRESSV for the polygon's mode, sent only when they change. */
static void address_mode(cpu_t* c, uint32_t mode) {
    uint32_t dev;
    switch (mode) {
    case 0:
        low8(&c->eax, rd8(WRAP_U)), test8(c, rd8(WRAP_U));
        if (!rd8(WRAP_U)) {
            low8(&c->eax, rd8(WRAP_V)), test8(c, rd8(WRAP_V));
            if (!rd8(WRAP_V)) return;
        }
        c->eax = dev = rd32(DEVICE);
        wr8(WRAP_U, 0), wr8(WRAP_V, 0);
        c->ecx = rd32(dev);
        stage_state(c, dev, 0xC, 3);
        return;
    case 1:
        low8(&c->eax, rd8(WRAP_U)), test8(c, rd8(WRAP_U));
        if (rd8(WRAP_U)) {
            low8(&c->eax, rd8(WRAP_V)), test8(c, rd8(WRAP_V));
            if (rd8(WRAP_V)) return;
        }
        c->eax = dev = rd32(DEVICE);
        wr8(WRAP_U, 1), wr8(WRAP_V, 1);
        c->edx = rd32(dev);
        stage_state(c, dev, 0xC, 1);
        return;
    case 2:
        low8(&c->eax, rd8(WRAP_U)), test8(c, rd8(WRAP_U));
        if (!rd8(WRAP_U)) {
            c->eax = dev = rd32(DEVICE);
            wr8(WRAP_U, 1);
            c->ecx = rd32(dev);
            stage_state(c, dev, 0xD, 1);
        }
        low8(&c->eax, rd8(WRAP_V)), test8(c, rd8(WRAP_V));
        if (!rd8(WRAP_V)) return;
        c->eax = dev = rd32(DEVICE);
        wr8(WRAP_V, 0);
        c->edx = rd32(dev);
        stage_state(c, dev, 0xE, 3);
        return;
    case 3:
        low8(&c->eax, rd8(WRAP_U)), test8(c, rd8(WRAP_U));
        if (rd8(WRAP_U)) {
            c->eax = dev = rd32(DEVICE);
            wr8(WRAP_U, 0);
            c->ecx = rd32(dev);
            stage_state(c, dev, 0xD, 3);
        }
        low8(&c->eax, rd8(WRAP_V)), test8(c, rd8(WRAP_V));
        if (rd8(WRAP_V)) return;
        c->eax = dev = rd32(DEVICE);
        wr8(WRAP_V, 1);
        c->edx = rd32(dev);
        stage_state(c, dev, 0xE, 1);
        return;
    }
}

/* The fog factor in the specular alpha: the fog table interpolated at the vertex's depth. */
static uint32_t vertex_fog(cpu_t* c, uint32_t vtx) {
    uint32_t entries = rd32(FOG_ENTRIES), index, table, fog_bits;
    double depth = rdf(vtx + 4) * (double)(int32_t)entries, fog;
    float at = (float)depth, factor;
    if (!(depth >= rdf(K_ZERO))) at = 0.0f;
    index = float_to_int(bits_of(at));
    entries = rd32(FOG_ENTRIES);
    table = rd32(FOG_TABLE);
    if (index < entries - 1u)
        fog = (rdf(table + index * 4u + 4u) - rdf(table + index * 4u)) * ((double)at - (double)(int32_t)index) +
              rdf(table + index * 4u);
    else
        fog = rdf(table + entries * 4u - 4u);
    factor = (float)((rdf(FOG_SCALE) * rdf(K_HALF) + rdf(K_ONE)) - fog * rdf(FOG_SCALE));
    fog_bits = bits_of(factor);
    if (fog_bits & 0x80000000u) return 0;
    if (fog_bits >= rd32(FOG_SATURATED)) return 0xFF000000u;
    /* The cmp before `call _ftol`: what a lifted call flushes to the globals. */
    set_flags(c, FK_CMP, fog_bits, rd32(FOG_SATURATED), CMP_B(fog_bits, rd32(FOG_SATURATED)));
    g_flag_k = c->k, g_flag_a = c->a, g_flag_b = c->b, g_flag_cf = c->cf;
    return (uint32_t)fp_st0_to_i64((double)factor * rdf(K_255), 0, 0x0C7F) << 24;
}

void polygon_submit(void) {
    cpu_t c;
    uint32_t esp0, self, poly, mode, mat, n, buf, i, dev, vtx = 0;
    uint8_t fog_on;
    if (g_alias_entry || !polygon_submit_lifted) {
        if (polygon_submit_lifted) polygon_submit_lifted();
        return;
    }
    esp0 = g_esp;
    poly = rd32(esp0 + 4);
    n = rd32(poly);
    if (n > MAX_VERTICES || !rd32(DEVICE)) {
        polygon_submit_lifted();
        return;
    }
    g_cur_func = POLYGON_SUBMIT_VA;
    cpu_in(&c);
    self = c.ecx;
    c.ebp = esp0 - 4;
    wr32(c.ebp, g_ebp);
    wr32(c.ebp - 0xBC, c.ebx);
    wr32(c.ebp - 0xC0, c.esi);
    wr32(c.ebp - 0xC4, c.edi);
    c.esp = c.ebp - 0xC4;
    c.esi = poly;
    c.eax = mode = rd32(poly + 0x78);
    set_flags(&c, FK_CMP, mode, 3, CMP_B(mode, 3));
    if (mode <= 3) address_mode(&c, mode);

    c.eax = rd32(poly + 0xC);
    c.ecx = rd32(c.eax + 0x2C);
    c.esi = n;
    c.edx = mat = rd32(c.ecx + 8);
    c.eax = ((n << 5) + 3) & 0xFFFFFFFCu;
    low8(&c.ebx, 0);
    set_flags(&c, FK_TEST, (n << 5) + 3, 0xFFFFFFFCu, 0);
    wr8(c.ebp - 0x2C, 0);
    call(&c, GSUB(0x00626BA0)); /* _alloca_probe */
    buf = c.esp;
    c.edi = n - 1;
    set_flags(&c, FK_TEST, c.edi, c.edi, 0);
    c.eax = buf;
    fog_on = 0;
    if ((int32_t)c.edi >= 0) {
        fog_on = 1;
        wr8(c.ebp - 0x2C, 1);
        c.esi = buf + c.edi * 32u + 8u;
        for (i = c.edi;; i--) {
            uint32_t color, specular, fog, out = buf + i * 32u, threshold, z;
            float intensity;
            vtx = rd32(rd32(poly + 4) + i * 4u);
            c.ebx = vtx;
            c.edi = i;
            fog = vertex_fog(&c, vtx);
            if (!rd32(mat + 0x54)) {
                c.eax = 0, c.ecx = mat, c.edx = mat;
                set_flags(&c, FK_TEST, 0, 0, 0);
                call(&c, GSUB(0x0050C9C0)); /* allocates the material's lighting table */
            }
            threshold = rd32(mat + 0x2C);
            intensity = *(const float*)ADDR(vtx + 0x18);
            if ((double)intensity > (double)(int32_t)threshold) {
                int32_t span = (int32_t)(rd32(mat + 0x14) - threshold);
                float above = (float)(((double)intensity - (double)(int32_t)threshold) / (double)span);
                float below = (float)(rdf(K_ONE) - (double)above);
                color = (channel(rd8(mat + 8), below) & 0xFFu) | (channel(rd8(mat + 9), below) & 0xFFu) << 8 |
                        (channel(rd8(mat + 0xA), below) & 0xFFu) << 16;
                specular = (channel(rd8(mat + 8), above) & 0xFFu) | (channel(rd8(mat + 9), above) & 0xFFu) << 8 |
                           (channel(rd8(mat + 0xA), above) & 0xFFu) << 16;
            } else {
                float lit = (float)((double)intensity / (double)(int32_t)threshold *
                                        (*(const double*)ADDR(K_ONE_DOUBLE) - rdf(mat + 0x30)) +
                                    rdf(mat + 0x30));
                uint32_t l0 = channel(rd8(mat + 8), lit), l1 = channel(rd8(mat + 9), lit),
                         l2 = channel(rd8(mat + 0xA), lit);
                float ambient = (float)(rdf(K_ONE) - (double)lit);
                uint32_t a0 = channel(rd8(mat + 4), ambient), a1 = channel(rd8(mat + 5), ambient),
                         a2 = channel(rd8(mat + 6), ambient);
                color = clamp_sum(a0, l0) | clamp_sum(a1, l1) << 8 | clamp_sum(a2, l2) << 16;
                specular = 0;
            }
            *(float*)ADDR(out) = (float)(rdf(OFFSET_X) + rdf(vtx + 0xC));
            *(float*)ADDR(out + 4) = (float)(rdf(OFFSET_Y) + rdf(vtx + 0x10));
            z = rd32(vtx + 0x14);
            *(float*)ADDR(out + 8) =
                (float)((z > rd32(Z_LIMIT) ? rdf(Z_LIMIT) : rdf(vtx + 0x14)) * rdf(self + 0x10));
            wr32(out + 0xC, rd32(vtx + 0x14));
            wr32(out + 0x10, color);
            wr32(out + 0x18, rd32(vtx + 0x1C));
            wr32(out + 0x1C, rd32(vtx + 0x20));
            wr32(out + 0x14, specular | fog);
            c.eax = specular | fog;
            c.ecx = rd32(vtx + 0x20);
            c.edx = rd32(vtx + 0x1C);
            c.esi = out + 8u - 0x20u;
            c.edi = i - 1;
            set_flags(&c, FK_TEST, c.edi, c.edi, 0);
            if (++g_backedges >= RECOMP_YIELD_EVERY && recomp_yield_hook) yield(&c);
            if ((int32_t)c.edi < 0) break;
        }
        low8(&c.ebx, 1);
    }

    dev = rd32(DEVICE);
    cmp8(&c, rd8(FOG_SENT), fog_on);
    if (rd8(FOG_SENT) != fog_on) {
        uint32_t word = rd32(c.ebp - 0x2C);
        c.eax = dev;
        set_flags(&c, FK_TEST, word, 0xFFu, 0);
        c.ecx = word & 0xFFu;
        push(&c, c.ecx);
        push(&c, 0x1C);
        wr8(FOG_SENT, fog_on);
        c.edx = rd32(dev);
        push(&c, dev);
        method(&c, c.edx, SET_RENDER_STATE);
    }
    cmp8(&c, rd8(SHADE_SENT), 1);
    if (rd8(SHADE_SENT) != 1) {
        c.eax = dev = rd32(DEVICE);
        push(&c, 2);
        push(&c, 9);
        wr8(SHADE_SENT, 1);
        c.edx = rd32(dev);
        push(&c, dev);
        method(&c, c.edx, SET_RENDER_STATE);
    }
    low8(&c.eax, rd8(SPECULAR_SENT));
    test8(&c, rd8(SPECULAR_SENT));
    if (rd8(SPECULAR_SENT)) {
        c.eax = dev = rd32(DEVICE);
        push(&c, 0);
        push(&c, 0x1D);
        wr8(SPECULAR_SENT, 0);
        c.ecx = rd32(dev);
        push(&c, dev);
        method(&c, c.ecx, SET_RENDER_STATE);
    }
    c.eax = dev = rd32(DEVICE);
    c.edx = rd32(dev);
    push(&c, 0x1C);
    push(&c, rd32(rd32(c.ebp + 8)));
    c.ecx = buf;
    push(&c, buf);
    push(&c, 0x1C4);
    push(&c, 6);
    push(&c, dev);
    method(&c, c.edx, DRAW_PRIMITIVE);

    g_eax = (c.eax & 0xFFFFFF00u) | 1u;
    g_ecx = c.ecx;
    g_edx = c.edx;
    g_edi = rd32(c.ebp - 0xC4);
    g_esi = rd32(c.ebp - 0xC0);
    g_ebx = rd32(c.ebp - 0xBC);
    g_ebp = rd32(c.ebp);
    g_esp = esp0 + 8;
    g_flag_k = c.k, g_flag_a = c.a, g_flag_b = c.b, g_flag_cf = c.cf;
}

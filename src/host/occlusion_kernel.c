#include "occlusion_kernel.h"

#include <string.h>

#ifdef RECOMP_X87_LOCAL
#error "the kernel reproduces the x87 model the MSVC build lifts with: shifting (fp_push_impl) or the ring"
#endif

#define EPSILON_ON 0x0066CC98u
#define EPSILON_VALUE 0x00651C20u
#define EPSILON_USED 0x00741110u
#define OCCLUDED_COUNT 0x007410F8u
#define VISIBLE_COUNT 0x007410FCu

recomp_func_t occlusion_test_lifted;

static __forceinline uint32_t rd32(uint32_t va) { return *(const uint32_t*)ADDR(va); }
static __forceinline double rdf(uint32_t va) { return *(const float*)ADDR(va); }
static __forceinline void wr32(uint32_t va, uint32_t v) { *(uint32_t*)ADDR(va) = v; }

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

static void yield(uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp, uint32_t esi,
                  uint32_t edi) {
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

static __forceinline void backedge(uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp,
                                   uint32_t esi, uint32_t edi) {
    if (++g_backedges >= RECOMP_YIELD_EVERY && recomp_yield_hook) yield(eax, ecx, edx, ebx, esp, ebp, esi, edi);
}

/* The x87 sequence at 0x00630DFD in its order: two products summed, the third added, then d and the epsilon, each
 * rounding to the double the 53-bit control word gives; stored as a float. */
static __forceinline uint32_t plane_side_bits(uint32_t plane, uint32_t point) {
    double a = rdf(plane + 8) * rdf(point + 8);
    double b = rdf(plane + 4) * rdf(point + 4);
    double s = a + b;
    float f;
    uint32_t bits;
    s = s + rdf(plane) * rdf(point);
    s = s + rdf(plane + 0xC);
    s = s + rdf(EPSILON_USED);
    f = (float)s;
    memcpy(&bits, &f, 4);
    return bits;
}

/* cdecl bool (const CSet<COccluder*>* occluders, const CArray<CVector3>* points): true when one occluder has every
 * point behind each of its planes (the float's sign bit set). */
void occlusion_test(void) {
    uint32_t esp0, sp, eax, ebx, ecx, edx, esi, edi, ebp, entry_ebx, entry_ebp, entry_esi, entry_edi;
    int planes_run = 0;
    if (g_alias_entry && occlusion_test_lifted) {
        occlusion_test_lifted();
        return;
    }
    g_cur_func = OCCLUSION_TEST_VA;
    esp0 = g_esp;
    sp = esp0 - 0x1C;
    entry_ebx = ebx = g_ebx;
    entry_ebp = ebp = g_ebp;
    entry_esi = esi = g_esi;
    entry_edi = edi = g_edi;
    ecx = rd32(esp0 + 4);
    edx = rd32(ecx);
    wr32(esp0 - 0x10, entry_ebx);
    wr32(esp0 - 0x14, entry_ebp);
    wr32(esp0 - 0x18, entry_esi);
    wr32(esp0 - 0x1C, entry_edi);
    eax = 0;
    wr32(esp0 - 0xC, 0);
    if (!edx) goto visible;
    ebp = rd32(esp0 + 8);
    for (;;) {
        ecx = rd32(ecx + 4);
        eax = rd32(ecx + eax * 4);
        ecx = rd32(ebp);
        edx = 0;
        wr32(esp0 - 4, eax);
        wr32(esp0 + 8, 0);
        if (!ecx) goto occluded;
        for (;;) {
            ecx = rd32(ebp + 4) + edx;
            ebx = (ebx & 0xFFFFFF00u) | *(const uint8_t*)ADDR(EPSILON_ON);
            if (ebx & 0xFF) {
                esi = rd32(EPSILON_VALUE);
                wr32(EPSILON_USED, esi);
            } else {
                wr32(EPSILON_USED, 0);
            }
            edi = rd32(eax + 0x28);
            esi = 0;
            if (edi) {
                eax = rd32(eax + 0x2C);
                for (;;) {
                    uint32_t bits = plane_side_bits(eax, ecx);
                    planes_run = 1;
                    wr32(esp0 - 8, bits);
                    ebx = bits >> 31;
                    if (!ebx) goto fails;
                    esi++;
                    eax += 0x10;
                    backedge(eax, ecx, edx, ebx, sp, ebp, esi, edi);
                    if (esi >= edi) break;
                }
                eax = rd32(esp0 - 4);
            }
            ecx = rd32(esp0 + 8) + 1;
            esi = rd32(ebp);
            edx += 0xC;
            wr32(esp0 + 8, ecx);
            if (ecx >= esi) goto occluded;
            backedge(eax, ecx, edx, ebx, sp, ebp, esi, edi);
        }
    fails:
        eax = rd32(esp0 - 0xC) + 1;
        ecx = rd32(esp0 + 4);
        edx = rd32(ecx);
        wr32(esp0 - 0xC, eax);
        if (eax >= edx) goto visible;
        backedge(eax, ecx, edx, ebx, sp, ebp, esi, edi);
    }

occluded:
    eax = rd32(OCCLUDED_COUNT) + 1;
    wr32(OCCLUDED_COUNT, eax);
    eax = (eax & 0xFFFFFF00u) | 1u;
    goto done;
visible:
    eax = rd32(VISIBLE_COUNT) + 1;
    wr32(VISIBLE_COUNT, eax);
    eax &= 0xFFFFFF00u;
done:
    if (planes_run) x87_balanced(2);
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
    g_ebx = entry_ebx;
    g_ebp = entry_ebp;
    g_esi = entry_esi;
    g_edi = entry_edi;
    g_flag_k = FK_ADD;
    g_flag_a = esp0 - 0xC;
    g_flag_b = 0xC;
    g_flag_cf = (esp0 - 0xC) + 0xC < esp0 - 0xC;
    g_esp = esp0 + 4;
}

/* Processor.dll!bGetProcessorInfo answered from the host's CPUID. The original DLL probes the CPU with 16-bit EFLAGS code
 * and times it; the game reads only three things from the 0x4D-dword record it fills: the feature word at +0x0C (bit 29 =
 * PentiumPro-compatible, required to start), the clock in MHz at +0x10 and the processor name at +0x14. */
#include "linux_processor.h"

#include <cpuid.h>
#include <stdio.h>
#include <string.h>

#include "guest_ptr.h"
#include "linux_log.h"

#define RECORD_DWORDS 0x4D
#define FLAG_CPUID 0x00000001u
#define FLAG_PPRO_CLASS 0x10000000u
#define FLAG_PPRO_COMPATIBLE 0x20000000u
#define NAME_OFFSET 0x14
#define NAME_MAX 0x40

typedef struct {
    unsigned edx_bit;
    uint32_t flag;
} feature_map_t;

/* CPUID.1:EDX bit -> record flag, as Processor.dll maps them. */
static const feature_map_t k_features[] = {
    { 1, 0x00000400u }, { 2, 0x00000200u }, { 3, 0x00000800u }, { 4, 0x00000040u },  { 5, 0x00001000u }, { 6, 0x00002000u },
    { 7, 0x00040000u }, { 8, 0x00000080u }, { 9, 0x00000020u }, { 11, 0x00004000u }, { 12, 0x00080000u }, { 13, 0x00008000u },
    { 14, 0x00010000u }, { 15, 0x00000010u }, { 16, 0x00020000u }, { 23, 0x00000008u },
};

static unsigned host_mhz(void) {
    FILE* f = fopen("/proc/cpuinfo", "r");
    char line[256];
    unsigned mhz = 0;
    while (f && fgets(line, sizeof line, f)) {
        double v;
        if (sscanf(line, "cpu MHz : %lf", &v) == 1 && v > 0) {
            mhz = (unsigned)v;
            break;
        }
    }
    if (f) fclose(f);
    return mhz ? mhz : 1000;
}

static void brand_string(char* out, size_t cap) {
    unsigned r[12] = { 0 }, a, b, c, d;
    out[0] = 0;
    if (__get_cpuid_max(0x80000000u, NULL) < 0x80000004u) return;
    for (unsigned i = 0; i < 3; i++) {
        __cpuid(0x80000002u + i, a, b, c, d);
        r[i * 4] = a, r[i * 4 + 1] = b, r[i * 4 + 2] = c, r[i * 4 + 3] = d;
    }
    memcpy(out, r, sizeof r);
    out[sizeof r < cap ? sizeof r : cap - 1] = 0;
}

static uint32_t family_code(unsigned vendor, unsigned family) {
    if (vendor == 0) return family >= 6 ? 0x6Du : family == 5 ? 0x69u : 0x68u;
    if (vendor == 1) return family >= 6 ? 0xCEu : 0xCDu;
    return family >= 5 ? 0x132u : 0xFFFFFFFFu;
}

static uint64_t p_bGetProcessorInfo(const uint32_t* a, void* u) {
    uint32_t rec[RECORD_DWORDS];
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0, vendor = 3, family;
    char name[NAME_MAX];
    uint32_t flags = 0;
    (void)u;
    memset(rec, 0, sizeof rec);
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
        if (ebx == 0x756E6547u && edx == 0x49656E69u && ecx == 0x6C65746Eu) vendor = 0;
        else if (ebx == 0x68747541u && edx == 0x69746E65u && ecx == 0x444D4163u) vendor = 1;
    }
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
    family = (eax >> 8) & 0xF;
    if (family == 0xF) family += (eax >> 20) & 0xFF;
    flags = FLAG_CPUID;
    for (size_t i = 0; i < sizeof k_features / sizeof k_features[0]; i++)
        if (edx & (1u << k_features[i].edx_bit)) flags |= k_features[i].flag;
    if (family >= 5) {
        flags |= FLAG_PPRO_CLASS;
        if (flags & 0x10u) flags |= FLAG_PPRO_COMPATIBLE;
    }
    brand_string(name, sizeof name);
    rec[0] = rec[1] = family_code(vendor, family);
    rec[2] = vendor;
    rec[3] = flags;
    rec[4] = host_mhz();
    memcpy((char*)rec + NAME_OFFSET, name, strlen(name) + 1);
    lx_log("hle", "Linux HLE: Processor.dll record from host CPUID (family %u, vendor %u, flags 0x%08X, %u MHz)", family, vendor, flags,
           (unsigned)rec[4]);
    return gp_copy_out(a[0], rec, sizeof rec) ? 1 : 0;
}

static const hle_entry_t k_table[] = { { "processor.dll", "bGetProcessorInfo", p_bGetProcessorInfo, 1, HT_CDECL } };

const hle_entry_t* hle_processor_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}

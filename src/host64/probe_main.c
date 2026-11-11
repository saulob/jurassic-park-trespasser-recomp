/*
 * trespasser_x64_probe: a headless 64-bit process that lays the retail images out in a guest
 * address space and binds trespass.exe's imports the way the 64-bit host will, then reports.
 * It runs no guest code, opens no window and plays no sound.
 *
 *   trespasser_x64_probe [game_dir]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"

#ifndef TRESPASSER_DEFAULT_GAME_DIR
#define TRESPASSER_DEFAULT_GAME_DIR "game"
#endif

/* The dwords SelfMod's routines store into their own disp32 fields (tools/x64_selfmod_scan.py). */
static const uint32_t k_patch_sites[] = {
    0x0063CC83u, 0x0063CCA3u, 0x0063D6B9u, 0x0063D6C4u, 0x0063DB43u, 0x0063DB63u, 0x0063DD59u, 0x0063DD64u,
    0x0063E532u, 0x0063E556u, 0x0063EFA2u, 0x0063EFC2u, 0x0063F492u, 0x0063F4B6u, 0x0063F712u, 0x0063F732u,
    0x0063FEF4u, 0x0063FF1Eu, 0x0063FF29u, 0x00640994u, 0x006409B6u, 0x006409C1u, 0x00640E74u, 0x00640E9Eu,
    0x00640EA9u, 0x006410F4u, 0x00641116u, 0x00641121u, 0x006412E4u, 0x00641486u, 0x0064179Au, 0x0064194Eu,
    0x00642E96u, 0x00642EA6u, 0x00642EBAu, 0x00643376u, 0x00643386u, 0x0064339Au, 0x0064364Bu, 0x0064365Bu,
    0x00643C09u, 0x00643C38u, 0x00644019u, 0x00644048u,
};

#define MAX_DLLS 32

typedef struct {
    char name[64];
    int imports, guest, thunks;
} dll_stat_t;

typedef struct {
    gm_image_t* smack;
    gm_image_t* irt;
    dll_stat_t dll[MAX_DLLS];
    int ndll;
} probe_t;

static uint64_t not_implemented(const uint32_t* args, void* user) {
    (void)args;
    fprintf(stderr, "[probe] unimplemented import %s called\n", (const char*)user);
    return 0;
}

static dll_stat_t* stat_for(probe_t* p, const char* dll) {
    for (int i = 0; i < p->ndll; i++)
        if (!_stricmp(p->dll[i].name, dll)) return &p->dll[i];
    if (p->ndll == MAX_DLLS) return NULL;
    snprintf(p->dll[p->ndll].name, sizeof p->dll[0].name, "%s", dll);
    return &p->dll[p->ndll++];
}

static uint32_t resolve(const char* dll, const char* name, uint16_t ordinal, void* user) {
    probe_t* p = (probe_t*)user;
    dll_stat_t* s = stat_for(p, dll);
    gm_image_t* img = NULL;
    char* label;
    size_t n;
    uint32_t va;
    if (s) s->imports++;
    if (!_stricmp(dll, "smackw32.dll")) img = p->smack;
    if (!_stricmp(dll, "msvcirt.dll")) img = p->irt;
    if (img) {
        va = gm_pe_export(img, name, name ? 0 : ordinal);
        if (va && s) s->guest++;
        return va;
    }
    n = strlen(dll) + (name ? strlen(name) : 8) + 2;
    label = (char*)malloc(n);
    if (!label) return 0;
    if (name) snprintf(label, n, "%s!%s", dll, name);
    else snprintf(label, n, "%s#%u", dll, ordinal);
    va = ht_register(label, not_implemented, 0, HT_STDCALL, label);
    if (va && s) s->thunks++;
    return va;
}

static int map_optional(const char* dir, const char* file, int mode, gm_image_t* img) {
    char path[MAX_PATH], err[128];
    snprintf(path, sizeof path, "%s\\%s", dir, file);
    if (gm_pe_map(path, mode, 0, img, err, sizeof err)) {
        printf("  %-14s not mapped: %s\n", file, err);
        return 0;
    }
    printf("  %-14s 0x%08X-0x%08X (prefers 0x%08X%s)\n", file, img->base, img->base + img->span, img->image_base,
           img->base == img->image_base ? "" : ", relocated");
    return 1;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : TRESPASSER_DEFAULT_GAME_DIR;
    gm_image_t exe, smack, irt, proc;
    probe_t p;
    uint32_t bound = 0, held = 0, d;
    int unresolved, i;

    memset(&p, 0, sizeof p);
    printf("trespasser_x64_probe: %d-bit process, game directory %s\n", (int)(sizeof(void*) * 8), dir);
    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest address space\n");
        return 2;
    }
    printf("guest window at host %p (4 GB reserved, nothing executable)\n", (void*)gm_base());

    printf("images:\n");
    if (!map_optional(dir, "trespass.exe", GM_PE_PREFERRED, &exe)) return 3;
    for (i = 0; i < exe.nsections; i++)
        printf("    %-8s 0x%08X size 0x%06X\n", exe.sections[i].name, exe.sections[i].va, exe.sections[i].vsize);
    if (map_optional(dir, "SMACKW32.DLL", GM_PE_PREFERRED, &smack)) p.smack = &smack;
    if (map_optional(dir, "MSVCIRT.DLL", GM_PE_PREFERRED, &irt)) p.irt = &irt;
    map_optional(dir, "Processor.dll", GM_PE_ANYWHERE, &proc);

    unresolved = gm_pe_bind(&exe, resolve, &p, &bound);
    printf("imports: %u bound, %d unresolved\n", bound, unresolved);
    for (i = 0; i < p.ndll; i++)
        printf("  %-14s %3d imports: %3d to guest code, %3d host thunks\n", p.dll[i].name, p.dll[i].imports,
               p.dll[i].guest, p.dll[i].thunks);

    for (i = 0; i < (int)(sizeof k_patch_sites / sizeof k_patch_sites[0]); i++)
        if (gp_read_u32(k_patch_sites[i], &d) && (d == 0xDEADBEEFu || d == 0xDEADC0DEu)) held++;
    printf("SelfMod: %u of %u patch sites hold their placeholder (lift with lift32 patch_sites)\n", held,
           (unsigned)(sizeof k_patch_sites / sizeof k_patch_sites[0]));
    printf("guest regions %d, host thunks %d, refused host-pointer stores %u\n", gm_region_count(), ht_count(),
           gp_leak_count());
    gm_shutdown();
    return unresolved ? 1 : 0;
}

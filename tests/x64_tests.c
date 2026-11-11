/*
 * Headless checks of the 64-bit host's groundwork (src/host64): the guest address space,
 * pointer marshalling, PE mapping and import binding against the retail files, and host COM
 * objects behind guest vtables. No window, device, sound or lifted code.
 *
 *   x64_tests [game_dir]      (default: %TRESPASSER_GAME_DIR%, else .\game)
 *
 * Image tests are skipped when the game files are absent.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"

static int s_pass, s_fail, s_skip;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) s_pass++;                                                  \
        else {                                                               \
            s_fail++;                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

static void skip(const char* why) {
    s_skip++;
    printf("  SKIP %s\n", why);
}

static int faults_on_read(const volatile uint8_t* p) {
    __try {
        volatile uint8_t v = *p;
        (void)v;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
    return 0;
}

static int faults_on_write(volatile uint8_t* p) {
    __try {
        *p = 1;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
    return 0;
}

static void test_window(void) {
    uint8_t* b;
    printf("[window] 4 GB reservation\n");
    CHECK(gm_init(0, 0));
    CHECK(gm_init(0, 0));
    b = gm_base();
    CHECK(b != NULL);
    CHECK(((uintptr_t)b & 0xFFFF) == 0);
    CHECK((uint64_t)(uintptr_t)b + GM_SPAN > 0xFFFFFFFFull);
    CHECK(faults_on_read(b));
    CHECK(faults_on_read(b + 0x00400000u));
    CHECK(faults_on_read(b + 0xFFFFFFFFull));
    CHECK(faults_on_read(b + GM_SPAN + 8));
}

static void test_conversion(void) {
    uint32_t va;
    int local = 0;
    unsigned bad;
    uint8_t* p;
    printf("[va] conversion and bounds\n");
    va = gm_alloc_pages(0x20000000u, 0x3000, "test");
    CHECK(va == 0x20000000u);
    p = (uint8_t*)gm_ptr(va, 16);
    CHECK(p == gm_base() + va);
    CHECK(gm_va(p) == va);
    CHECK(gm_va(p + 0x2FFF) == va + 0x2FFF);
    CHECK(gm_va(NULL) == 0);
    bad = gm_bad_va_count();
    CHECK(gm_va(&local) == 0);
    CHECK(gm_bad_va_count() == bad + 1);
    CHECK(gm_ptr(0, 4) == NULL);
    CHECK(gm_ptr(0x8000, 4) == NULL);
    CHECK(gm_ptr(GM_NULL_GUARD_END, 4) != NULL);
    CHECK(gm_ptr(0xFFFFFFFCu, 4) != NULL);
    CHECK(gm_ptr(0xFFFFFFFDu, 4) == NULL);
    CHECK(gm_ptr_checked(va, 0x3000, 1) != NULL);
    CHECK(gm_ptr_checked(va, 0x3001, 0) == NULL);
    CHECK(gm_ptr_checked(va + 0x3000, 1, 0) == NULL);
    p[0] = 0x5A;
    CHECK(*(uint8_t*)gm_ptr(va, 1) == 0x5A);
    CHECK(gm_protect(va, 0x1000, GM_PROT_R));
    CHECK(gm_ptr_checked(va, 4, 0) != NULL);
    CHECK(gm_ptr_checked(va, 4, 1) == NULL);
    CHECK(faults_on_write(p));
    CHECK(gm_protect(va, 0x1000, GM_PROT_RW));
    CHECK(!faults_on_write(p));
    CHECK(gm_decommit(va + 0x1000, 0x1000));
    CHECK(faults_on_read(p + 0x1000));
    CHECK(gm_commit(va + 0x1000, 0x1000, GM_PROT_RW));
    CHECK(p[0x1000] == 0);
    CHECK(gm_contains(p, 16));
    CHECK(!gm_contains(&local, 1));
}

static void test_reservations(void) {
    uint32_t a, b, lo, size;
    const char* tag;
    printf("[va] reservations\n");
    CHECK(gm_reserve(0x1000, 0x1000, "null") == 0);
    CHECK(gm_reserve(GM_THUNK_LO, 0x1000, "thunk") == 0);
    CHECK(gm_reserve(0x20002000u, 0x1000, "overlap") == 0);
    CHECK(gm_reserve(0xFFFF0000u, 0x20000, "past 4GB") == 0);
    a = gm_reserve(0, 0x12345, "any");
    CHECK(a >= GM_ANY_LO && a < GM_ANY_HI && (a & 0xFFFF) == 0);
    CHECK(gm_region(a + 0x1FFFF, &lo, &size, &tag) && lo == a && size == 0x20000 && !strcmp(tag, "any"));
    CHECK(!gm_region(a + 0x20000, &lo, NULL, NULL) || lo != a);
    CHECK(!gm_commit(a + 0x1F000, 0x2000, GM_PROT_RW));
    CHECK(gm_release(a));
    b = gm_reserve(a, 0x10000, "again");
    CHECK(b == a);
    CHECK(gm_release(b));
    CHECK(!gm_release(b));
}

static void test_heap(void) {
    uint32_t v[64], big, r, i;
    int distinct = 1;
    printf("[heap] guest allocator\n");
    for (i = 0; i < 64; i++) {
        v[i] = gm_heap_alloc(1 + i * 37, 1);
        CHECK(v[i] && (v[i] & 15) == 0 && v[i] >= GM_ANY_LO && v[i] < GM_ANY_HI);
        CHECK(gm_heap_size(v[i]) == 1 + i * 37);
        memset(gm_ptr(v[i], 1 + i * 37), (int)i, 1 + i * 37);
    }
    for (i = 1; i < 64; i++)
        if (v[i] == v[i - 1]) distinct = 0;
    CHECK(distinct);
    for (i = 0; i < 64; i++) CHECK(((uint8_t*)gm_ptr(v[i], 1))[0] == (uint8_t)i);
    gm_heap_free(v[10]);
    CHECK(!gm_heap_ok(v[10]));
    CHECK(gm_heap_alloc(1 + 10 * 37, 0) == v[10]);
    r = gm_heap_realloc(v[3], 5000, 1);
    CHECK(r && ((uint8_t*)gm_ptr(r, 1))[0] == 3 && ((uint8_t*)gm_ptr(r + 4999, 1))[0] == 0);
    big = gm_heap_alloc(0x100000, 1);
    CHECK(big && gm_heap_size(big) == 0x100000);
    CHECK(gm_ptr_checked(big, 0x100000, 1) != NULL);
    gm_heap_free(big);
    CHECK(gm_ptr_checked(big, 4, 0) == NULL);
    CHECK(!gm_heap_ok(0x20000010u));
    CHECK(!gm_heap_ok(0));
    {
        uint32_t m = gm_scratch_mark(), s1 = gm_scratch(100), s2 = gm_scratch_copy("abc", 4);
        CHECK(s1 && s2 > s1 && !strcmp((const char*)gm_ptr(s2, 4), "abc"));
        gm_scratch_release(m);
        CHECK(gm_scratch(100) == s1);
        gm_scratch_release(m);
    }
}

typedef struct {
    uint32_t hwnd, message, wParam, lParam, time;
    int32_t x, y;
} guest_msg_t;
GP_GUEST_LAYOUT(guest_msg_t, 28);

static void test_marshalling(void) {
    static int host_static;
    uint32_t slot = gm_heap_alloc(64, 1), hits[4], n;
    uint64_t value;
    guest_ptr_t where;
    HMODULE self = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)self + ((IMAGE_DOS_HEADER*)self)->e_lfanew);
    uint64_t mod_lo = (uint64_t)(uintptr_t)self, mod_hi = mod_lo + nt->OptionalHeader.SizeOfImage;
    guest_msg_t gm = {0x00010024u, 0x0200u, 1, 0x00200010u, 1234, 16, 32}, back;
    MSG host;
    printf("[ptr] marshalling and leak checks\n");
    gp_leak_reset();
    CHECK(gp_store_ptr(slot, gm_ptr(slot + 8, 4)));
    CHECK(*(uint32_t*)gm_ptr(slot, 4) == slot + 8);
    CHECK(gp_store_ptr(slot, NULL) && *(uint32_t*)gm_ptr(slot, 4) == 0);
    CHECK(!gp_store_ptr(slot, &host_static));
    CHECK(gp_leak_count() == 1);
    CHECK(gp_last_leak(&value, &where) && value == (uint64_t)(uintptr_t)&host_static && where == slot);
    CHECK(*(uint32_t*)gm_ptr(slot, 4) == 0);
    CHECK(!gp_store_u32(slot, 0x100000000ull));
    CHECK(gp_leak_count() == 2);
    CHECK(gp_store_u32(slot, 0xFFFFFFFFull));
    CHECK(gp_string(slot, 4) == NULL);
    CHECK(gp_copy_out(slot + 16, "guest", 6) && !strcmp(gp_string(slot + 16, 16), "guest"));
    CHECK(gp_string(slot + 16, 5) == NULL);
    CHECK(gp_string(0x7FFF0000u, 16) == NULL);

    /* A host pointer planted in guest memory is found by the qword scan. */
    CHECK(gm_scan_qwords(slot, 64, mod_lo, mod_hi, hits, 4) == 0);
    value = (uint64_t)(uintptr_t)&host_static;
    memcpy(gm_ptr(slot + 40, 8), &value, 8);
    n = gm_scan_qwords(GM_ANY_LO, GM_ANY_HI - GM_ANY_LO, mod_lo, mod_hi, hits, 4);
    CHECK(n == 1 && hits[0] == slot + 40);
    memset(gm_ptr(slot + 40, 8), 0, 8);
    CHECK(gm_scan_qwords(GM_ANY_LO, GM_ANY_HI - GM_ANY_LO, mod_lo, mod_hi, hits, 4) == 0);

    /* A guest MSG is 28 bytes, the host's 48: it crosses by copy, handles sign-extended. */
    CHECK(sizeof(MSG) != sizeof(guest_msg_t));
    CHECK(GP_COPY_OUT(slot, gm));
    memset(&back, 0, sizeof back);
    CHECK(GP_COPY_IN(back, slot));
    host.hwnd = (HWND)(intptr_t)(int32_t)back.hwnd;
    host.message = back.message;
    host.wParam = back.wParam;
    host.lParam = (LPARAM)(int32_t)back.lParam;
    host.time = back.time;
    host.pt.x = back.x;
    host.pt.y = back.y;
    CHECK((uint32_t)(uintptr_t)host.hwnd == gm.hwnd && host.pt.y == 32 && host.lParam == 0x00200010);
    CHECK(!GP_COPY_IN(back, 0x7FFFFFF0u));
    gm_heap_free(slot);
}

/* ---- images ---- */

typedef struct {
    gm_image_t* smack;
    gm_image_t* irt;
    int thunks, guest_exports;
} bind_ctx_t;

static uint64_t unbound_import(const uint32_t* args, void* user) {
    (void)args;
    (void)user;
    return 0;
}

static uint32_t resolve(const char* dll, const char* name, uint16_t ordinal, void* user) {
    bind_ctx_t* c = (bind_ctx_t*)user;
    char* label;
    size_t n;
    if (name && c->smack && !_stricmp(dll, "smackw32.dll")) {
        c->guest_exports++;
        return gm_pe_export(c->smack, name, 0);
    }
    if (name && c->irt && !_stricmp(dll, "MSVCIRT.dll")) {
        c->guest_exports++;
        return gm_pe_export(c->irt, name, 0);
    }
    n = strlen(dll) + (name ? strlen(name) : 8) + 2;
    label = (char*)malloc(n);
    if (!label) return 0;
    if (name) snprintf(label, n, "%s!%s", dll, name);
    else snprintf(label, n, "%s#%u", dll, ordinal);
    c->thunks++;
    return ht_register(label, unbound_import, 0, HT_STDCALL, NULL);
}

static int read_file_dword(const char* path, uint32_t off, uint32_t* out) {
    FILE* f = fopen(path, "rb");
    int ok;
    if (!f) return 0;
    ok = fseek(f, (long)off, SEEK_SET) == 0 && fread(out, 4, 1, f) == 1;
    fclose(f);
    return ok;
}

static void test_images(const char* dir) {
    char exe[MAX_PATH], smk[MAX_PATH], prc[MAX_PATH], irt[MAX_PATH], err[128];
    gm_image_t timg, simg, pimg, iimg;
    const gm_section_t* sm;
    bind_ctx_t ctx;
    uint32_t bound = 0, iat, slot, d;
    int unresolved, have_smack, have_proc, have_irt;
    printf("[pe] retail images from %s\n", dir);
    snprintf(exe, sizeof exe, "%s\\trespass.exe", dir);
    snprintf(smk, sizeof smk, "%s\\SMACKW32.DLL", dir);
    snprintf(prc, sizeof prc, "%s\\Processor.dll", dir);
    snprintf(irt, sizeof irt, "%s\\MSVCIRT.DLL", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        skip("trespass.exe not found: image tests");
        return;
    }
    CHECK(gm_pe_map(exe, GM_PE_PREFERRED, 0, &timg, err, sizeof err) == 0);
    if (!timg.base) {
        printf("  map failed: %s\n", err);
        return;
    }
    CHECK(timg.base == 0x00400000u && timg.image_base == 0x00400000u);
    CHECK(timg.span == 0x347000u);
    CHECK(timg.entry == 0x00626BE2u);
    CHECK(timg.stamp == 0x361E4297u);
    CHECK(timg.nsections == 5);
    CHECK(timg.relocated_dwords == 0);
    sm = gm_pe_section(&timg, "SelfMod");
    CHECK(sm && sm->va == 0x0063B000u && sm->vsize == 0x9B75u);
    CHECK(gm_pe_section(&timg, ".data") && gm_pe_section(&timg, ".data")->vsize == 0xE4408u);
    /* SelfMod's first patch site still holds its placeholder; .data past its raw size is zero */
    CHECK(gp_read_u32(0x0063CC83u, &d) && d == 0xDEADBEEFu);
    CHECK(gp_read_u32(0x0066E100u, &d) && d == 0);
    CHECK(gm_pe_map(exe, GM_PE_PREFERRED, 0, &simg, err, sizeof err) != 0);

    memset(&ctx, 0, sizeof ctx);
    have_smack = GetFileAttributesA(smk) != INVALID_FILE_ATTRIBUTES;
    have_proc = GetFileAttributesA(prc) != INVALID_FILE_ATTRIBUTES;
    have_irt = GetFileAttributesA(irt) != INVALID_FILE_ATTRIBUTES;
    if (have_smack) {
        CHECK(gm_pe_map(smk, GM_PE_ANYWHERE, 0, &simg, err, sizeof err) == 0);
        CHECK(simg.base == 0x10000000u && simg.relocated_dwords == 0);
        CHECK(gm_pe_export(&simg, "_SmackOpen@12", 0) > simg.base &&
              gm_pe_export(&simg, "_SmackOpen@12", 0) < simg.base + simg.span);
        CHECK(gm_pe_export(&simg, "_NoSuchExport@0", 0) == 0);
        ctx.smack = &simg;
    } else {
        skip("SMACKW32.DLL not found");
    }
    if (have_proc) {
        uint32_t file_dword = 0, first_reloc_rva = 0, rel_off = 0;
        CHECK(gm_pe_map(prc, GM_PE_ANYWHERE, 0, &pimg, err, sizeof err) == 0);
        CHECK(pimg.image_base == 0x10000000u);
        if (have_smack) {
            const uint32_t* blk = (const uint32_t*)gm_ptr(pimg.base + pimg.reloc_rva, 10);
            const uint16_t* ent = (const uint16_t*)(blk + 2);
            const gm_section_t* s;
            int i;
            CHECK(pimg.base != 0x10000000u && pimg.relocated_dwords > 0);
            CHECK((ent[0] >> 12) == 3);
            first_reloc_rva = blk[0] + (ent[0] & 0x0FFFu);
            for (i = 0; i < pimg.nsections; i++) {
                s = &pimg.sections[i];
                if (pimg.base + first_reloc_rva >= s->va && pimg.base + first_reloc_rva < s->va + s->raw_size) {
                    IMAGE_DOS_HEADER dos;
                    IMAGE_NT_HEADERS32 nt;
                    IMAGE_SECTION_HEADER sh;
                    FILE* f = fopen(prc, "rb");
                    if (f && fread(&dos, sizeof dos, 1, f) == 1 && !fseek(f, dos.e_lfanew, SEEK_SET) &&
                        fread(&nt, sizeof nt, 1, f) == 1) {
                        int k;
                        for (k = 0; k < nt.FileHeader.NumberOfSections && fread(&sh, sizeof sh, 1, f) == 1; k++)
                            if (first_reloc_rva >= sh.VirtualAddress && first_reloc_rva < sh.VirtualAddress + sh.SizeOfRawData)
                                rel_off = sh.PointerToRawData + first_reloc_rva - sh.VirtualAddress;
                    }
                    if (f) fclose(f);
                }
            }
            CHECK(rel_off && read_file_dword(prc, rel_off, &file_dword));
            CHECK(gp_read_u32(pimg.base + first_reloc_rva, &d) && d == file_dword + (pimg.base - 0x10000000u));
        }
        CHECK(gm_pe_export(&pimg, "bGetProcessorInfo", 0) >= pimg.base &&
              gm_pe_export(&pimg, "bGetProcessorInfo", 0) < pimg.base + pimg.span);
    } else {
        skip("Processor.dll not found");
    }
    if (have_irt) {
        CHECK(gm_pe_map(irt, GM_PE_ANYWHERE, 0, &iimg, err, sizeof err) == 0);
        CHECK(iimg.base == 0x780A0000u);
        ctx.irt = &iimg;
    } else {
        skip("MSVCIRT.DLL not found");
    }

    unresolved = gm_pe_bind(&timg, resolve, &ctx, &bound);
    CHECK(unresolved == 0);
    CHECK(bound == 297);
    CHECK(ctx.thunks + ctx.guest_exports == 297);
    if (have_smack && have_irt) CHECK(ctx.guest_exports == 14 + 26);
    CHECK(gp_read_u32(0x00645000u, &iat) && ht_is_thunk(iat) && !strcmp(ht_name(iat), "ADVAPI32.dll!RegQueryValueExA"));
    CHECK(gp_read_u32(0x0064501Cu, &iat) && ht_is_thunk(iat) && !strcmp(ht_name(iat), "COMCTL32.dll#17"));
    /* every IAT slot is a thunk or an address inside a mapped guest image */
    {
        int ok = 1, k;
        for (slot = 0x00645000u; slot < 0x00645000u + 0x4D0u; slot += 4) {
            uint32_t lo;
            if (!gp_read_u32(slot, &iat) || !iat) continue;
            if (!ht_is_thunk(iat) && !(gm_region(iat, &lo, NULL, NULL) && lo != 0)) ok = 0;
        }
        CHECK(ok);
        {
            HMODULE self = GetModuleHandleA(NULL);
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)self + ((IMAGE_DOS_HEADER*)self)->e_lfanew);
            uint64_t lo = (uint64_t)(uintptr_t)self;
            k = (int)gm_scan_qwords(timg.base, timg.span, lo, lo + nt->OptionalHeader.SizeOfImage, NULL, 0);
            CHECK(k == 0);
        }
    }
    if (have_irt) {
        uint32_t cout = gm_pe_export(&iimg, "?cout@@3Vostream_withassign@@A", 0);
        CHECK(cout >= iimg.base && cout < iimg.base + iimg.span);
        CHECK(gm_pe_section(&iimg, ".data") != NULL);
    }
}

/* ---- host COM objects behind guest vtables ---- */

typedef struct {
    uint32_t refs;
    int32_t total;
    const char* name;
} counter_t;

static const uint8_t IID_COUNTER[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static uint64_t m_query(const uint32_t* a, void* user) {
    uint8_t iid[16];
    (void)user;
    if (!ht_object_host(a[0])) return 0x80004003u;
    if (!GP_COPY_IN(iid, a[1]) || memcmp(iid, IID_COUNTER, 16)) {
        gp_write_u32(a[2], 0);
        return 0x80004002u;
    }
    ((counter_t*)ht_object_host(a[0]))->refs++;
    return gp_write_u32(a[2], a[0]) ? 0 : 0x80004003u;
}

static uint64_t m_addref(const uint32_t* a, void* user) {
    counter_t* c = (counter_t*)ht_object_host(a[0]);
    (void)user;
    return c ? ++c->refs : 0;
}

static uint64_t m_release(const uint32_t* a, void* user) {
    counter_t* c = (counter_t*)ht_object_host(a[0]);
    (void)user;
    if (!c) return 0;
    if (--c->refs) return c->refs;
    ht_object_free(a[0]);
    return 0;
}

static uint64_t m_add(const uint32_t* a, void* user) {
    counter_t* c = (counter_t*)ht_object_host(a[0]);
    (void)user;
    if (!c) return 0x80004003u;
    c->total += (int32_t)a[1];
    return (uint32_t)c->total;
}

static uint64_t m_get_name(const uint32_t* a, void* user) {
    counter_t* c = (counter_t*)ht_object_host(a[0]);
    uint32_t n = (uint32_t)strlen(c ? c->name : "") + 1;
    (void)user;
    if (!c || a[2] < n || !gp_copy_out(a[1], c->name, n)) return 0x80070057u;
    return 0;
}

static uint64_t m_get_self(const uint32_t* a, void* user) {
    counter_t* c = (counter_t*)ht_object_host(a[0]);
    (void)user;
    if (!c) return 0x80004003u;
    if (!gp_store_ptr(a[1], c)) return 0x80004005u;
    return 0;
}

static uint64_t m_wide(const uint32_t* a, void* user) {
    (void)user;
    return ((uint64_t)a[1] << 32) | a[2];
}

static const ht_method_t k_counter_methods[] = {
    {"ICounter::QueryInterface", m_query, 3},
    {"ICounter::AddRef", m_addref, 1},
    {"ICounter::Release", m_release, 1},
    {"ICounter::Add", m_add, 2},
    {"ICounter::GetName", m_get_name, 3},
    {"ICounter::GetSelf", m_get_self, 2},
    {"ICounter::Wide", m_wide, 3},
};

/* What lifted code does for `push args; push this; mov eax,[this]; call [eax+slot*4]`. */
static uint32_t guest_call(uint32_t* esp, uint32_t obj, int slot, int nargs, const uint32_t* args, uint32_t* edx,
                           int* ok) {
    uint32_t vtbl = 0, fn = 0, eax = 0, before = *esp;
    int i;
    for (i = nargs - 1; i >= 0; i--) {
        *esp -= 4;
        gp_write_u32(*esp, args[i]);
    }
    *esp -= 4;
    gp_write_u32(*esp, obj);
    *esp -= 4;
    gp_write_u32(*esp, 0xDEAD0000u);
    *ok = gp_read_u32(obj, &vtbl) && gp_read_u32(vtbl + 4u * (uint32_t)slot, &fn) && ht_invoke(fn, esp, &eax, edx);
    *ok = *ok && *esp == before;
    return eax;
}

static void test_com(void) {
    counter_t host = {1, 0, "counter-1"};
    uint32_t stack, esp, vt, obj, buf, args[3], edx = 0, r, out, hits[2];
    int ok;
    printf("[com] guest vtable -> host thunks\n");
    CHECK(ht_init());
    stack = gm_alloc_pages(0, 0x10000, "guest stack");
    CHECK(stack != 0);
    esp = stack + 0x10000 - 16;
    vt = ht_vtable(k_counter_methods, 7, NULL);
    CHECK(vt != 0);
    for (int i = 0; i < 7; i++) {
        uint32_t fn = 0;
        CHECK(gp_read_u32(vt + 4u * (uint32_t)i, &fn) && ht_is_thunk(fn) && fn < 0x80000000u);
        CHECK(!strcmp(ht_name(fn), k_counter_methods[i].name));
    }
    obj = ht_object_new(vt, 8, &host);
    CHECK(obj && ht_object_host(obj) == &host && ht_object_of(&host) == obj);

    args[0] = 5;
    CHECK(guest_call(&esp, obj, 3, 1, args, NULL, &ok) == 5 && ok);
    args[0] = (uint32_t)-7;
    CHECK((int32_t)guest_call(&esp, obj, 3, 1, args, NULL, &ok) == -2 && ok);

    buf = gm_heap_alloc(64, 1);
    args[0] = buf;
    args[1] = 64;
    CHECK(guest_call(&esp, obj, 4, 2, args, NULL, &ok) == 0 && ok && !strcmp(gp_string(buf, 64), "counter-1"));
    args[1] = 4;
    CHECK(guest_call(&esp, obj, 4, 2, args, NULL, &ok) == 0x80070057u && ok);

    /* QueryInterface hands back the guest object, never the host one */
    gp_copy_out(buf, IID_COUNTER, 16);
    out = buf + 32;
    args[0] = buf;
    args[1] = out;
    CHECK(guest_call(&esp, obj, 0, 2, args, NULL, &ok) == 0 && ok);
    CHECK(gp_read_u32(out, &r) && r == obj && host.refs == 2);

    /* a method that tries to return its host pointer is refused and counted */
    gp_leak_reset();
    args[0] = out;
    CHECK(guest_call(&esp, obj, 5, 1, args, NULL, &ok) == 0x80004005u && ok);
    CHECK(gp_leak_count() == 1);

    args[0] = 0x11111111u;
    args[1] = 0x22222222u;
    CHECK(guest_call(&esp, obj, 6, 2, args, &edx, &ok) == 0x22222222u && edx == 0x11111111u && ok);

    CHECK(gm_scan_qwords(GM_ANY_LO, GM_ANY_HI - GM_ANY_LO, (uint64_t)(uintptr_t)&host,
                         (uint64_t)(uintptr_t)&host + sizeof host, hits, 2) == 0);

    /* forged and stale objects resolve to nothing */
    {
        uint32_t forged = gm_heap_alloc(8, 1), h = 0;
        gp_read_u32(obj + 4, &h);
        gp_write_u32(forged, vt);
        gp_write_u32(forged + 4, h);
        CHECK(ht_object_host(forged) == NULL);
        CHECK(guest_call(&esp, forged, 3, 1, args, NULL, &ok) == 0x80004003u && ok);
        gm_heap_free(forged);
    }
    CHECK(guest_call(&esp, obj, 2, 0, args, NULL, &ok) == 1 && ok);
    CHECK(guest_call(&esp, obj, 2, 0, args, NULL, &ok) == 0 && ok);
    CHECK(ht_object_host(obj) == NULL && ht_object_count() == 0);

    CHECK(!ht_invoke(0x7F000004u, &esp, &r, NULL));
    CHECK(!ht_invoke(0x00401000u, &esp, &r, NULL));
    CHECK(faults_on_read(gm_base() + vt + 0) == 0);
    CHECK(faults_on_read(gm_base() + GM_THUNK_LO));
    gm_heap_free(buf);
}

int main(int argc, char** argv) {
    char dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TRESPASSER_GAME_DIR", dir, sizeof dir);
    if (argc > 1) snprintf(dir, sizeof dir, "%s", argv[1]);
    else if (!n || n >= sizeof dir) snprintf(dir, sizeof dir, "game");
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("x64_tests: %d-bit host\n", (int)(sizeof(void*) * 8));
    test_window();
    if (!gm_ready()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    test_conversion();
    test_reservations();
    test_heap();
    test_marshalling();
    test_images(dir);
    test_com();
    printf("guest regions %d, thunks %d\n", gm_region_count(), ht_count());
    printf("%d passed, %d failed, %d skipped\n", s_pass, s_fail, s_skip);
    gm_shutdown();
    return s_fail ? 1 : 0;
}

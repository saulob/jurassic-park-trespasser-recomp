/*
 * See guest_memory.h. The window is one address-space reservation (host_os.h): guest VirtualAlloc/VirtualFree map
 * onto commits and decommits inside it, on Windows x64 and on 64-bit POSIX alike.
 *
 * The reservation table, the 64 KB first fit and the heap's size classes are adapted from
 * pcrecomp runtime/win32hle/guest_mem.c (MIT); the PE structures come from its pe_format.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "host_os.h"
#include "pe_format.h"

#if UINTPTR_MAX != 0xFFFFFFFFFFFFFFFFull
#error "src/host64 is the 64-bit host"
#endif

#define MAX_REGIONS 8192
#define TAIL_GUARD 0x10000u /* MEM64 at 0xFFFFFFFC must fault, not reach host memory */

typedef struct {
    uint64_t lo, hi;
    const char* tag;
} region_t;

static uint8_t* s_base;
static hos_rwlock_t s_lock = HOS_RWLOCK_INIT;
static region_t s_rgn[MAX_REGIONS];
static int s_nrgn;
static volatile int32_t s_bad_va;

int gm_ready(void) { return s_base != NULL; }
uint8_t* gm_base(void) { return s_base; }
unsigned gm_bad_va_count(void) { return (unsigned)s_bad_va; }

static void heap_reset(void);

int gm_init(uint64_t preferred_base, int require) {
    if (s_base) return 1;
    s_base = hos_reserve(preferred_base, (size_t)(GM_SPAN + TAIL_GUARD), require);
    s_nrgn = 0;
    heap_reset();
    return s_base != NULL;
}

void gm_shutdown(void) {
    if (s_base) hos_release(s_base, (size_t)(GM_SPAN + TAIL_GUARD));
    s_base = NULL;
    s_nrgn = 0;
    heap_reset();
}

static int overlaps(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < s_nrgn; i++)
        if (lo < s_rgn[i].hi && hi > s_rgn[i].lo) return 1;
    return 0;
}

static void add_region(uint64_t lo, uint64_t hi, const char* tag) {
    int i = s_nrgn;
    while (i > 0 && s_rgn[i - 1].lo > lo) {
        s_rgn[i] = s_rgn[i - 1];
        i--;
    }
    s_rgn[i].lo = lo;
    s_rgn[i].hi = hi;
    s_rgn[i].tag = tag;
    s_nrgn++;
}

static int find_region(uint64_t va) {
    int lo = 0, hi = s_nrgn - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (va < s_rgn[mid].lo) hi = mid - 1;
        else if (va >= s_rgn[mid].hi) lo = mid + 1;
        else return mid;
    }
    return -1;
}

static int forbidden(uint64_t lo, uint64_t hi) {
    return lo < GM_NULL_GUARD_END || hi > GM_SPAN || (lo < GM_THUNK_HI && hi > GM_THUNK_LO);
}

static uint32_t find_gap(uint64_t n) {
    uint64_t at = GM_ANY_LO;
    for (int i = 0; i <= s_nrgn; i++) {
        uint64_t next = i < s_nrgn ? s_rgn[i].lo : GM_ANY_HI;
        if (next > GM_ANY_HI) next = GM_ANY_HI;
        if (next > at && next - at >= n) return (uint32_t)at;
        if (i < s_nrgn && s_rgn[i].hi > at) at = s_rgn[i].hi;
        if (at >= GM_ANY_HI) break;
    }
    return 0;
}

static uint32_t reserve_locked(uint32_t va, uint32_t size, const char* tag) {
    uint64_t lo, hi;
    if (!s_base || !size || s_nrgn >= MAX_REGIONS) return 0;
    if (va) {
        lo = va & ~(uint64_t)(GM_GRANULARITY - 1);
        hi = ((uint64_t)va + size + GM_GRANULARITY - 1) & ~(uint64_t)(GM_GRANULARITY - 1);
        if (forbidden(lo, hi) || overlaps(lo, hi)) return 0;
    } else {
        uint64_t n = ((uint64_t)size + GM_GRANULARITY - 1) & ~(uint64_t)(GM_GRANULARITY - 1);
        lo = find_gap(n);
        if (!lo) return 0;
        hi = lo + n;
    }
    add_region(lo, hi, tag ? tag : "");
    return (uint32_t)lo;
}

uint32_t gm_reserve(uint32_t va, uint32_t size, const char* tag) {
    uint32_t r;
    hos_wrlock(&s_lock);
    r = reserve_locked(va, size, tag);
    hos_unlock_wr(&s_lock);
    return r;
}

static int inside_one_region(uint32_t va, uint32_t size) {
    int i;
    if (!size) return 0;
    hos_rdlock(&s_lock);
    i = find_region(va);
    i = i >= 0 && (uint64_t)va + size <= s_rgn[i].hi;
    hos_unlock_rd(&s_lock);
    return i;
}

int gm_commit(uint32_t va, uint32_t size, int prot) {
    if (!inside_one_region(va, size)) return 0;
    return hos_commit(s_base + va, size, prot);
}

int gm_decommit(uint32_t va, uint32_t size) {
    if (!inside_one_region(va, size)) return 0;
    return hos_decommit(s_base + va, size);
}

int gm_protect(uint32_t va, uint32_t size, int prot) {
    if (!inside_one_region(va, size)) return 0;
    return hos_protect(s_base + va, size, prot);
}

int gm_release(uint32_t va) {
    int i, ok = 0;
    hos_wrlock(&s_lock);
    i = find_region(va);
    if (i >= 0 && s_rgn[i].lo == va) {
        hos_decommit(s_base + s_rgn[i].lo, (size_t)(s_rgn[i].hi - s_rgn[i].lo));
        memmove(&s_rgn[i], &s_rgn[i + 1], sizeof s_rgn[0] * (size_t)(s_nrgn - i - 1));
        s_nrgn--;
        ok = 1;
    }
    hos_unlock_wr(&s_lock);
    return ok;
}

uint32_t gm_alloc_pages(uint32_t va, uint32_t size, const char* tag) {
    uint32_t r = gm_reserve(va, size, tag);
    uint64_t end;
    if (!r) return 0;
    end = (uint64_t)(va ? va : r) + size;
    if (!gm_commit(r, (uint32_t)(((end - r) + GM_PAGE - 1) & ~(uint64_t)(GM_PAGE - 1)), GM_PROT_RW)) {
        gm_release(r);
        return 0;
    }
    return r;
}

int gm_region(uint32_t va, uint32_t* lo, uint32_t* size, const char** tag) {
    int i;
    hos_rdlock(&s_lock);
    i = find_region(va);
    if (i >= 0) {
        if (lo) *lo = (uint32_t)s_rgn[i].lo;
        if (size) *size = (uint32_t)(s_rgn[i].hi - s_rgn[i].lo);
        if (tag) *tag = s_rgn[i].tag;
    }
    hos_unlock_rd(&s_lock);
    return i >= 0;
}

int gm_region_count(void) { return s_nrgn; }

void* gm_ptr(uint32_t va, uint32_t len) {
    if (!s_base || va < GM_NULL_GUARD_END || (uint64_t)va + len > GM_SPAN) return NULL;
    return s_base + va;
}

void* gm_ptr_checked(uint32_t va, uint32_t len, int writable) {
    uint8_t* p = (uint8_t*)gm_ptr(va, len);
    return p && hos_accessible(p, len, writable) ? p : NULL;
}

int gm_contains(const void* p, size_t len) {
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)s_base;
    return s_base && a >= b && (uint64_t)(a - b) + len <= GM_SPAN;
}

uint32_t gm_va(const void* p) {
    if (!p) return 0;
    if (!gm_contains(p, 1)) {
        hos_atomic_inc(&s_bad_va);
        return 0;
    }
    return (uint32_t)((uintptr_t)p - (uintptr_t)s_base);
}

/* ---- heap ---- */

#define HDR 16u
#define MAGIC 0x48454150u
#define SMALL_MAX 0x8000u
#define CARVE 0x100000u
#define NCLASS 64
#define LARGE 0xFFu

static uint32_t s_free[NCLASS];
static uint32_t s_carve, s_carve_end;

static void heap_reset(void) {
    memset(s_free, 0, sizeof s_free);
    s_carve = s_carve_end = 0;
}

static uint32_t class_size(int c) {
    int k;
    uint32_t p;
    if (c < 7) return 32u + 16u * (uint32_t)c;
    k = c - 7;
    p = 128u << (k / 4);
    return p + p / 4u * (uint32_t)(k % 4 + 1);
}

static int class_of(uint32_t total) {
    for (int c = 0; c < NCLASS; c++)
        if (class_size(c) >= total) return c;
    return -1;
}

static uint32_t* hdr(uint32_t block) { return (uint32_t*)(s_base + block); }

static uint32_t small_alloc_locked(int c) {
    uint32_t va = s_free[c], sz = class_size(c);
    if (va) {
        s_free[c] = *(uint32_t*)(s_base + va + HDR);
        return va;
    }
    if (!s_carve || s_carve + sz > s_carve_end) {
        uint32_t r = reserve_locked(0, CARVE, "heap");
        if (!r) return 0;
        if (!hos_commit(s_base + r, CARVE, HOS_PROT_RW)) return 0;
        s_carve = r;
        s_carve_end = r + CARVE;
    }
    va = s_carve;
    s_carve += sz;
    return va;
}

uint32_t gm_heap_alloc(uint32_t n, int zero) {
    uint32_t total, va;
    int c;
    uint32_t* h;
    if (!s_base || n > 0xFFFFFFFFu - GM_GRANULARITY - HDR) return 0;
    total = n + HDR;
    c = total <= SMALL_MAX ? class_of(total) : -1;
    hos_wrlock(&s_lock);
    if (c >= 0) {
        va = small_alloc_locked(c);
    } else {
        va = reserve_locked(0, total, "heap-large");
        if (va && !hos_commit(s_base + va, (total + GM_PAGE - 1) & ~(GM_PAGE - 1), HOS_PROT_RW)) va = 0;
    }
    hos_unlock_wr(&s_lock);
    if (!va) return 0;
    h = hdr(va);
    h[0] = n;
    h[1] = MAGIC;
    h[2] = c >= 0 ? (uint32_t)c : LARGE;
    h[3] = 0;
    if (zero) memset(h + 4, 0, n);
    return va + HDR;
}

int gm_heap_ok(uint32_t va) {
    const uint32_t* h;
    if (va < GM_NULL_GUARD_END + HDR || !gm_ptr_checked(va - HDR, HDR, 0)) return 0;
    h = hdr(va - HDR);
    return h[1] == MAGIC && (h[2] < NCLASS || h[2] == LARGE);
}

uint32_t gm_heap_size(uint32_t va) { return gm_heap_ok(va) ? hdr(va - HDR)[0] : 0; }

void gm_heap_free(uint32_t va) {
    uint32_t* h;
    if (!va || !gm_heap_ok(va)) return;
    h = hdr(va - HDR);
    h[1] = 0;
    hos_wrlock(&s_lock);
    if (h[2] == LARGE) {
        int i = find_region(va - HDR);
        if (i >= 0) {
            hos_decommit(s_base + s_rgn[i].lo, (size_t)(s_rgn[i].hi - s_rgn[i].lo));
            memmove(&s_rgn[i], &s_rgn[i + 1], sizeof s_rgn[0] * (size_t)(s_nrgn - i - 1));
            s_nrgn--;
        }
    } else {
        *(uint32_t*)(s_base + va) = s_free[h[2]];
        s_free[h[2]] = va - HDR;
    }
    hos_unlock_wr(&s_lock);
}

static uint32_t capacity(const uint32_t* h) {
    return h[2] == LARGE ? ((h[0] + HDR + GM_GRANULARITY - 1) & ~(GM_GRANULARITY - 1)) - HDR : class_size((int)h[2]) - HDR;
}

uint32_t gm_heap_realloc(uint32_t va, uint32_t n, int zero) {
    uint32_t *h, old, nva;
    if (!va) return gm_heap_alloc(n, zero);
    if (!gm_heap_ok(va)) return 0;
    h = hdr(va - HDR);
    old = h[0];
    if (n <= capacity(h)) {
        if (zero && n > old) memset(s_base + va + old, 0, n - old);
        h[0] = n;
        return va;
    }
    nva = gm_heap_alloc(n, zero);
    if (!nva) return 0;
    memcpy(s_base + nva, s_base + va, old < n ? old : n);
    gm_heap_free(va);
    return nva;
}

/* ---- scratch ---- */

#define SCRATCH 0x40000u
static HOS_THREAD_LOCAL uint32_t t_scr_lo, t_scr_top;

uint32_t gm_scratch_mark(void) { return t_scr_top; }

uint32_t gm_scratch(uint32_t n) {
    uint32_t va;
    if (!t_scr_lo) {
        t_scr_lo = gm_heap_alloc(SCRATCH, 1);
        if (!t_scr_lo) return 0;
        t_scr_top = t_scr_lo;
    }
    va = (t_scr_top + 15u) & ~15u;
    if ((uint64_t)va + n > (uint64_t)t_scr_lo + SCRATCH) return 0;
    t_scr_top = va + n;
    memset(s_base + va, 0, n);
    return va;
}

uint32_t gm_scratch_copy(const void* src, uint32_t n) {
    uint32_t va = gm_scratch(n);
    if (va) memcpy(s_base + va, src, n);
    return va;
}

void gm_scratch_release(uint32_t mark) {
    if (t_scr_lo) t_scr_top = mark ? mark : t_scr_lo;
}

uint32_t gm_scan_qwords(uint32_t va, uint32_t len, uint64_t lo, uint64_t hi, uint32_t* hits, uint32_t max) {
    uint32_t found = 0;
    uint64_t at = ((uint64_t)va + 7) & ~7ull, end = (uint64_t)va + len;
    if (!s_base || end > GM_SPAN) return 0;
    while (at + 8 <= end) {
        uint64_t page_end = (at & ~(uint64_t)(GM_PAGE - 1)) + GM_PAGE;
        if (!gm_ptr_checked((uint32_t)at, 8, 0)) {
            at = page_end;
            continue;
        }
        for (; at + 8 <= end && at + 8 <= page_end; at += 8) {
            uint64_t v;
            memcpy(&v, s_base + at, 8);
            if (v >= lo && v < hi) {
                if (found < max && hits) hits[found] = (uint32_t)at;
                found++;
            }
        }
    }
    return found;
}

/* ---- PE32 images ---- */

static void set_err(char* err, size_t n, const char* msg) {
    if (err && n) {
        strncpy(err, msg, n - 1);
        err[n - 1] = 0;
    }
}

static uint32_t apply_relocs(uint32_t base, uint32_t rva, uint32_t size, int32_t delta) {
    uint32_t off = 0, n = 0;
    if (!rva || !size || !delta) return 0;
    while (off + sizeof(pe_base_reloc) <= size) {
        const pe_base_reloc* blk = (const pe_base_reloc*)gm_ptr(base + rva + off, sizeof(pe_base_reloc));
        uint32_t i, count;
        const uint16_t* ent;
        if (!blk || blk->SizeOfBlock < sizeof *blk || off + blk->SizeOfBlock > size) break;
        count = (blk->SizeOfBlock - (uint32_t)sizeof *blk) / 2;
        ent = (const uint16_t*)(blk + 1);
        for (i = 0; i < count; i++) {
            if ((ent[i] >> 12) == PE_REL_HIGHLOW) {
                uint32_t* p = (uint32_t*)gm_ptr(base + blk->VirtualAddress + (ent[i] & 0x0FFFu), 4);
                if (p) {
                    *p += (uint32_t)delta;
                    n++;
                }
            }
        }
        off += blk->SizeOfBlock;
    }
    return n;
}

int gm_pe_map_memory(const uint8_t* file, size_t len, int mode, uint32_t at, gm_image_t* img, char* err,
                     size_t errlen) {
    const pe_dos_header* dos = (const pe_dos_header*)file;
    const pe_nt_headers32* nt;
    const pe_opt_header32* opt;
    const pe_section_header* sec;
    uint32_t span, base = 0, want;
    int i, has_relocs;

    memset(img, 0, sizeof *img);
    if (!s_base) return set_err(err, errlen, "guest memory not initialised"), -1;
    if (len < sizeof *dos || dos->e_magic != 0x5A4D) return set_err(err, errlen, "not an MZ file"), -2;
    if (dos->e_lfanew < 0 || (size_t)dos->e_lfanew + sizeof(pe_nt_headers32) > len)
        return set_err(err, errlen, "bad e_lfanew"), -2;
    nt = (const pe_nt_headers32*)(file + dos->e_lfanew);
    opt = &nt->OptionalHeader;
    if (nt->Signature != 0x00004550u || opt->Magic != 0x10B || nt->FileHeader.Machine != 0x14C)
        return set_err(err, errlen, "not a PE32 i386 image"), -2;
    if (nt->FileHeader.NumberOfSections > GM_MAX_SECTIONS) return set_err(err, errlen, "too many sections"), -2;
    sec = (const pe_section_header*)((const uint8_t*)opt + nt->FileHeader.SizeOfOptionalHeader);
    if ((const uint8_t*)(sec + nt->FileHeader.NumberOfSections) > file + len)
        return set_err(err, errlen, "section table past the end of the file"), -2;

    span = (opt->SizeOfImage + GM_PAGE - 1) & ~(GM_PAGE - 1);
    has_relocs = opt->DataDirectory[PE_DIR_BASERELOC].Size != 0 && !(nt->FileHeader.Characteristics & 1);
    want = mode == GM_PE_AT ? at : opt->ImageBase;
    if (want != opt->ImageBase && !has_relocs) return set_err(err, errlen, "image has no relocations"), -3;
    base = gm_reserve(want, span, "image");
    if (!base && mode == GM_PE_ANYWHERE && has_relocs) base = gm_reserve(0, span, "image");
    if (!base) return set_err(err, errlen, "address range in use"), -3;
    if (!gm_commit(base, span, GM_PROT_RW)) {
        gm_release(base);
        return set_err(err, errlen, "commit failed"), -4;
    }
    if (opt->SizeOfHeaders > len || opt->SizeOfHeaders > span) {
        gm_release(base);
        return set_err(err, errlen, "bad SizeOfHeaders"), -2;
    }
    memcpy(gm_ptr(base, opt->SizeOfHeaders), file, opt->SizeOfHeaders);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t vsize = sec[i].VirtualSize ? sec[i].VirtualSize : sec[i].SizeOfRawData;
        uint32_t copy = sec[i].SizeOfRawData < vsize ? sec[i].SizeOfRawData : vsize;
        gm_section_t* s = &img->sections[i];
        if ((uint64_t)sec[i].VirtualAddress + vsize > span) {
            gm_release(base);
            return set_err(err, errlen, "section outside SizeOfImage"), -2;
        }
        if (copy && (uint64_t)sec[i].PointerToRawData + copy <= len)
            memcpy(gm_ptr(base + sec[i].VirtualAddress, copy), file + sec[i].PointerToRawData, copy);
        memcpy(s->name, sec[i].Name, 8);
        s->name[8] = 0;
        s->va = base + sec[i].VirtualAddress;
        s->vsize = vsize;
        s->raw_size = sec[i].SizeOfRawData;
        s->characteristics = sec[i].Characteristics;
    }
    img->nsections = nt->FileHeader.NumberOfSections;
    img->base = base;
    img->image_base = opt->ImageBase;
    img->span = span;
    img->entry = opt->AddressOfEntryPoint ? base + opt->AddressOfEntryPoint : 0;
    img->import_rva = opt->DataDirectory[PE_DIR_IMPORT].VirtualAddress;
    img->import_size = opt->DataDirectory[PE_DIR_IMPORT].Size;
    img->export_rva = opt->DataDirectory[0].VirtualAddress;
    img->export_size = opt->DataDirectory[0].Size;
    img->resource_rva = opt->DataDirectory[PE_DIR_RESOURCE].VirtualAddress;
    img->resource_size = opt->DataDirectory[PE_DIR_RESOURCE].Size;
    img->reloc_rva = opt->DataDirectory[PE_DIR_BASERELOC].VirtualAddress;
    img->reloc_size = opt->DataDirectory[PE_DIR_BASERELOC].Size;
    img->stamp = nt->FileHeader.TimeDateStamp;
    img->machine = nt->FileHeader.Machine;
    img->characteristics = nt->FileHeader.Characteristics;
    if (base != opt->ImageBase)
        img->relocated_dwords = apply_relocs(base, img->reloc_rva, img->reloc_size, (int32_t)(base - opt->ImageBase));
    return 0;
}

int gm_pe_map(const char* path, int mode, uint32_t at, gm_image_t* img, char* err, size_t errlen) {
    FILE* f = fopen(path, "rb");
    long size;
    uint8_t* buf;
    int r;
    if (!f) return set_err(err, errlen, "cannot open file"), -5;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return set_err(err, errlen, "bad file size"), -5;
    }
    buf = (uint8_t*)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return set_err(err, errlen, "read failed"), -5;
    }
    fclose(f);
    r = gm_pe_map_memory(buf, (size_t)size, mode, at, img, err, errlen);
    free(buf);
    return r;
}

const gm_section_t* gm_pe_section(const gm_image_t* img, const char* name) {
    for (int i = 0; i < img->nsections; i++)
        if (!strcmp(img->sections[i].name, name)) return &img->sections[i];
    return NULL;
}

static const char* guest_str(uint32_t va, uint32_t max) {
    const char* s = (const char*)gm_ptr_checked(va, 1, 0);
    uint32_t i;
    if (!s) return NULL;
    for (i = 0; i < max; i++) {
        if (!gm_ptr_checked(va + i, 1, 0)) return NULL;
        if (!s[i]) return s;
    }
    return NULL;
}

int gm_pe_bind(const gm_image_t* img, gm_resolve_fn resolve, void* user, uint32_t* bound) {
    uint32_t d, nbound = 0;
    int unresolved = 0;
    if (bound) *bound = 0;
    if (!img->import_rva) return 0;
    for (d = img->base + img->import_rva;; d += (uint32_t)sizeof(pe_import_descriptor)) {
        const pe_import_descriptor* desc = (const pe_import_descriptor*)gm_ptr_checked(d, sizeof *desc, 0);
        const char* dll;
        uint32_t ilt, iat;
        if (!desc || !desc->Name) break;
        dll = guest_str(img->base + desc->Name, 260);
        if (!dll) return -1;
        ilt = img->base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk);
        iat = img->base + desc->FirstThunk;
        for (;; ilt += 4, iat += 4) {
            uint32_t* in = (uint32_t*)gm_ptr_checked(ilt, 4, 0);
            uint32_t* out = (uint32_t*)gm_ptr_checked(iat, 4, 1);
            uint32_t va;
            if (!in || !out) return -1;
            if (!*in) break;
            if (*in & PE_ORDINAL_FLAG) {
                va = resolve(dll, NULL, (uint16_t)(*in & 0xFFFFu), user);
            } else {
                const char* name = guest_str(img->base + *in + 2, 512);
                va = name ? resolve(dll, name, 0, user) : 0;
            }
            *out = va;
            if (va) nbound++;
            else unresolved++;
        }
    }
    if (bound) *bound = nbound;
    return unresolved;
}

uint32_t gm_pe_export(const gm_image_t* img, const char* name, uint16_t ordinal) {
    const uint32_t* dir;
    uint32_t nfuncs, nnames, funcs, names, ords, rva = 0, i;
    if (!img->export_rva) return 0;
    dir = (const uint32_t*)gm_ptr_checked(img->base + img->export_rva, 40, 0);
    if (!dir) return 0;
    /* IMAGE_EXPORT_DIRECTORY: Base at +16, NumberOfFunctions +20, NumberOfNames +24, then the three RVAs */
    nfuncs = dir[5];
    nnames = dir[6];
    funcs = img->base + dir[7];
    names = img->base + dir[8];
    ords = img->base + dir[9];
    if (name) {
        for (i = 0; i < nnames; i++) {
            const uint32_t* np = (const uint32_t*)gm_ptr_checked(names + 4 * i, 4, 0);
            const uint16_t* op = (const uint16_t*)gm_ptr_checked(ords + 2 * i, 2, 0);
            const char* s = np ? guest_str(img->base + *np, 512) : NULL;
            if (s && op && !strcmp(s, name)) {
                const uint32_t* fp = *op < nfuncs ? (const uint32_t*)gm_ptr_checked(funcs + 4 * *op, 4, 0) : NULL;
                rva = fp ? *fp : 0;
                break;
            }
        }
    } else if (ordinal >= dir[4] && (uint32_t)(ordinal - dir[4]) < nfuncs) {
        const uint32_t* fp = (const uint32_t*)gm_ptr_checked(funcs + 4 * (uint32_t)(ordinal - dir[4]), 4, 0);
        rva = fp ? *fp : 0;
    }
    if (!rva || (rva >= img->export_rva && rva < img->export_rva + img->export_size)) return 0;
    return img->base + rva;
}

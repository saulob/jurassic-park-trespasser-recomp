/*
 * The guest's 32-bit address space inside a 64-bit host process.
 *
 * One 4 GB reservation holds every guest address; a guest VA is an offset into it, so lifted
 * code compiled without RECOMP_FLAT_MEMORY (ADDR(va) = g_mem_base + va) and host shims agree
 * on where everything lives. Pages are committed only when the guest (or the loader) asks.
 * Nothing in it is ever executable: the host runs lifted C, never guest bytes.
 *
 * Layout and heap size classes follow pcrecomp runtime/win32hle/guest_mem.c (MIT).
 */
#ifndef HOST64_GUEST_MEMORY_H
#define HOST64_GUEST_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GM_SPAN 0x100000000ull
#define GM_PAGE 0x1000u
#define GM_GRANULARITY 0x10000u
#define GM_NULL_GUARD_END 0x00010000u
#define GM_ANY_LO 0x01000000u /* above trespass.exe, below the 0x10000000 DLL bases */
#define GM_ANY_HI 0x7F000000u
#define GM_THUNK_LO 0x7F000000u
#define GM_THUNK_HI 0x7FFF0000u

enum { GM_PROT_NONE = 0, GM_PROT_R = 1, GM_PROT_RW = 2 };

/* Reserve the 4 GB window. preferred_base 0 lets the OS choose; otherwise that address is
 * tried first (a lift built with RECOMP_MEM_BASE needs exactly it: pass require=1). */
int gm_init(uint64_t preferred_base, int require);
void gm_shutdown(void);
int gm_ready(void);
uint8_t* gm_base(void);

/* Reservations, 64 KB aligned like VirtualAlloc. va 0: first fit in [GM_ANY_LO, GM_ANY_HI).
 * Return the reservation's VA, 0 on failure (overlap, null guard, thunk range, no room). */
uint32_t gm_reserve(uint32_t va, uint32_t size, const char* tag);
int gm_commit(uint32_t va, uint32_t size, int prot);
int gm_decommit(uint32_t va, uint32_t size);
int gm_protect(uint32_t va, uint32_t size, int prot);
int gm_release(uint32_t va);
/* reserve + commit read/write */
uint32_t gm_alloc_pages(uint32_t va, uint32_t size, const char* tag);

/* Lookup of the reservation containing va: its start, size and tag; 0 if none. */
int gm_region(uint32_t va, uint32_t* lo, uint32_t* size, const char** tag);
int gm_region_count(void);

/* VA <-> host pointer. gm_ptr checks only the range (NULL for va 0, the null guard, or a
 * range that leaves the 4 GB window); gm_ptr_checked also requires committed, readable
 * (and, if writable is set, writable) pages. gm_va returns 0 for a pointer outside the window
 * and counts it in gm_bad_va_count(). */
void* gm_ptr(uint32_t va, uint32_t len);
void* gm_ptr_checked(uint32_t va, uint32_t len, int writable);
uint32_t gm_va(const void* p);
int gm_contains(const void* p, size_t len);
unsigned gm_bad_va_count(void);

/* Guest heap: 16-byte header before each block; returns guest VAs. */
uint32_t gm_heap_alloc(uint32_t n, int zero);
uint32_t gm_heap_realloc(uint32_t va, uint32_t n, int zero);
void gm_heap_free(uint32_t va);
int gm_heap_ok(uint32_t va);
uint32_t gm_heap_size(uint32_t va);

/* Per-thread scratch for structures handed to the guest for one call, last in first out. */
uint32_t gm_scratch_mark(void);
uint32_t gm_scratch(uint32_t n);
uint32_t gm_scratch_copy(const void* src, uint32_t n);
void gm_scratch_release(uint32_t mark);

/* Host pointers that ended up in guest memory: every 8-byte-aligned qword in [va, va+len)
 * whose value lies in [lo, hi). Returns how many; fills up to max hits. Committed pages only. */
uint32_t gm_scan_qwords(uint32_t va, uint32_t len, uint64_t lo, uint64_t hi, uint32_t* hits, uint32_t max);

/* ---- 32-bit PE images ---- */
#define GM_MAX_SECTIONS 16
typedef struct {
    char name[9];
    uint32_t va, vsize, raw_size, characteristics;
} gm_section_t;

typedef struct {
    uint32_t base, image_base, span, entry;
    uint32_t import_rva, import_size, export_rva, export_size;
    uint32_t resource_rva, resource_size, reloc_rva, reloc_size;
    uint32_t stamp, relocated_dwords;
    uint16_t machine, characteristics;
    int nsections;
    gm_section_t sections[GM_MAX_SECTIONS];
} gm_image_t;

enum {
    GM_PE_PREFERRED = 0,   /* at ImageBase or fail */
    GM_PE_ANYWHERE = 1,    /* at ImageBase if free, else anywhere (needs relocations) */
    GM_PE_AT = 2           /* at the base given (needs relocations if it differs) */
};

/* Map a PE32 (i386) file: headers, sections at their VAs, .bss zeroed, relocations applied
 * when the base moved. Returns 0 on success, otherwise a negative code and a reason in err. */
int gm_pe_map(const char* path, int mode, uint32_t at, gm_image_t* img, char* err, size_t errlen);
int gm_pe_map_memory(const uint8_t* file, size_t len, int mode, uint32_t at, gm_image_t* img, char* err,
                     size_t errlen);
const gm_section_t* gm_pe_section(const gm_image_t* img, const char* name);

/* Fill the IAT the way the Windows loader would, asking resolve() for each import (name NULL
 * for an import by ordinal). Returns the number left unresolved (their slots are set to 0). */
typedef uint32_t (*gm_resolve_fn)(const char* dll, const char* name, uint16_t ordinal, void* user);
int gm_pe_bind(const gm_image_t* img, gm_resolve_fn resolve, void* user, uint32_t* bound);

/* An export of a mapped image by name (ordinal 0) or by ordinal (name NULL); 0 if none.
 * Forwarded exports are not followed (returns 0). */
uint32_t gm_pe_export(const gm_image_t* img, const char* name, uint16_t ordinal);

#ifdef __cplusplus
}
#endif

#endif

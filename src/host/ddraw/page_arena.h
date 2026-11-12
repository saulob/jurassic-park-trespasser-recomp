#ifndef TRESPASSER_PAGE_ARENA_H
#define TRESPASSER_PAGE_ARENA_H

/*
 * Write-watched pixel memory for small textures. VirtualAlloc reserves 64 KB of address space at a time, so the
 * thousands of small texture surfaces the game creates (each mip level is a surface) would cost far more address
 * space than pixels in this 32-bit process. They share 1 MB write-watched chunks instead, in runs of whole 4 KB
 * pages, so a write to one never marks another's pages.
 */

#include <stddef.h>
#include <stdint.h>

#define PAGE_ARENA_PAGE 4096u
#define PAGE_ARENA_CHUNK_PAGES 256u
#define PAGE_ARENA_MAX_BYTES (16u * PAGE_ARENA_PAGE)

/* First run of `need` clear bits in a bitmap of `total` bits; -1 if none. */
int page_run_find(const uint32_t* bitmap, unsigned total, unsigned need);
void page_run_mark(uint32_t* bitmap, unsigned first, unsigned count, int used);

/* Zeroed, write-watched memory of at least `bytes` (at most PAGE_ARENA_MAX_BYTES), or NULL. */
void* page_arena_alloc(size_t bytes);
void page_arena_free(void* p, size_t bytes);
/* Address space reserved by the chunks, and the bytes handed out. */
void page_arena_usage(size_t* reserved, size_t* used);

#endif

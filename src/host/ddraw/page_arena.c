#define WIN32_LEAN_AND_MEAN
#include "page_arena.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define CHUNK_BYTES (PAGE_ARENA_CHUNK_PAGES * PAGE_ARENA_PAGE)
#define MAX_CHUNKS 512u

typedef struct {
    uint8_t* base;
    uint32_t used[PAGE_ARENA_CHUNK_PAGES / 32];
    unsigned free_pages;
} chunk_t;

static chunk_t s_chunks[MAX_CHUNKS];
static unsigned s_chunk_count;
static size_t s_used_bytes;
static SRWLOCK s_lock = SRWLOCK_INIT;

int page_run_find(const uint32_t* bitmap, unsigned total, unsigned need) {
    unsigned run = 0;
    if (!need || need > total) return -1;
    for (unsigned i = 0; i < total; i++) {
        if (bitmap[i / 32] & (1u << (i % 32))) {
            run = 0;
            continue;
        }
        if (++run == need) return (int)(i + 1 - need);
    }
    return -1;
}

void page_run_mark(uint32_t* bitmap, unsigned first, unsigned count, int used) {
    for (unsigned i = first; i < first + count; i++) {
        if (used) bitmap[i / 32] |= 1u << (i % 32);
        else bitmap[i / 32] &= ~(1u << (i % 32));
    }
}

static unsigned pages_for(size_t bytes) { return (unsigned)((bytes + PAGE_ARENA_PAGE - 1) / PAGE_ARENA_PAGE); }

static void* take(chunk_t* c, unsigned pages) {
    int first = page_run_find(c->used, PAGE_ARENA_CHUNK_PAGES, pages);
    if (first < 0) return NULL;
    page_run_mark(c->used, (unsigned)first, pages, 1);
    c->free_pages -= pages;
    s_used_bytes += (size_t)pages * PAGE_ARENA_PAGE;
    return c->base + (size_t)first * PAGE_ARENA_PAGE;
}

void* page_arena_alloc(size_t bytes) {
    unsigned pages = pages_for(bytes);
    void* p = NULL;
    if (!bytes || bytes > PAGE_ARENA_MAX_BYTES) return NULL;
    AcquireSRWLockExclusive(&s_lock);
    for (unsigned i = 0; i < s_chunk_count && !p; i++)
        if (s_chunks[i].base && s_chunks[i].free_pages >= pages) p = take(&s_chunks[i], pages);
    if (!p && s_chunk_count < MAX_CHUNKS) {
        chunk_t* c = &s_chunks[s_chunk_count];
        c->base = (uint8_t*)VirtualAlloc(NULL, CHUNK_BYTES, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (c->base) {
            memset(c->used, 0, sizeof c->used);
            c->free_pages = PAGE_ARENA_CHUNK_PAGES;
            s_chunk_count++;
            p = take(c, pages);
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    return p;
}

/* The pages are zeroed and their write-watch state cleared before another surface can get them. */
void page_arena_free(void* p, size_t bytes) {
    unsigned pages = pages_for(bytes);
    AcquireSRWLockExclusive(&s_lock);
    for (unsigned i = 0; i < s_chunk_count; i++) {
        chunk_t* c = &s_chunks[i];
        size_t offset = (size_t)((uint8_t*)p - c->base);
        if (!c->base || (uint8_t*)p < c->base || offset >= CHUNK_BYTES) continue;
        memset(p, 0, (size_t)pages * PAGE_ARENA_PAGE);
        ResetWriteWatch(p, (size_t)pages * PAGE_ARENA_PAGE);
        page_run_mark(c->used, (unsigned)(offset / PAGE_ARENA_PAGE), pages, 0);
        c->free_pages += pages;
        s_used_bytes -= (size_t)pages * PAGE_ARENA_PAGE;
        break;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void page_arena_usage(size_t* reserved, size_t* used) {
    AcquireSRWLockShared(&s_lock);
    *reserved = (size_t)s_chunk_count * CHUNK_BYTES;
    *used = s_used_bytes;
    ReleaseSRWLockShared(&s_lock);
}

/*
 * The engine's fixed heaps: pHeapAlloc reserves one range per heap and pvCommitMem commits it on demand. pvCommitMem
 * ignores VirtualAlloc's result and returns the requested end, and CDArray::bCommit commits before it checks its
 * capacity, so a request past the reservation marks pages committed that are not: the next allocation that fits
 * writes into them. Requests are kept inside the reservation, and the heaps that modern scenes fill grow.
 */
#include "host.h"
#include "host_policy.h"

#define HEAP_RESERVE_VA 0x005F8A20u /* pHeapAlloc(bytes), cdecl: VirtualAlloc reserve */
#define HEAP_COMMIT_VA 0x005F8B10u  /* pvCommitMem(pv, bytes), cdecl: returns pv plus the page-rounded size */
#define POLYGON_COUNT 0x00732F2Cu   /* darpolyPolygons.uLen */
#define POLYGON_CAPACITY 0x00732F34u
#define VERTEX_COUNT 0x00732EF4u    /* darvVertices.uLen */
#define VERTEX_CAPACITY 0x00732EFCu
#define PAGE 0x1000u
#define MAX_RESERVATIONS 64

static recomp_func_t s_reserve_lifted, s_commit_lifted;
static int s_enlarged, s_guarded;
static struct {
    uint32_t base, end;
} s_reservations[MAX_RESERVATIONS];
static unsigned s_reservation_count, s_reservation_next;
static unsigned long long s_clamped;                /* DEBUG_SAULO */
static uint32_t s_polygon_peak, s_vertex_peak;      /* DEBUG_SAULO */

static void remember(uint32_t base, uint32_t bytes) {
    uint32_t end = base + ((bytes + PAGE - 1u) & ~(PAGE - 1u));
    for (unsigned i = 0; i < s_reservation_count; i++)
        if (s_reservations[i].base < end && base < s_reservations[i].end) {
            s_reservations[i].base = base;
            s_reservations[i].end = end;
            return;
        }
    if (s_reservation_count < MAX_RESERVATIONS) {
        s_reservations[s_reservation_count].base = base;
        s_reservations[s_reservation_count++].end = end;
        return;
    }
    s_reservations[s_reservation_next].base = base;
    s_reservations[s_reservation_next].end = end;
    s_reservation_next = (s_reservation_next + 1u) % MAX_RESERVATIONS;
}

static uint32_t reservation_end(uint32_t pv) {
    for (unsigned i = 0; i < s_reservation_count; i++)
        if (pv >= s_reservations[i].base && pv < s_reservations[i].end) return s_reservations[i].end;
    return 0;
}

/* The constructor stores the capacity, then reserves capacity x size: both grow together, never one alone. */
static void heap_reserve(void) {
    uint32_t bytes = GUEST_ARG(0);
    engine_heap_t heap;
    if (g_alias_entry) {
        s_reserve_lifted();
        return;
    }
    heap = engine_heap_for(bytes);
    if (heap.scale > 1u && !engine_limits_original()) {
        if (!heap.capacity_va) {
            GUEST_ARG(0) = bytes * heap.scale;
        } else if (MEM32(heap.capacity_va) == heap.capacity && !MEM32(heap.capacity_va - 4u)) {
            MEM32(heap.capacity_va) = heap.capacity * heap.scale;
            GUEST_ARG(0) = bytes * heap.scale;
        }
    }
    bytes = GUEST_ARG(0);
    s_reserve_lifted();
    if (g_eax) remember(g_eax, bytes);
}

static void heap_commit(void) {
    uint32_t pv, bytes, end, allowed;
    if (g_alias_entry) {
        s_commit_lifted();
        return;
    }
    pv = GUEST_ARG(0);
    bytes = GUEST_ARG(1);
    end = reservation_end(pv);
    if (end) {
        allowed = engine_heap_commit_bytes(pv, bytes, end, PAGE);
        if (allowed != bytes) {
            GUEST_ARG(1) = allowed;
            s_clamped++;
        }
    }
    if (MEM32(POLYGON_COUNT) > s_polygon_peak) s_polygon_peak = MEM32(POLYGON_COUNT);
    if (MEM32(VERTEX_COUNT) > s_vertex_peak) s_vertex_peak = MEM32(VERTEX_COUNT);
    if (end && !GUEST_ARG(1)) {
        g_eax = pv;
        GUEST_RETURN_STDCALL(0);
        return;
    }
    s_commit_lifted();
}

void engine_heaps_install(void) {
    s_enlarged = guest_override_redirect(HEAP_RESERVE_VA, heap_reserve, &s_reserve_lifted);
    s_guarded = s_enlarged && guest_override_redirect(HEAP_COMMIT_VA, heap_commit, &s_commit_lifted);
    if (!s_guarded)
        host_log("graphics", "pHeapAlloc/pvCommitMem have no incremental-link thunk: the engine's fixed heaps keep their "
                 "1998 size and a request past one can corrupt it");
}

int engine_heaps_enlarged(void) { return s_enlarged && !engine_limits_original(); }

void engine_heaps_log(void) {
    if (!s_guarded) return;
    host_log("graphics", "  scene heaps: up to %u polygons and %u vertices per scene (1998: 20000 and 80000), commits "
             "kept inside each heap's reservation", MEM32(POLYGON_CAPACITY), MEM32(VERTEX_CAPACITY));
}

/* DEBUG_SAULO: called with the renderer's 5-second performance lines. */
void engine_heaps_window(void) {
    if (!s_guarded || (!s_polygon_peak && !s_clamped)) return;
    host_log("objects", "last 5 s: scene heap peak %u polygons of %u, %u vertices of %u; commits cut at a reservation's "
             "end %llu", s_polygon_peak, MEM32(POLYGON_CAPACITY), s_vertex_peak, MEM32(VERTEX_CAPACITY), s_clamped);
    s_polygon_peak = s_vertex_peak = 0;
    s_clamped = 0;
}

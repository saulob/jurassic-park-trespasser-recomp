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
#define POLYGON_HEAP_BYTES 0x0025D780u /* darpolyPolygons */
#define VERTEX_HEAP_BYTES 0x30D400u    /* darvVertices */
#define PAGE 0x1000u
#define MAX_RESERVATIONS 64

static recomp_func_t s_reserve_lifted, s_commit_lifted;
static int s_enlarged, s_guarded;
static struct {
    uint32_t base, end;
} s_reservations[MAX_RESERVATIONS];
static unsigned s_reservation_count, s_reservation_next;
static int s_clamp_logged;

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
            if (!s_clamp_logged)
                host_log("graphics", "an engine heap is full: a commit at 0x%08X was kept inside its reservation (later "
                         "ones are not logged)", pv);
            s_clamp_logged = 1;
        }
    }
    if (end && !GUEST_ARG(1)) {
        g_eax = pv;
        GUEST_RETURN_STDCALL(0);
        return;
    }
    s_commit_lifted();
}

void engine_heaps_install(void) {
    s_enlarged = guest_override_install(HEAP_RESERVE_VA, heap_reserve, &s_reserve_lifted);
    s_guarded = s_enlarged && guest_override_install(HEAP_COMMIT_VA, heap_commit, &s_commit_lifted);
}

int engine_heaps_enlarged(void) { return s_enlarged && !engine_limits_original(); }

void engine_heaps_log(void) {
    engine_heap_t polygons = engine_heap_for(POLYGON_HEAP_BYTES), vertices = engine_heap_for(VERTEX_HEAP_BYTES);
    if (!s_guarded) return;
    host_log("graphics", "  scene heaps: capacity %u polygons and %u vertices per scene (1998: %u and %u), commits "
             "kept inside each heap's reservation", polygons.capacity * polygons.scale,
             vertices.capacity * vertices.scale, polygons.capacity, vertices.capacity);
}

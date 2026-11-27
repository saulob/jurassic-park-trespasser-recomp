/*
 * The engine's fixed heaps: pHeapAlloc reserves one range per heap and pvCommitMem commits it on demand. pvCommitMem
 * ignores VirtualAlloc's result and returns the requested end, and CDArray::bCommit commits before it checks its
 * capacity, so a request past the reservation marks pages committed that are not: the next allocation that fits
 * writes into them. Requests are kept inside the reservation, and the heaps that modern scenes fill grow. In a custom
 * level's session a request that does not fit ends the load before anything is written; a guard follows each heap for what
 * is written without being asked for.
 */
#include "guest_abi.h"
#include "engine_mode.h"
#include "jpt_custom_level.h"
#include "jpt_save.h"
#include "level_abort.h"

#include <string.h>

#define HEAP_RESERVE_VA GVA(0x005F8A20) /* pHeapAlloc(bytes), cdecl: VirtualAlloc reserve */
#define HEAP_COMMIT_VA GVA(0x005F8B10)  /* pvCommitMem(pv, bytes), cdecl: returns pv plus the page-rounded size */
#define POLYGON_HEAP_BYTES 0x0025D780u /* darpolyPolygons */
#define POLYGON_POINTER_HEAP_BYTES 0x13880u /* darppolyPolygons */
#define VERTEX_HEAP_BYTES 0x30D400u    /* darvVertices */
#define PAGE 0x1000u
#define GUARD 0x10000u
#define MAX_RESERVATIONS 256

static recomp_func_t s_reserve_lifted, s_commit_lifted;
static int s_enlarged, s_guarded;
typedef struct {
    uint32_t base, end;
    const char* name;
    uint32_t checked_len_va, element; /* of a heap whose users ask for room first and look at the capacity after */
} reservation_t;
static reservation_t s_reservations[MAX_RESERVATIONS];
static unsigned s_reservation_count, s_reservation_next;
static int s_clamp_logged;
static uint32_t s_left_out;
#define TERRAIN_THICKNESS_D3D GVA(0x00664D50)
#define TERRAIN_THICKNESS_OWN (-0.05f)

static engine_session_t s_session;
static float s_terrain_cull;

/* guard: of every reservation; one that is in the way of a new one was released. */
static void remember(const reservation_t* r, uint32_t guard) {
    for (unsigned i = 0; i < s_reservation_count;)
        if (s_reservations[i].base < r->end + guard && r->base < s_reservations[i].end + guard) s_reservations[i] = s_reservations[--s_reservation_count];
        else i++;
    if (s_reservation_count < MAX_RESERVATIONS) {
        s_reservations[s_reservation_count++] = *r;
        return;
    }
    s_reservations[s_reservation_next] = *r;
    s_reservation_next = (s_reservation_next + 1u) % MAX_RESERVATIONS;
}

static reservation_t* reservation_of(uint32_t pv) {
    for (unsigned i = 0; i < s_reservation_count; i++)
        if (pv >= s_reservations[i].base && pv < s_reservations[i].end) return &s_reservations[i];
    return NULL;
}

/* The heap whose guard the address is in. */
static reservation_t* full_heap(uint32_t pv) {
    if (!s_session.custom_level || reservation_of(pv)) return NULL;
    for (unsigned i = 0; i < s_reservation_count; i++)
        if (pv >= s_reservations[i].end && pv < s_reservations[i].end + GUARD) return &s_reservations[i];
    return NULL;
}

uint32_t engine_heaps_region_bytes(uint32_t bytes) {
    const uint32_t wanted = s_session.texture_pool_mb << 20;
    if (!s_session.custom_level || bytes != ENGINE_TEXTURE_POOL_BYTES || wanted <= bytes) return bytes;
    if (guest_reservation_fits(wanted)) return wanted;
    host_log("custom-level", "warning: no address space for a texture pool of %u bytes, %u reserved instead", wanted, bytes);
    return bytes;
}

/* A scene's sections pass through one buffer, read and written without looking at its end. */
uint32_t engine_heaps_buffer_bytes(uint32_t bytes) { return s_session.custom_level && bytes == JPT_SAVE_SECTION_BYTES ? JPT_SAVE_SECTION_BYTES_MAX : bytes; }

int engine_heaps_guard(uint32_t va) { return full_heap(va) != NULL; }

/* Every array and heap of the engine counts its elements, asks for their pages and only then writes them. Two of the scene's
 * arrays are asked for room that a shape may need: what they count is in use, and a shape without room is left out. */
static void refuse_overflow(const reservation_t* r, uint32_t pv, uint32_t bytes) {
    const uint32_t capacity = r->end - r->base, used = pv - r->base < capacity ? pv - r->base : capacity;
    if (!engine_heap_overflow(pv, bytes, r->end)) return;
    if (r->checked_len_va && MEM32(r->checked_len_va) <= capacity / r->element) {
        if (!s_left_out++) host_log("custom-level", "ENGINE_LIMIT_EXCEEDED: %s hold %u bytes; a shape without room is left out of the frame (later ones are not logged)", r->name, capacity);
        return;
    }
    level_limit_exceeded(r->name, r->base, used, bytes, capacity);
}

/* The constructor stores the capacity, then reserves capacity x size: both grow together, never one alone. */
static void heap_reserve(void) {
    uint32_t bytes = GUEST_ARG(0), usual, guard;
    unsigned scale = 1;
    engine_heap_t heap;
    reservation_t made = { 0, 0, engine_heap_name(GUEST_ARG(0)), 0, 0 };
    if (g_alias_entry) {
        s_reserve_lifted();
        return;
    }
    heap = engine_heap_for(bytes);
    if (heap.scale > 1u && !engine_limits_original()) {
        if (!heap.capacity_va) {
            scale = heap.scale;
        } else if (MEM32(heap.capacity_va) == heap.capacity && !MEM32(heap.capacity_va - 4u)) {
            MEM32(heap.capacity_va) = heap.capacity * heap.scale;
            scale = heap.scale;
        }
    }
    usual = bytes * scale;
    bytes = engine_session_heap_bytes(&s_session, bytes, scale);
    guard = s_session.custom_level ? GUARD : 0;
    if (s_session.custom_level && !guest_reservation_fits(bytes + guard)) {
        if (bytes == usual || !guest_reservation_fits(usual + guard)) level_allocation_failed(made.name, 0, bytes);
        host_log("custom-level", "warning: no address space for %u bytes, %u reserved instead", bytes, usual);
        bytes = usual;
    }
    GUEST_ARG(0) = bytes + guard;
    s_reserve_lifted();
    if (!g_eax) return;
    made.base = g_eax;
    made.end = g_eax + ((bytes + PAGE - 1u) & ~(PAGE - 1u));
    if (heap.bytes == POLYGON_HEAP_BYTES || heap.bytes == POLYGON_POINTER_HEAP_BYTES) {
        made.checked_len_va = heap.capacity_va - 8u;
        made.element = heap.bytes / heap.capacity;
    }
    remember(&made, guard);
}

static void heap_commit(void) {
    uint32_t pv, bytes, allowed;
    const reservation_t* r;
    if (g_alias_entry) {
        s_commit_lifted();
        return;
    }
    pv = GUEST_ARG(0);
    bytes = GUEST_ARG(1);
    r = reservation_of(pv);
    if (!r) r = full_heap(pv);
    if (r && s_session.custom_level) refuse_overflow(r, pv, bytes);
    if (r) {
        allowed = engine_heap_commit_bytes(pv, bytes, r->end, PAGE);
        if (allowed != bytes) {
            GUEST_ARG(1) = allowed;
            if (engine_heap_overflow(pv, bytes, r->end) && !s_clamp_logged++)
                host_log("graphics", "warning: an engine heap is full: %u bytes asked for at 0x%08X, %u left of its %u (later ones "
                         "are not logged)", bytes, pv, pv < r->end ? r->end - pv : 0u, r->end - r->base);
        }
        if (!GUEST_ARG(1)) {
            g_eax = pv;
            GUEST_RETURN_STDCALL(0);
            return;
        }
    }
    s_commit_lifted();
}

/* How far behind a terrain polygon the eye must be for it to be left out, with Direct3D. */
static void terrain_cull(float more) {
    float* thickness = (float*)ADDR(TERRAIN_THICKNESS_D3D);
    if (more > 0.0f ? *thickness != TERRAIN_THICKNESS_OWN : s_terrain_cull == 0.0f) return;
    *thickness = TERRAIN_THICKNESS_OWN - more;
    s_terrain_cull = more;
}

static void session_ended(void) {
    terrain_cull(0.0f);
    memset(&s_session, 0, sizeof s_session);
    jpt_save_set_buffer(JPT_SAVE_SECTION_BYTES);
    s_left_out = 0;
    s_clamp_logged = 0;
}

void engine_heaps_install(void) {
    s_enlarged = guest_override_install(HEAP_RESERVE_VA, heap_reserve, &s_reserve_lifted);
    s_guarded = s_enlarged && guest_override_install(HEAP_COMMIT_VA, heap_commit, &s_commit_lifted);
    session_ended();
    s_reservation_count = s_reservation_next = 0;
    jpt_custom_level_on_end(session_ended);
    s_session.custom_level = jpt_custom_level_active();
    if (s_session.custom_level) jpt_save_set_buffer(JPT_SAVE_SECTION_BYTES_MAX);
    s_session.mesh_faces = jpt_custom_level_limit("mesh_faces");
    s_session.mesh_heap_mb = jpt_custom_level_limit("mesh_heap_mb");
    s_session.extra_terrain_nodes = jpt_custom_level_limit("terrain_nodes");
    s_session.texture_pool_mb = jpt_custom_level_limit("texture_pool_mb");
    if (jpt_custom_level_requires(JPT_LEVEL_CAP_TERRAIN_CULL)) terrain_cull((float)jpt_custom_level_limit("terrain_cull_mm") / 1000.0f);
    if (!s_session.custom_level || !s_enlarged) return;
    host_log("custom-level", "engine limits for this session: %u polygons a mesh (1998: %u), %u MB for loaded meshes, %u more terrain nodes, %u MB for textures",
             s_session.mesh_faces ? s_session.mesh_faces : ENGINE_MESH_FACES, ENGINE_MESH_FACES,
             s_session.mesh_heap_mb ? s_session.mesh_heap_mb : ENGINE_MESH_HEAP_BYTES >> 20,
             s_session.extra_terrain_nodes * 1000u, s_session.texture_pool_mb ? s_session.texture_pool_mb : ENGINE_TEXTURE_POOL_BYTES >> 20);
}

int engine_heaps_enlarged(void) { return s_enlarged && !engine_limits_original(); }

void engine_heaps_log(void) {
    engine_heap_t polygons = engine_heap_for(POLYGON_HEAP_BYTES), vertices = engine_heap_for(VERTEX_HEAP_BYTES);
    if (!s_guarded) return;
    host_log("graphics", "  scene heaps: capacity %u polygons and %u vertices per scene (1998: %u and %u), commits "
             "kept inside each heap's reservation", polygons.capacity * polygons.scale,
             vertices.capacity * vertices.scale, polygons.capacity, vertices.capacity);
}

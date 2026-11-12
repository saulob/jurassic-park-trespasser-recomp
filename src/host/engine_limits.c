/*
 * The 1998 engine limits that Direct3D 11 lifts: how far objects stay drawn and stay 3D, the texture size Direct3D may
 * use (world textures and impostors) and the texture memory the game divides among them. Only guest data and
 * incremental-link thunks change; the software renderer keeps the original values, and each value is written back
 * when F5 changes the renderer.
 */
#include "host.h"
#include "host_policy.h"

#include <math.h>
#include <string.h>

#define CACHE_MIN_DISTANCE 0x0064E458u      /* bShouldCache's fMinDistanceToCache (.rdata) */
#define CULL_COMBINED 0x006634ECu           /* fCullDistanceCombined: multiplies every squared cull distance */
#define RECOMMENDED_TEX_DIM 0x006D0F1Cu     /* d3dDriver.iRecommendedMaxDim, from "Max Recommended Tex Dim" */
#define LARGE_MEM_ALLOC_VA 0x005072D0u      /* CDirect3D::iLargeMemAllocMB */
#define CACHE_PARAMETERS_VA 0x005477E0u     /* CRenderCache::SetParameters */
#define TERRAIN_TEXTURE_MEM_VA 0x00492200u  /* CTextureNode::SetTextureMemSize(KB) */
#define ALLOCATE_TEXTURE_MEM_VA 0x00506270u /* CDirect3D::AllocateTextureMemory */
#define ALLOCATE_NON_LOCAL_VA 0x00506EB0u   /* CDirect3D::bAllocateNonLocalMemory: the split */
#define ALLOCATE_MANAGED_VA 0x00506C00u     /* the same split for managed textures */
#define CACHE_MEMORY_VA 0x00547710u         /* CRenderCache::SetTextureMemSize(system KB, video KB), cdecl */
#define AGP_ALLOCATE_VA 0x00514FE0u         /* CAGPTextureMemManager::AllocateMemory(bytes) */
#define SET_COMBINED_CULLING_VA 0x00477E00u /* (objects, shadows), cdecl */
#define SET_GLOBAL_PARTITION_DATA_VA 0x004A24B0u
#define IN_RANGE_VA 0x00477A40u             /* CPartition::bInRange(distance squared) */
#define SHOULD_CACHE_VA 0x00543830u         /* bShouldCache(partition, camera, distance squared, ...), cdecl */
#define FLAG_CACHEABLE_VA 0x00478A30u       /* CPartition::bFlagCacheable, thiscall */
#define CACHE_UPDATE_VA 0x00545820u         /* CRenderCache::UpdateRenderContext: rebuilds an impostor's camera */
#define CACHE_DRAW_VA 0x00547A30u           /* CScheduleCache::PostScheduleExecute: renders it into its page */
#define MAX_OBJECTS_PER_CACHE 0x007371A0u   /* rcsRenderCacheSettings.iMaxObjectsPerCache */
#define D3D_DRIVER 0x006D0E98u
#define D3D_IN_USE (D3D_DRIVER + 0x1u)          /* CDirect3D::bUse */
#define D3D_TEXTURE_TOTAL (D3D_DRIVER + 0x604u) /* bytes split among the texture pools */
#define AGP_TEXTURE_MANAGER 0x006D2040u         /* +8 allocated, +0xC 85% mark, +0x10 95% mark */
#define TRANSITION_SLOTS 4096u
#define SOFTWARE_SLOTS 8192u
/* CPartition: CShortFloats (a float's high 16 bits), SPartitionData, cache state. */
#define PARTITION_CACHE_MUL 0x4u
#define PARTITION_CYLINDER_SQR 0xAu
#define PARTITION_SPHERE_SQR 0xCu
#define PARTITION_AREA_EFFICIENCY_SQR 0xEu
#define PARTITION_FLAGS 0x10u
#define PARTITION_NO_CACHE_UNTIL 0x14u
#define PARTITION_RENDER_CACHE 0x18u
#define PARTITION_PARENT 0x24u
#define PARTITION_FRAME 0x006648F4u     /* SPartitionSettings::u4FrameNumber */
#define MAX_ACCEPT_CYLINDER 0x00737190u /* rcsRenderCacheSettings.fMaxAcceptCylinder */
#define AREA_THRESHOLD_SQR 0x0066F510u  /* CPartition::fAreaEfficiencyThresholdSqr */

static recomp_func_t s_large_mem_lifted, s_cache_parameters_lifted, s_terrain_mem_lifted, s_allocate_lifted;
static recomp_func_t s_non_local_lifted, s_managed_lifted, s_cache_memory_lifted, s_agp_allocate_lifted;
static recomp_func_t s_combined_lifted, s_in_range_lifted, s_should_cache_lifted, s_flag_cacheable_lifted;
static recomp_func_t s_cache_update_lifted, s_cache_draw_lifted;
static int s_group_objects_game;
static unsigned long s_cache_never_seen, s_cache_never_lifted;
static int s_keep_original, s_modern, s_installed, s_fixed_split, s_cache_distance_writable;
static unsigned s_accepted, s_rejected;
static struct {
    int active;
    int32_t engine_cache, host_cache;
} s_split;

/* DEBUG_SAULO: object distance decisions per 5 s window and for the session. */
typedef struct {
    unsigned long long meshes_in_range, groups_in_range, beyond;
    unsigned long long meshes_3d, meshes_impostor, groups_3d, groups_impostor, software_3d, new_caches;
    unsigned long long to_impostor, to_3d;
    unsigned long long mesh_refused[CACHE_REFUSED_COUNT], group_refused[CACHE_REFUSED_COUNT];
    unsigned long long projected_culls, with_their_object;
    unsigned long long cache_updates, cache_update_ticks, cache_draws, cache_draw_ticks;
} object_stats_t;
/* Objects (instances, not spatial groups) that stayed 3D this frame: their parts stay 3D with them. */
#define OBJECT_SLOTS 4096u
static engine_frame_slot_t s_kept_3d[OBJECT_SLOTS];
#define OBJECT_DEPTH 16
static object_stats_t s_objects, s_objects_total;
static struct {
    uint32_t partition;
    uint8_t impostor;
} s_last_decision[TRANSITION_SLOTS];
/* Partitions holding a mesh the CPU draws into the Direct3D frame (a bump map makes it so). */
static uint32_t s_software[SOFTWARE_SLOTS];
static unsigned s_software_count;

void engine_limits_keep_original(void) { s_keep_original = 1; }

int engine_limits_original(void) { return s_keep_original; }

int engine_limits_modern(void) { return s_modern; }

static float object_cull_combined(void) { return ENGINE_OBJECT_DISTANCE_SCALE * ENGINE_OBJECT_DISTANCE_SCALE; }

static float read_float(uint32_t va) { return *(volatile float*)ADDR(va); }

static int write_float(uint32_t va, float value) {
    volatile float* p = (volatile float*)ADDR(va);
    DWORD old;
    if (!VirtualProtect((void*)p, sizeof *p, PAGE_READWRITE, &old)) return 0;
    *p = value;
    VirtualProtect((void*)p, sizeof *p, old, &old);
    return 1;
}

/* Only a value the game itself still holds (original or ours) is replaced. */
static void set_float(uint32_t va, float original, float modern, const char* what) {
    float now = read_float(va), want = s_modern ? modern : original;
    if (now == want) return;
    if (now != original && now != modern) {
        host_log("graphics", "%s at 0x%08X holds %.3f, not the game's %.3f: left as it is", what, va, now, original);
        return;
    }
    if (!write_float(va, want)) host_log("graphics", "%s at 0x%08X could not be written", what, va);
}

static void apply_group_objects(void) {
    if (s_group_objects_game) MEM32(MAX_OBJECTS_PER_CACHE) = (uint32_t)engine_cache_group_objects(s_modern, s_group_objects_game);
}

static void apply_data(void) {
    apply_group_objects();
    if (read_float(CACHE_MIN_DISTANCE) != ENGINE_CACHE_DISTANCE_ORIGINAL)
        write_float(CACHE_MIN_DISTANCE, ENGINE_CACHE_DISTANCE_ORIGINAL);
    /* 0 before the first level: SetGlobalPartitionData sets it through set_combined_culling. */
    if (read_float(CULL_COMBINED) != 0.0f) set_float(CULL_COMBINED, 1.0f, object_cull_combined(), "object cull factor");
}

/* The engine divides this budget with signed 32-bit products: a value it cannot divide is never handed to it. */
static void large_mem_alloc(void) {
    unsigned budget;
    engine_texture_layout_t layout;
    s_large_mem_lifted();
    if (g_alias_entry) return;
    budget = engine_texture_budget_mb(g_eax, s_modern, s_fixed_split);
    layout = engine_texture_layout(budget, 0xFFFFFFFFu, s_modern && s_fixed_split);
    if (layout.accepted && !layout.overflow) g_eax = budget;
}

/* The split computes the image cache as T * 40 / 100 in 32 bits, hands it to the render cache and subtracts it from
 * the regular textures: inside it the host's share goes to the cache and the difference to the regular pool. */
static void allocate_split(recomp_func_t lifted) {
    int32_t total;
    if (g_alias_entry || !s_modern || !s_fixed_split) {
        lifted();
        return;
    }
    total = (int32_t)MEM32(D3D_TEXTURE_TOTAL);
    s_split.engine_cache = engine_image_cache_bytes(total, 0);
    s_split.host_cache = engine_image_cache_bytes(total, 1);
    s_split.active = 1;
    lifted();
    s_split.active = 0;
}

static void allocate_non_local(void) { allocate_split(s_non_local_lifted); }

static void allocate_managed(void) { allocate_split(s_managed_lifted); }

static void cache_memory(void) {
    if (!g_alias_entry && s_split.active) GUEST_ARG(1) = (uint32_t)(s_split.host_cache / 1024);
    s_cache_memory_lifted();
}

/* AllocateMemory keeps 85% and 95% of the regular pool as its marks, products that wrap from 25 and 22 MB. */
static void agp_allocate(void) {
    uint32_t self = g_ecx;
    int32_t bytes, allocated, low, high;
    if (g_alias_entry || !s_modern || !s_fixed_split) {
        s_agp_allocate_lifted();
        return;
    }
    bytes = (int32_t)GUEST_ARG(0);
    if (s_split.active && bytes > 0x280000) {
        bytes = (int32_t)((int64_t)bytes + s_split.engine_cache - s_split.host_cache);
        if (bytes <= 0x280000) bytes = 0x280000;
        GUEST_ARG(0) = (uint32_t)bytes;
    }
    s_agp_allocate_lifted();
    if (bytes <= 0) return;
    engine_agp_marks(bytes, &allocated, &low, &high);
    MEM32(self + 0x8) = (uint32_t)allocated;
    MEM32(self + 0xC) = (uint32_t)low;
    MEM32(self + 0x10) = (uint32_t)high;
}

/* DEBUG_SAULO: whether the game kept Direct3D after dividing its texture memory. */
static void allocate_texture_memory(void) {
    int was_in_use = MEM8(D3D_IN_USE) != 0;
    s_allocate_lifted();
    if (g_alias_entry || !was_in_use) return;
    if (MEM8(D3D_IN_USE)) {
        if (s_accepted++ < 4)
            host_log("graphics", "guest hardware renderer accepted: %ld KB of texture memory, %ld KB for regular "
                     "textures (evicted above %ld KB)", (long)(int32_t)MEM32(D3D_TEXTURE_TOTAL) / 1024,
                     (long)(int32_t)MEM32(AGP_TEXTURE_MANAGER + 8) / 1024,
                     (long)(int32_t)MEM32(AGP_TEXTURE_MANAGER + 0x10) / 1024);
    } else if (s_rejected++ < 4) {
        host_log("graphics", "guest hardware renderer rejected: the game's texture pools failed (%ld KB of texture "
                 "memory, 85%% mark %ld KB); it turned Direct3D off and draws with its software renderer",
                 (long)(int32_t)MEM32(D3D_TEXTURE_TOTAL) / 1024, (long)(int32_t)MEM32(AGP_TEXTURE_MANAGER + 0xC) / 1024);
    }
}

void engine_limits_summary(unsigned long long hardware_draw_calls) {
    if (s_rejected)
        host_log("graphics", "guest hardware renderer accepted %u times, rejected %u times: the game fell back to "
                 "its software renderer", s_accepted, s_rejected);
    else if (s_accepted)
        host_log("graphics", "guest hardware renderer accepted %u times, never rejected: hardware path remained active "
                 "through gameplay (%llu hardware draw calls)", s_accepted, hardware_draw_calls);
    engine_limits_window(0);
    if (s_objects_total.meshes_in_range + s_objects_total.groups_in_range)
        host_log("objects", "session: considered meshes %llu, groups %llu, culled %llu; meshes 3D %llu, impostor %llu; "
                 "groups 3D %llu, impostor %llu; switched 3D to impostor %llu, impostor to 3D %llu",
                 s_objects_total.meshes_in_range, s_objects_total.groups_in_range, s_objects_total.beyond,
                 s_objects_total.meshes_3d, s_objects_total.meshes_impostor, s_objects_total.groups_3d,
                 s_objects_total.groups_impostor, s_objects_total.to_impostor, s_objects_total.to_3d);
}

/* The engine reads the recommended dimension in a few places that must keep the 128 its budgets were tuned for. */
static void with_original_dim(recomp_func_t lifted) {
    uint32_t dim = MEM32(RECOMMENDED_TEX_DIM);
    if (g_alias_entry || !s_modern || dim <= ENGINE_TEX_DIM_ORIGINAL) {
        lifted();
        return;
    }
    MEM32(RECOMMENDED_TEX_DIM) = ENGINE_TEX_DIM_ORIGINAL;
    lifted();
    MEM32(RECOMMENDED_TEX_DIM) = dim;
}

/* Its Direct3D branch keeps impostors at full scale only below 256 px. */
static void cache_parameters(void) {
    int alias = g_alias_entry != 0;
    with_original_dim(s_cache_parameters_lifted);
    if (alias) return;
    s_group_objects_game = (int)MEM32(MAX_OBJECTS_PER_CACHE);
    apply_group_objects();
}

/* CacheNever is the first thing bFlagCacheable reads; without it, the rest of its test decides. */
static void flag_cacheable(void) {
    uint32_t partition = g_ecx, flags;
    int lifted;
    if (g_alias_entry) {
        s_flag_cacheable_lifted();
        return;
    }
    flags = MEM32(partition + PARTITION_FLAGS);
    lifted = engine_cache_never_lifted(s_modern, MEM32(partition), flags);
    if (flags & PARTITION_CACHE_NEVER) s_cache_never_seen++;
    if (lifted) MEM32(partition + PARTITION_FLAGS) = flags & ~PARTITION_CACHE_NEVER;
    s_flag_cacheable_lifted();
    if (!lifted) return;
    MEM32(partition + PARTITION_FLAGS) |= PARTITION_CACHE_NEVER;
    if (g_eax & 0xFFu) s_cache_never_lifted++;
}

/* DEBUG_SAULO: how many impostor images the scheduler rebuilds and what they cost the guest thread. */
static void timed(recomp_func_t lifted, unsigned long long* calls, unsigned long long* ticks) {
    LARGE_INTEGER t0, t1;
    if (g_alias_entry) {
        lifted();
        return;
    }
    QueryPerformanceCounter(&t0);
    lifted();
    QueryPerformanceCounter(&t1);
    (*calls)++;
    *ticks += (unsigned long long)(t1.QuadPart - t0.QuadPart);
}

static void cache_update(void) { timed(s_cache_update_lifted, &s_objects.cache_updates, &s_objects.cache_update_ticks); }
static void cache_draw(void) { timed(s_cache_draw_lifted, &s_objects.cache_draws, &s_objects.cache_draw_ticks); }

/* With 128 px pages a node texture is at most 128 px and a larger base texture keeps only its top-left corner, drawn
 * over the whole map; 256 px is the most its packed pages can address. The pool grows to hold them. */
static void terrain_texture_mem(void) {
    uint32_t kb = GUEST_ARG(0);
    if (g_alias_entry || !s_modern || !kb) {
        with_original_dim(s_terrain_mem_lifted);
        return;
    }
    if (kb < ENGINE_TERRAIN_TEXTURE_KB_MODERN) GUEST_ARG(0) = ENGINE_TERRAIN_TEXTURE_KB_MODERN;
    s_terrain_mem_lifted();
}

/* SetGlobalPartitionData sets 1 for objects and shadows; the image cache doubles and restores whatever is there. */
static void set_combined_culling(void) {
    if (!g_alias_entry && g_cur_func == SET_GLOBAL_PARTITION_DATA_VA) {
        memset(s_software, 0, sizeof s_software);
        s_software_count = 0;
        if (s_modern) *(float*)ADDR(g_esp + 4) *= object_cull_combined();
    }
    s_combined_lifted();
}

static unsigned software_slot(uint32_t partition) {
    unsigned i = (partition * 2654435761u) >> 19;
    while (s_software[i] && s_software[i] != partition) i = (i + 1u) & (SOFTWARE_SLOTS - 1u);
    return i;
}

static int holds_software(uint32_t partition) { return s_software[software_slot(partition)] == partition; }

/* The mesh and every group above it: a group cached as one impostor keeps the CPU out of the frame. */
static float short_float(uint32_t va) {
    uint32_t bits = (uint32_t)MEM16(va) << 16;
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void mark_software(uint32_t partition) {
    for (unsigned depth = 0; partition && depth < 64 && s_software_count < SOFTWARE_SLOTS / 2; depth++) {
        unsigned slot = software_slot(partition);
        if (s_software[slot] == partition) return;
        s_software[slot] = partition;
        s_software_count++;
        partition = MEM32(partition + PARTITION_PARENT);
    }
}

static int software_mesh(uint32_t partition) {
    uint32_t flags = MEM32(partition + PARTITION_FLAGS);
    return (flags & PARTITION_USES_MESH) && !(flags & PARTITION_HARDWARE_ABLE);
}

static engine_partition_t partition_view(uint32_t partition) {
    engine_partition_t p;
    p.flags = MEM32(partition + PARTITION_FLAGS);
    p.no_cache_until = MEM32(partition + PARTITION_NO_CACHE_UNTIL);
    p.frame = MEM32(PARTITION_FRAME);
    p.sphere_sqr = short_float(partition + PARTITION_SPHERE_SQR);
    p.cylinder_sqr = short_float(partition + PARTITION_CYLINDER_SQR);
    p.area_efficiency_sqr = short_float(partition + PARTITION_AREA_EFFICIENCY_SQR);
    p.cache_mul = short_float(partition + PARTITION_CACHE_MUL);
    return p;
}

static uint32_t frame_key(void) { return MEM32(PARTITION_FRAME) + 1u; }

/* bInRange(distance squared): a static mesh too small to cover a pixel or two is left out as well. */
static void in_range(void) {
    int alias = g_alias_entry != 0;
    uint32_t partition = g_ecx, bits = alias ? 0 : GUEST_ARG(0), flags;
    float distance_squared;
    s_in_range_lifted();
    if (alias) return;
    memcpy(&distance_squared, &bits, sizeof distance_squared);
    flags = MEM32(partition + PARTITION_FLAGS);
    if (s_modern && (g_eax & 0xFFu) &&
        engine_projected_cull(engine_partition_kind(MEM32(partition)), (flags & PARTITION_USES_MESH) != 0,
                              sqrtf(short_float(partition + PARTITION_SPHERE_SQR)), distance_squared,
                              widescreen_pixels_per_metre())) {
        g_eax &= ~0xFFu;
        s_objects.projected_culls++;
    }
    if (!(g_eax & 0xFFu)) s_objects.beyond++;
    else if (MEM32(partition + PARTITION_FLAGS) & PARTITION_USES_MESH) s_objects.meshes_in_range++;
    else s_objects.groups_in_range++;
    if (s_modern && software_mesh(partition)) mark_software(partition);
}

static cache_refusal_t refusal(const engine_partition_t* p, float distance_squared) {
    engine_cache_settings_t c;
    c.hardware = MEM8(D3D_IN_USE) != 0;
    c.max_accept_cylinder = read_float(MAX_ACCEPT_CYLINDER);
    c.area_threshold_sqr = read_float(AREA_THRESHOLD_SQR);
    c.min_distance = read_float(CACHE_MIN_DISTANCE);
    return engine_cache_refusal(p, &c, distance_squared);
}

/* DEBUG_SAULO */
static void count_decision(uint32_t partition, const engine_partition_t* p, float distance_squared, int impostor,
                           int software, int had_cache) {
    int mesh = (p->flags & PARTITION_USES_MESH) != 0;
    uint32_t slot = (partition * 2654435761u) >> 20;
    if (impostor) {
        if (mesh) s_objects.meshes_impostor++;
        else s_objects.groups_impostor++;
        if (!had_cache) s_objects.new_caches++;
    } else {
        cache_refusal_t why = refusal(p, distance_squared);
        if (mesh) {
            s_objects.meshes_3d++;
            s_objects.mesh_refused[why]++;
            if (software) s_objects.software_3d++;
        } else {
            s_objects.groups_3d++;
            s_objects.group_refused[why]++;
        }
    }
    if (s_last_decision[slot].partition == partition && s_last_decision[slot].impostor != impostor) {
        if (impostor) s_objects.to_impostor++;
        else s_objects.to_3d++;
    }
    s_last_decision[slot].partition = partition;
    s_last_decision[slot].impostor = (uint8_t)impostor;
}

/* The partitions are visited from the root down, so an object is decided before its parts. */
static int object_kept_3d(uint32_t partition, uint32_t frame) {
    uint32_t parent = MEM32(partition + PARTITION_PARENT);
    for (unsigned depth = 0; parent && depth < OBJECT_DEPTH; depth++, parent = MEM32(parent + PARTITION_PARENT))
        if (engine_frame_set_has(s_kept_3d, OBJECT_SLOTS, parent, frame)) return 1;
    return 0;
}

/* The distance is read only here, so each call sees its own: by the object's projected size; 1998's for entities,
 * meshes the CPU draws and the groups holding them; never while the object it is part of stays 3D. */
static void should_cache(void) {
    uint32_t partition, bits, frame;
    engine_partition_t p;
    engine_part_kind_t kind;
    float distance_squared;
    int software = 0, had_cache, impostor, with_object = 0;
    if (g_alias_entry) {
        s_should_cache_lifted();
        return;
    }
    partition = GUEST_ARG(0);
    bits = GUEST_ARG(2);
    memcpy(&distance_squared, &bits, sizeof distance_squared);
    p = partition_view(partition);
    kind = engine_partition_kind(MEM32(partition));
    frame = frame_key();
    if (s_modern && s_cache_distance_writable) {
        float distance;
        software = holds_software(partition) || software_mesh(partition);
        with_object = object_kept_3d(partition, frame);
        distance = with_object ? ENGINE_CACHE_DISTANCE_NEVER
                               : engine_cache_min_distance(1, software, kind, sqrtf(p.sphere_sqr),
                                                           widescreen_pixels_per_metre(), p.cache_mul);
        *(float*)ADDR(CACHE_MIN_DISTANCE) = distance;
    }
    had_cache = MEM32(partition + PARTITION_RENDER_CACHE) != 0;
    s_should_cache_lifted();
    impostor = (g_eax & 0xFFu) != 0;
    if (!impostor && kind != ENGINE_PART_SPATIAL && engine_refusal_holds_parts(refusal(&p, distance_squared)))
        engine_frame_set_add(s_kept_3d, OBJECT_SLOTS, partition, frame);
    if (with_object && !impostor) s_objects.with_their_object++;
    count_decision(partition, &p, distance_squared, impostor, software, had_cache);
}

static void log_refusals(const char* what, const unsigned long long* refused, double f) {
    char line[640];
    int n = 0;
    for (int r = 0; r < CACHE_REFUSED_COUNT; r++)
        if (refused[r] && n < (int)sizeof line - 80)
            n += _snprintf(line + n, sizeof line - 1 - n, "%s%s %.1f", n ? ", " : "",
                           engine_cache_refusal_name((cache_refusal_t)r), refused[r] / f);
    line[sizeof line - 1] = 0;
    if (n) host_log("objects", "last 5 s per frame: %s kept 3D because: %s", what, line);
}

/* DEBUG_SAULO: called with the renderer's 5-second performance lines (frames 0: fold the window in only). */
void engine_limits_window(unsigned long long frames) {
    const unsigned long long* from = (const unsigned long long*)&s_objects;
    unsigned long long* to = (unsigned long long*)&s_objects_total;
    const object_stats_t* o = &s_objects;
    double f = frames ? (double)frames : 1.0;
    if (frames && o->meshes_in_range + o->groups_in_range + o->beyond) {
        host_log("objects", "last 5 s per frame: considered meshes %.0f, groups %.0f, culled beyond their distance %.0f; "
                 "meshes 3D %.1f (drawn by the CPU %.1f), impostor %.1f; groups 3D %.1f, impostor %.1f; new impostor "
                 "caches %.2f; switched 3D to impostor %.2f, impostor to 3D %.2f; partitions holding CPU-drawn meshes "
                 "%u; %.0f px per metre at 1 m", o->meshes_in_range / f, o->groups_in_range / f, o->beyond / f,
                 o->meshes_3d / f, o->software_3d / f, o->meshes_impostor / f, o->groups_3d / f,
                 o->groups_impostor / f, o->new_caches / f, o->to_impostor / f, o->to_3d / f, s_software_count,
                 widescreen_pixels_per_metre());
        host_log("objects", "last 5 s per frame: static meshes culled below %.1f px %.1f; partitions kept 3D with "
                 "the object they belong to %.1f; loaded so far: CacheNever partitions %lu, of them static instances "
                 "made cacheable %lu; groups of up to %u objects per impostor", ENGINE_CULL_PIXELS,
                 o->projected_culls / f, o->with_their_object / f, s_cache_never_seen, s_cache_never_lifted,
                 MEM32(MAX_OBJECTS_PER_CACHE));
        log_refusals("meshes", o->mesh_refused, f);
        log_refusals("groups", o->group_refused, f);
    }
    if (frames && (o->cache_updates || o->cache_draws)) {
        LARGE_INTEGER hz;
        QueryPerformanceFrequency(&hz);
        host_log("objects", "last 5 s per frame: impostor images rebuilt %.2f (%.2f ms), drawn into their pages %.2f "
                 "(%.2f ms, software renderer)", o->cache_updates / f, o->cache_update_ticks * 1000.0 / hz.QuadPart / f,
                 o->cache_draws / f, o->cache_draw_ticks * 1000.0 / hz.QuadPart / f);
    }
    for (size_t i = 0; i < sizeof s_objects / sizeof *from; i++) to[i] += from[i];
    memset(&s_objects, 0, sizeof s_objects);
}

void engine_limits_apply(int hardware) {
    s_modern = hardware && !s_keep_original && s_installed;
    apply_data();
}

void engine_limits_install(int hardware) {
    static const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
        const char* name;
    } k[] = {
        { LARGE_MEM_ALLOC_VA, large_mem_alloc, &s_large_mem_lifted, "texture memory" },
        { CACHE_PARAMETERS_VA, cache_parameters, &s_cache_parameters_lifted, "impostor scale" },
        { TERRAIN_TEXTURE_MEM_VA, terrain_texture_mem, &s_terrain_mem_lifted, "terrain texture pages" },
        { SET_COMBINED_CULLING_VA, set_combined_culling, &s_combined_lifted, "object cull factor" },
    }, optional[] = {
        { FLAG_CACHEABLE_VA, flag_cacheable, &s_flag_cacheable_lifted, "image cache eligibility" },
    }, split[] = {
        { ALLOCATE_NON_LOCAL_VA, allocate_non_local, &s_non_local_lifted, "texture split" },
        { ALLOCATE_MANAGED_VA, allocate_managed, &s_managed_lifted, "managed texture split" },
        { CACHE_MEMORY_VA, cache_memory, &s_cache_memory_lifted, "image cache memory" },
        { AGP_ALLOCATE_VA, agp_allocate, &s_agp_allocate_lifted, "texture manager marks" },
    }, counters[] = {
        { ALLOCATE_TEXTURE_MEM_VA, allocate_texture_memory, &s_allocate_lifted, "texture memory allocation" },
        { IN_RANGE_VA, in_range, &s_in_range_lifted, "cull distance test" },
        { SHOULD_CACHE_VA, should_cache, &s_should_cache_lifted, "image cache test" },
        { CACHE_UPDATE_VA, cache_update, &s_cache_update_lifted, "impostor rebuild" },
        { CACHE_DRAW_VA, cache_draw, &s_cache_draw_lifted, "impostor drawing" },
    };
    DWORD old;
    for (size_t i = 0; i < sizeof counters / sizeof counters[0]; i++)
        if (!guest_override_redirect(counters[i].va, counters[i].replacement, counters[i].lifted))
            host_log("graphics", "sub_%08X (%s) has no incremental-link thunk: it is not counted", counters[i].va,
                     counters[i].name);
        else if (counters[i].va == SHOULD_CACHE_VA)
            s_cache_distance_writable = VirtualProtect((void*)ADDR(CACHE_MIN_DISTANCE), 4, PAGE_READWRITE, &old) != 0;
    s_installed = !s_keep_original;
    for (size_t i = 0; s_installed && i < sizeof k / sizeof k[0]; i++) {
        if (guest_override_redirect(k[i].va, k[i].replacement, k[i].lifted)) continue;
        host_log("graphics", "sub_%08X (%s) has no incremental-link thunk: the original engine limits stay", k[i].va,
                 k[i].name);
        s_installed = 0;
    }
    for (size_t i = 0; s_installed && i < sizeof optional / sizeof optional[0]; i++)
        if (!guest_override_redirect(optional[i].va, optional[i].replacement, optional[i].lifted))
            host_log("graphics", "sub_%08X (%s) has no incremental-link thunk: it keeps the 1998 rule", optional[i].va,
                     optional[i].name);
    s_fixed_split = s_installed;
    for (size_t i = 0; s_fixed_split && i < sizeof split / sizeof split[0]; i++) {
        if (guest_override_redirect(split[i].va, split[i].replacement, split[i].lifted)) continue;
        host_log("graphics", "sub_%08X (%s) has no incremental-link thunk: texture memory stays at %u MB", split[i].va,
                 split[i].name, ENGINE_TEXTURE_MB_SAFE);
        s_fixed_split = 0;
    }
    engine_limits_apply(hardware);
}

void engine_limits_log(int hardware, const char* filter, unsigned msaa, int vsync) {
    if (!hardware) {
        host_log("graphics", "Graphics improvements: software renderer, the original engine limits; VSync %s",
                 vsync ? "on" : "off");
        return;
    }
    host_log("graphics", "Graphics improvements (D3D11):");
    if (s_modern) {
        unsigned mb = s_fixed_split ? ENGINE_TEXTURE_MB_MODERN : ENGINE_TEXTURE_MB_SAFE;
        engine_texture_layout_t layout = engine_texture_layout(mb, 0xFFFFFFFFu, s_fixed_split);
        host_log("graphics", "  draw distance: every object stays drawn %.1fx as far (its level's Culling included), up "
                 "to the far clip; an object stays 3D while its bounding sphere spans more than %.0f px (%.0f-%.0f m, "
                 "times its CacheMul, never nearer than 1998), then the image cache may draw it as an impostor; at "
                 "1440 lines radius 2/4/6/10/16 m: %.0f/%.0f/%.0f/%.0f/%.0f m; an object's parts switch with it; "
                 "entities and meshes drawn by the CPU %.0f m as in 1998; static meshes under %.1f px are not drawn",
                 ENGINE_OBJECT_DISTANCE_SCALE, ENGINE_IMPOSTOR_SWITCH_PX, ENGINE_FULL_3D_MIN, ENGINE_FULL_3D_MAX,
                 engine_full_3d_distance(2.0f, 0.0f, 1.0f), engine_full_3d_distance(4.0f, 0.0f, 1.0f),
                 engine_full_3d_distance(6.0f, 0.0f, 1.0f), engine_full_3d_distance(10.0f, 0.0f, 1.0f),
                 engine_full_3d_distance(16.0f, 0.0f, 1.0f), ENGINE_CACHE_DISTANCE_ORIGINAL, ENGINE_CULL_PIXELS);
        host_log("graphics", "  LOD: the game's own, measured at the native render size (finer than 640x480), quality 4 "
                 "detail scale kept");
        host_log("graphics", "  image cache: a static instance the level marks CacheNever is cached like its "
                 "neighbours when nothing in it prevents it (animated or alpha-colour mesh, backdrop, water); up to %d "
                 "objects share a group impostor (1998: 10-20 by quality)", ENGINE_CACHE_GROUP_OBJECTS_MODERN);
        host_log("graphics", "  textures: up to %u px (was %u), texture memory %u MB (was %u MB, split in 64-bit "
                 "arithmetic%s): regular textures %ld KB (evicted above %ld KB), impostors up to %u px in %ld KB; "
                 "terrain pages at %u px in %u KB", ENGINE_TEX_DIM_MODERN, ENGINE_TEX_DIM_ORIGINAL, mb,
                 ENGINE_TEXTURE_MB_ORIGINAL, s_fixed_split ? "" : " unavailable", (long)layout.regular / 1024,
                 (long)layout.high_water_95 / 1024, ENGINE_TEX_DIM_MODERN, (long)layout.image_cache / 1024,
                 ENGINE_TEX_DIM_MODERN, ENGINE_TERRAIN_TEXTURE_KB_MODERN);
        texture_streaming_log();
        terrain_quality_log();
        engine_heaps_log();
    } else {
        host_log("graphics", "  draw distance, LOD, texture sizes and texture memory: the original engine limits%s",
                 s_keep_original ? " (--original-engine-limits)" : "");
    }
    host_log("graphics", "  texture filtering %s; mipmaps %s; MSAA %ux requested; VSync %s", filter,
             strcmp(filter, "original") ? "generated for the 3D world's textures" : "the game's own",
             msaa, vsync ? "on" : "off");
}

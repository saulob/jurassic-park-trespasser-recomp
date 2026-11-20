/*
 * The 1998 engine limits that Direct3D 11 lifts: how far objects stay drawn and stay 3D, the texture size Direct3D may
 * use (world textures and impostors) and the texture memory the game divides among them. Only guest data and
 * overridden functions change; the software renderer keeps the original values.
 */
#include "guest_va.h"
#include "host.h"
#include "host_policy.h"

#include <math.h>
#include <string.h>

#define CACHE_MIN_DISTANCE GVA(0x0064E458)      /* bShouldCache's fMinDistanceToCache (.rdata) */
#define CULL_COMBINED GVA(0x006634EC)           /* fCullDistanceCombined: multiplies every squared cull distance */
#define RECOMMENDED_TEX_DIM GVA(0x006D0F1C)     /* d3dDriver.iRecommendedMaxDim, from "Max Recommended Tex Dim" */
#define LARGE_MEM_ALLOC_VA GVA(0x005072D0)      /* CDirect3D::iLargeMemAllocMB */
#define CACHE_PARAMETERS_VA GVA(0x005477E0)     /* CRenderCache::SetParameters */
#define TERRAIN_TEXTURE_MEM_VA GVA(0x00492200)  /* CTextureNode::SetTextureMemSize(KB) */
#define TERRAIN_PAGE_SIZE GVA(0x0066424C)       /* iMaxTexturePageSize, set by SetTextureMemSize */
#define TERRAIN_TEXTURE_MEM_BYTES 0xD0u
#define FLOAT_ONE_BITS 0x3F800000u
#define ALLOCATE_TEXTURE_MEM_VA GVA(0x00506270) /* CDirect3D::AllocateTextureMemory */
#define ALLOCATE_NON_LOCAL_VA GVA(0x00506EB0)   /* CDirect3D::bAllocateNonLocalMemory: the split */
#define ALLOCATE_MANAGED_VA GVA(0x00506C00)     /* the same split for managed textures */
#define CACHE_MEMORY_VA GVA(0x00547710)         /* CRenderCache::SetTextureMemSize(system KB, video KB), cdecl */
#define AGP_ALLOCATE_VA GVA(0x00514FE0)         /* CAGPTextureMemManager::AllocateMemory(bytes) */
#define SET_COMBINED_CULLING_VA GVA(0x00477E00) /* (objects, shadows), cdecl */
#define SET_GLOBAL_PARTITION_DATA_VA GVA(0x004A24B0)
#define IN_RANGE_VA GVA(0x00477A40)             /* CPartition::bInRange(distance squared) */
#define SHOULD_CACHE_VA GVA(0x00543830)         /* bShouldCache(partition, camera, distance squared, ...), cdecl */
#define FLAG_CACHEABLE_VA GVA(0x00478A30)       /* CPartition::bFlagCacheable, thiscall */
#define MAX_OBJECTS_PER_CACHE GVA(0x007371A0)   /* rcsRenderCacheSettings.iMaxObjectsPerCache */
#define D3D_DRIVER GVA(0x006D0E98)
#define D3D_IN_USE (D3D_DRIVER + 0x1u)          /* CDirect3D::bUse */
#define D3D_TEXTURE_TOTAL (D3D_DRIVER + 0x604u) /* bytes split among the texture pools */
#define AGP_TEXTURE_MANAGER GVA(0x006D2040)         /* +8 allocated, +0xC 85% mark, +0x10 95% mark */
#define SOFTWARE_SLOTS 8192u
/* CPartition: CShortFloats (a float's high 16 bits), SPartitionData, cache state. */
#define PARTITION_CACHE_MUL 0x4u
#define PARTITION_CYLINDER_SQR 0xAu
#define PARTITION_SPHERE_SQR 0xCu
#define PARTITION_AREA_EFFICIENCY_SQR 0xEu
#define PARTITION_FLAGS 0x10u
#define PARTITION_NO_CACHE_UNTIL 0x14u
#define PARTITION_PARENT 0x24u
#define PARTITION_FRAME GVA(0x006648F4)     /* SPartitionSettings::u4FrameNumber */
#define MAX_ACCEPT_CYLINDER GVA(0x00737190) /* rcsRenderCacheSettings.fMaxAcceptCylinder */
#define AREA_THRESHOLD_SQR GVA(0x0066F510)  /* CPartition::fAreaEfficiencyThresholdSqr */

static recomp_func_t s_large_mem_lifted, s_cache_parameters_lifted, s_terrain_mem_lifted, s_allocate_lifted;
static recomp_func_t s_non_local_lifted, s_managed_lifted, s_cache_memory_lifted, s_agp_allocate_lifted;
static recomp_func_t s_combined_lifted, s_in_range_lifted, s_should_cache_lifted, s_flag_cacheable_lifted;
static int s_group_objects_game;
static int s_keep_original, s_modern, s_installed, s_fixed_split, s_cache_distance_writable;
static unsigned s_accepted, s_rejected;
static struct {
    int active;
    int32_t engine_cache, host_cache;
} s_split;

/* Objects (instances, not spatial groups) that stayed 3D this frame: their parts stay 3D with them. */
#define OBJECT_SLOTS 4096u
static engine_frame_slot_t s_kept_3d[OBJECT_SLOTS];
#define IMPOSTOR_STATE_SLOTS 16384u
#define REBUILD_FRAMES 120u
static engine_impostor_state_t s_impostors[IMPOSTOR_STATE_SLOTS], s_hidden[IMPOSTOR_STATE_SLOTS];
static struct { /* What makes a drawn object appear, vanish or change */
    unsigned long long requests, impostor_answers, became, dropped, rebuilt, group_became, group_dropped;
    unsigned long long distance_culled, distance_restored, subpixel_rejects, subpixel_hidden;
    uint32_t frame;
    unsigned in_frame, peak;
} s_cache_stats;
#define OBJECT_DEPTH 16
/* Partitions holding a mesh the CPU draws into the Direct3D frame (a bump map makes it so). */
static uint32_t s_software[SOFTWARE_SLOTS];
static unsigned s_software_count;

void engine_limits_keep_original(void) { s_keep_original = 1; }

int engine_limits_original(void) { return s_keep_original; }

int engine_limits_modern(void) { return s_modern; }

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
    if (read_float(CULL_COMBINED) != 0.0f)
        set_float(CULL_COMBINED, engine_object_cull_combined(0), engine_object_cull_combined(1), "object cull factor");
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

/* Whether the game kept Direct3D after dividing its texture memory. */
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
        host_log_perf("graphics", "guest hardware renderer accepted %u times, never rejected: hardware path remained active "
                 "through gameplay (%llu hardware draw calls)", s_accepted, hardware_draw_calls);
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
    if (lifted) MEM32(partition + PARTITION_FLAGS) = flags & ~PARTITION_CACHE_NEVER;
    s_flag_cacheable_lifted();
    if (lifted) MEM32(partition + PARTITION_FLAGS) |= PARTITION_CACHE_NEVER;
}

/* Reallocated right after SetTextureMemSize, which has just purged every node texture and emptied this pool. */
static void enlarge_terrain_shadow_pool(void) {
    uint32_t page = MEM32(TERRAIN_PAGE_SIZE), kb = engine_terrain_shadow_kb(page), pool_va, alloc_va, pool, eax = g_eax;
    uint32_t args[5];
    if (kb <= ENGINE_TERRAIN_SHADOW_KB_ORIGINAL) return;
    if (!terrain_shadow_pool_call((const uint8_t*)ADDR(TERRAIN_TEXTURE_MEM_VA), TERRAIN_TEXTURE_MEM_VA,
                                  TERRAIN_TEXTURE_MEM_BYTES, &pool_va, &alloc_va) ||
        !(pool = MEM32(pool_va))) {
        host_log("graphics", "terrain moving-shadow pool left at %u KB: SetTextureMemSize differs from 1.0.116",
                 ENGINE_TERRAIN_SHADOW_KB_ORIGINAL);
        return;
    }
    args[0] = kb * 1024u;
    args[1] = 0;
    args[2] = FLOAT_ONE_BITS;
    args[3] = page;
    args[4] = 0;
    if (guest_call(alloc_va, pool, args, 5, 0, NULL))
        host_log("graphics", "terrain moving-shadow pool %u KB (%u pages of %u px; the game's %u KB held %u)", kb,
                 ENGINE_TERRAIN_SHADOW_PAGES, page, ENGINE_TERRAIN_SHADOW_KB_ORIGINAL,
                 ENGINE_TERRAIN_SHADOW_KB_ORIGINAL * 1024u / (page * page * 2u));
    g_eax = eax;
}

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
    enlarge_terrain_shadow_pool();
}

/* SetGlobalPartitionData sets 1 for objects and shadows; the image cache doubles and restores whatever is there. */
static void set_combined_culling(void) {
    if (!g_alias_entry && g_cur_func == SET_GLOBAL_PARTITION_DATA_VA) {
        memset(s_software, 0, sizeof s_software);
        s_software_count = 0;
        if (s_modern) *(float*)ADDR(g_esp + 4) *= engine_object_cull_combined(1);
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
static void note_visibility(uint32_t partition, int drawn, int subpixel) {
    uint32_t frame = frame_key(), since;
    int known = engine_impostor_known(s_hidden, IMPOSTOR_STATE_SLOTS, partition, frame);
    engine_impostor_change_t change = engine_impostor_note(s_hidden, IMPOSTOR_STATE_SLOTS, partition, frame, !drawn, &since);
    if (subpixel) s_cache_stats.subpixel_rejects++;
    if (!known) return;
    if (change == ENGINE_IMPOSTOR_BECAME) {
        if (subpixel)
            s_cache_stats.subpixel_hidden++;
        else
            s_cache_stats.distance_culled++;
    } else if (change == ENGINE_IMPOSTOR_DROPPED) {
        s_cache_stats.distance_restored++;
    }
}

static void in_range(void) {
    int alias = g_alias_entry != 0, in_view, subpixel = 0;
    uint32_t partition = g_ecx, bits = alias ? 0 : GUEST_ARG(0), flags;
    float distance_squared;
    s_in_range_lifted();
    if (alias) return;
    memcpy(&distance_squared, &bits, sizeof distance_squared);
    flags = MEM32(partition + PARTITION_FLAGS);
    in_view = (g_eax & 0xFFu) != 0;
    if (s_modern && in_view &&
        engine_projected_cull(engine_partition_kind(MEM32(partition)), (flags & PARTITION_USES_MESH) != 0,
                              sqrtf(short_float(partition + PARTITION_SPHERE_SQR)), distance_squared,
                              widescreen_pixels_per_metre())) {
        g_eax &= ~0xFFu;
        subpixel = 1;
    }
    if (s_modern && software_mesh(partition)) mark_software(partition);
    if (s_modern && (flags & PARTITION_USES_MESH)) note_visibility(partition, in_view && !subpixel, subpixel);
}

static cache_refusal_t refusal(const engine_partition_t* p, float distance_squared) {
    engine_cache_settings_t c;
    c.hardware = MEM8(D3D_IN_USE) != 0;
    c.max_accept_cylinder = read_float(MAX_ACCEPT_CYLINDER);
    c.area_threshold_sqr = read_float(AREA_THRESHOLD_SQR);
    c.min_distance = read_float(CACHE_MIN_DISTANCE);
    return engine_cache_refusal(p, &c, distance_squared);
}

/* The partitions are visited from the root down, so an object is decided before its parts. */
static int object_kept_3d(uint32_t partition, uint32_t frame) {
    uint32_t parent = MEM32(partition + PARTITION_PARENT);
    for (unsigned depth = 0; parent && depth < OBJECT_DEPTH; depth++, parent = MEM32(parent + PARTITION_PARENT))
        if (engine_frame_set_has(s_kept_3d, OBJECT_SLOTS, parent, frame)) return 1;
    return 0;
}

static void note_cache_answer(uint32_t partition, uint32_t frame, int impostor, int group) {
    uint32_t since;
    engine_impostor_change_t change = engine_impostor_note(s_impostors, IMPOSTOR_STATE_SLOTS, partition, frame, impostor, &since);
    s_cache_stats.requests++;
    if (s_cache_stats.frame != frame) {
        s_cache_stats.frame = frame;
        s_cache_stats.in_frame = 0;
    }
    if (!impostor) {
        if (change == ENGINE_IMPOSTOR_DROPPED) {
            s_cache_stats.dropped++;
            s_cache_stats.group_dropped += (unsigned long long)group;
        }
        return;
    }
    s_cache_stats.impostor_answers++;
    if (++s_cache_stats.in_frame > s_cache_stats.peak) s_cache_stats.peak = s_cache_stats.in_frame;
    if (change != ENGINE_IMPOSTOR_BECAME) return;
    s_cache_stats.became++;
    s_cache_stats.group_became += (unsigned long long)group;
    if (since && since <= REBUILD_FRAMES) s_cache_stats.rebuilt++;
}

/* The distance is read only here, so each call sees its own: by the object's projected size; 1998's for entities,
 * meshes the CPU draws and the groups holding them; never while the object it is part of stays 3D. */
static void should_cache(void) {
    uint32_t partition, bits, frame;
    engine_partition_t p;
    engine_part_kind_t kind;
    float distance_squared;
    int held;
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
    held = engine_impostor_held(s_impostors, IMPOSTOR_STATE_SLOTS, partition, frame);
    if (s_modern && s_cache_distance_writable) {
        int software = holds_software(partition) || software_mesh(partition);
        float distance = object_kept_3d(partition, frame)
                             ? ENGINE_CACHE_DISTANCE_NEVER
                             : engine_cache_min_distance_held(1, software, kind, sqrtf(p.sphere_sqr),
                                                              widescreen_pixels_per_metre(), p.cache_mul, held);
        *(float*)ADDR(CACHE_MIN_DISTANCE) = distance;
    }
    s_should_cache_lifted();
    note_cache_answer(partition, frame, (g_eax & 0xFFu) != 0, kind == ENGINE_PART_SPATIAL);
    if (!(g_eax & 0xFFu) && kind != ENGINE_PART_SPATIAL && engine_refusal_holds_parts(refusal(&p, distance_squared)))
        engine_frame_set_add(s_kept_3d, OBJECT_SLOTS, partition, frame);
}

void engine_limits_cache_summary(void) {
    if (!s_cache_stats.requests) return;
    host_log("perf-detail", "image cache: %llu bShouldCache answers, %llu impostor | 3D -> impostor %llu, "
             "impostor -> 3D %llu (switch %.0f px, back to 3D %.0f px), impostors rebuilt within %u frames of being "
             "dropped %llu | peak impostors in a frame %u | an impostor is built when it is chosen, never shown unready",
             s_cache_stats.requests, s_cache_stats.impostor_answers, s_cache_stats.became, s_cache_stats.dropped,
             ENGINE_IMPOSTOR_SWITCH_PX, ENGINE_IMPOSTOR_RETURN_PX, REBUILD_FRAMES, s_cache_stats.rebuilt,
             s_cache_stats.peak);
    host_log("perf-detail", "pop-in sources (meshes seen in consecutive frames): 3D -> impostor %llu (of "
             "them group impostors %llu), impostor -> 3D %llu (groups %llu) | culled by distance %llu, back within "
             "distance %llu | hidden under %.1f px %llu (%llu sub-pixel answers) | LOD transitions: not visible to the "
             "host (the mesh LOD choice has no override)", s_cache_stats.became, s_cache_stats.group_became,
             s_cache_stats.dropped, s_cache_stats.group_dropped, s_cache_stats.distance_culled,
             s_cache_stats.distance_restored, ENGINE_CULL_PIXELS, s_cache_stats.subpixel_hidden,
             s_cache_stats.subpixel_rejects);
}

static void engine_limits_apply(int hardware) {
    s_modern = hardware && !s_keep_original && s_installed;
    apply_data();
}

void engine_limits_install(int hardware) {
    const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
    } k[] = {
        { LARGE_MEM_ALLOC_VA, large_mem_alloc, &s_large_mem_lifted },
        { CACHE_PARAMETERS_VA, cache_parameters, &s_cache_parameters_lifted },
        { TERRAIN_TEXTURE_MEM_VA, terrain_texture_mem, &s_terrain_mem_lifted },
        { SET_COMBINED_CULLING_VA, set_combined_culling, &s_combined_lifted },
    }, cache_never[] = {
        { FLAG_CACHEABLE_VA, flag_cacheable, &s_flag_cacheable_lifted },
    }, split[] = {
        { ALLOCATE_NON_LOCAL_VA, allocate_non_local, &s_non_local_lifted },
        { ALLOCATE_MANAGED_VA, allocate_managed, &s_managed_lifted },
        { CACHE_MEMORY_VA, cache_memory, &s_cache_memory_lifted },
        { AGP_ALLOCATE_VA, agp_allocate, &s_agp_allocate_lifted },
    }, policy[] = {
        { ALLOCATE_TEXTURE_MEM_VA, allocate_texture_memory, &s_allocate_lifted },
        { IN_RANGE_VA, in_range, &s_in_range_lifted },
        { SHOULD_CACHE_VA, should_cache, &s_should_cache_lifted },
    };
    DWORD old;
    for (size_t i = 0; i < sizeof policy / sizeof policy[0]; i++)
        if (guest_override_install(policy[i].va, policy[i].replacement, policy[i].lifted) &&
            policy[i].va == SHOULD_CACHE_VA)
            s_cache_distance_writable = VirtualProtect((void*)ADDR(CACHE_MIN_DISTANCE), 4, PAGE_READWRITE, &old) != 0;
    s_installed = !s_keep_original;
    for (size_t i = 0; s_installed && i < sizeof k / sizeof k[0]; i++)
        s_installed = guest_override_install(k[i].va, k[i].replacement, k[i].lifted);
    for (size_t i = 0; s_installed && i < sizeof cache_never / sizeof cache_never[0]; i++)
        guest_override_install(cache_never[i].va, cache_never[i].replacement, cache_never[i].lifted);
    s_fixed_split = s_installed;
    for (size_t i = 0; s_fixed_split && i < sizeof split / sizeof split[0]; i++)
        s_fixed_split = guest_override_install(split[i].va, split[i].replacement, split[i].lifted);
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
                 "to the level's far clip, which the host reaches out to %.0f m at most (terrain textures); an object "
                 "stays 3D while its bounding sphere spans more than %.0f px (%.0f-%.0f m, times its CacheMul, never "
                 "nearer than 1998), then the image cache may draw it as an impostor; at 1440 lines "
                 "radius 2/4/6/10/16 m: %.0f/%.0f/%.0f/%.0f/%.0f m; an object's parts switch with it; entities and "
                 "meshes drawn by the CPU %.0f m as in 1998; static meshes under %.1f px are not drawn",
                 ENGINE_OBJECT_DISTANCE_SCALE, ENGINE_FAR_CLIP_MAX, ENGINE_IMPOSTOR_SWITCH_PX, ENGINE_FULL_3D_MIN,
                 ENGINE_FULL_3D_MAX,
                 engine_full_3d_distance(2.0f, 0.0f, 1.0f), engine_full_3d_distance(4.0f, 0.0f, 1.0f),
                 engine_full_3d_distance(6.0f, 0.0f, 1.0f), engine_full_3d_distance(10.0f, 0.0f, 1.0f),
                 engine_full_3d_distance(16.0f, 0.0f, 1.0f), ENGINE_CACHE_DISTANCE_ORIGINAL, ENGINE_CULL_PIXELS);
        host_log("graphics", "  LOD: the game's own, measured at the native render size (finer than 640x480), quality 4 "
                 "detail scale kept");
        host_log("graphics", "  image cache: a static instance the level marks CacheNever is cached like its "
                 "neighbours when nothing in it prevents it (animated or alpha-colour mesh, backdrop, water); up to %d "
                 "objects share a group impostor (1998: 10-20 by quality)", ENGINE_CACHE_GROUP_OBJECTS_MODERN);
        int32_t regular = engine_regular_texture_bytes(&layout, 1);
        host_log("graphics", "  textures: up to %u px (was %u), texture memory %u MB (was %u MB, split in 64-bit "
                 "arithmetic%s): regular textures %ld KB (evicted above %ld KB), impostors up to %u px in %ld KB, "
                 "terrain pages at %u px in %u KB, water %ld KB; terrain moving shadows %u KB besides",
                 ENGINE_TEX_DIM_MODERN, ENGINE_TEX_DIM_ORIGINAL, mb, ENGINE_TEXTURE_MB_ORIGINAL,
                 s_fixed_split ? "" : " unavailable", (long)regular / 1024, (long)((int64_t)regular * 95 / 100) / 1024,
                 ENGINE_TEX_DIM_MODERN, (long)layout.image_cache / 1024, ENGINE_TEX_DIM_MODERN,
                 ENGINE_TERRAIN_TEXTURE_KB_MODERN, (long)layout.water / 1024,
                 engine_terrain_shadow_kb(ENGINE_TEX_DIM_MODERN));
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

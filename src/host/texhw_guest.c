#include "texhw_guest.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "guest_abi.h"
#include "jpt_lit.h"
#include "jpt_texhw.h"

#define POLYGON_COUNT 0x00u
#define POLYGON_VERTICES 0x04u
#define POLYGON_FEATURES 0x08u
#define POLYGON_TEXTURE 0x0Cu
#define POLYGON_VALUE 0x10u
#define POLYGON_MIP 0x30u
#define POLYGON_DRAW 0x74u
#define TEXTURE_MIPS 0x08u
#define TEXTURE_MIP_COUNT 0x20u
#define TEXTURE_COLORS 0x2Cu
#define TEXTURE_HASH 0x38u
#define TEXTURE_MIPS_MOST 6u
#define RASTER_WIDTH 0x08u
#define RASTER_HEIGHT 0x0Cu
#define RASTER_LINKED 0x48u
#define RASTER_D3D_TEXTURE 0x5Cu
#define NULL_RASTER GVA(0x00669B90)
#define DRAW_FIRST 6u
#define DRAW_LAST 8u
#define SLOTS 4096u
#define WORD ((uint32_t)sizeof(uint32_t))
#define FEATURE_SHADE 0x00000002u
#define FEATURE_LIGHT 0x00020000u
#define FEATURE_TERRAIN 0x00800000u
#define VERTEX_VALUE 0x18u
#define VERTEX_U 0x1Cu
#define COLORS_TABLE 0x08u /* its material first */
#define TABLE_DARK 0x04u
#define TABLE_FULL 0x08u
#define TABLE_SHADED 0x2Cu
#define TABLE_LEAST 0x30u
#define MATERIAL_SPECULAR 0x04u
#define MATERIAL_HIGHLIGHT 0x08u
#define MATERIAL_FLAGS 0x0Cu
#define MATERIAL_EMISSIVE 0x14u
#define MATERIAL_REFRACTS 0xFFFF0000u
#define VIEW_CORNERS GVA(0x007328E0) /* directions: top left, top right, bottom left, bottom right */
#define CORNER_BYTES 12u
#define GAMMA GVA(0x006D1680)
#define GAMMA_EXPONENT 0x808u
#define GAMMA_SCALE 0x80Cu
#define GAMMA_HALF (0.999f * 0.5f)
#define ACTIVE_LIGHTS GVA(0x00740718)
#define NODE_VALUE 0x08u
#define LIGHTS_WALKED_MAX 256u
#define INSTANCE_RENDER 0x28u
#define RENDER_LIGHT 0x08u
#define LIGHT_PRESENCE 0x08u
#define LIGHT_INTENSITY 0x10u
#define PRESENCE_TURN 0x0Cu
#define VT_DIRECTIONAL GVA(0x0064EBA8)
#define VT_AMBIENT GVA(0x0064EB60)
#define LIGHT_LENGTH 0.999f
#define CORNERS_MAX 64u
#define LIT_POLYGONS 8192u /* a power of two, and the next too */
#define LIT_POINTS 32768u
#define LIT_VERTICES 49152u

/* The engine has one texture per key. Textures it makes elsewhere leave the field of the key as the heap had it: one of those is told
 * by the key's own texture still being there. */
static struct {
    uint32_t hash, texture;
} s_owner[SLOTS];
static unsigned s_generation;
static int s_padded_logged;

static int owns(uint32_t hash, uint32_t texture) {
    unsigned i = (hash * 2654435761u >> 20) & (SLOTS - 1u);
    uint32_t current;
    for (unsigned n = 0; n < SLOTS; n++, i = (i + 1u) & (SLOTS - 1u)) {
        if (s_owner[i].texture && s_owner[i].hash != hash) continue;
        if (s_owner[i].texture == texture) return 1;
        if (s_owner[i].texture && guest_read32(s_owner[i].texture + TEXTURE_HASH, &current) && current == hash) return 0;
        s_owner[i].hash = hash;
        s_owner[i].texture = texture;
        return 1;
    }
    return 0;
}

/* ---- normal maps ---- */

typedef struct {
    uint32_t polygon;
    unsigned first;
    jpt_lit_draw_t draw;
    jpt_lit_material_t material;
    jpt_lit_shade_t shade;
    jpt_lit_frame_t face;
    float middle[3];
    int shaded;
} lit_polygon_t;

typedef struct {
    uint32_t key[4];
    float normal[3];
    int used;
} lit_point_t;

static struct {
    lit_polygon_t* polygons;
    lit_point_t* points;
    jpt_lit_vertex_t* vertices;
    unsigned count, vertex_count, point_count;
    float forward[3], right[3], up[3];
    jpt_lit_bump_t bump;
    int usable, logged;
} L;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }

static int lit_memory(void) {
    if (!L.polygons) L.polygons = (lit_polygon_t*)calloc(LIT_POLYGONS, sizeof *L.polygons);
    if (!L.points) L.points = (lit_point_t*)calloc(LIT_POINTS, sizeof *L.points);
    if (!L.vertices) L.vertices = (jpt_lit_vertex_t*)calloc(LIT_VERTICES, sizeof *L.vertices);
    return L.polygons && L.points && L.vertices;
}

static int read_lights(jpt_lit_lights_t* out) {
    const uint32_t head = MEM32(ACTIVE_LIGHTS);
    uint32_t node, instance, render, light, presence, vtable;
    memset(out, 0, sizeof *out);
    if (!head || !guest_read32(head, &node)) return 0;
    for (unsigned n = 0; node != head && n < LIGHTS_WALKED_MAX; n++) {
        if (!node || !guest_readable(node, NODE_VALUE + WORD) || !(instance = MEM32(node + NODE_VALUE)) || !guest_read32(instance + INSTANCE_RENDER, &render) || !render) return 0;
        if (!guest_read32(render + RENDER_LIGHT, &light) || !light || !guest_readable(light, LIGHT_INTENSITY + WORD)) return 0;
        vtable = MEM32(light);
        if (vtable == VT_AMBIENT) out->ambient += rdf(light + LIGHT_INTENSITY) * (1.0f - out->ambient);
        presence = MEM32(light + LIGHT_PRESENCE);
        if (vtable == VT_DIRECTIONAL && out->count < JPT_LIT_LIGHTS_MAX && presence && guest_readable(presence, PRESENCE_TURN + (uint32_t)sizeof(float[4]))) {
            const float* turn = (const float*)ADDR(presence + PRESENCE_TURN);
            const float c = turn[0], x = turn[1], y = turn[2], z = turn[3];
            float* towards = out->light[out->count].towards;
            towards[0] = -2.0f * (x * z + c * y) * LIGHT_LENGTH;
            towards[1] = -2.0f * (y * z - c * x) * LIGHT_LENGTH;
            towards[2] = -2.0f * (c * c - 0.5f + z * z) * LIGHT_LENGTH;
            out->light[out->count++].strength = rdf(light + LIGHT_INTENSITY);
        }
        node = MEM32(node);
    }
    return node == head;
}

static int read_view(void) {
    float corner[4][3], length[3] = { 0 };
    for (uint32_t c = 0; c < 4; c++)
        for (uint32_t i = 0; i < 3; i++) corner[c][i] = rdf(VIEW_CORNERS + CORNER_BYTES * c + 4u * i);
    for (int i = 0; i < 3; i++) {
        L.forward[i] = (corner[0][i] + corner[3][i]) * 0.5f;
        L.right[i] = (corner[1][i] - corner[0][i]) * 0.5f;
        L.up[i] = (corner[0][i] - corner[2][i]) * 0.5f;
        length[0] += L.forward[i] * L.forward[i], length[1] += L.right[i] * L.right[i], length[2] += L.up[i] * L.up[i];
    }
    return length[0] > 0.0f && length[1] > 0.0f && length[2] > 0.0f;
}

static void lit_frame(void) {
    jpt_lit_lights_t lights;
    if (L.count) memset(L.polygons, 0, LIT_POLYGONS * sizeof *L.polygons);
    if (L.point_count) memset(L.points, 0, LIT_POINTS * sizeof *L.points);
    L.count = L.vertex_count = L.point_count = 0;
    L.usable = jpt_texhw_normals() && lit_memory() && read_view() && read_lights(&lights) && jpt_lit_combine(&lights, &L.bump);
    if (L.usable && !L.logged++)
        host_log("custom-level", "texhw: normal maps in use; light of strength %.2f towards %.2f, %.2f, %.2f, ambient %.2f, of %d light(s)", L.bump.strength, L.bump.towards[0], L.bump.towards[1],
                 L.bump.towards[2], L.bump.ambient, lights.count);
}

static lit_polygon_t* lit_polygon(uint32_t polygon, int make) {
    unsigned i = polygon * 2654435761u >> 19;
    for (; L.polygons[i].polygon; i = (i + 1u) & (LIT_POLYGONS - 1u))
        if (L.polygons[i].polygon == polygon) return make ? NULL : &L.polygons[i];
    if (!make || L.count >= LIT_POLYGONS / 2u) return NULL;
    L.count++;
    L.polygons[i].polygon = polygon;
    return &L.polygons[i];
}

/* What the faces that share a vertex's point and light add up to. */
static float* point_normal(uint32_t vertex) {
    uint32_t key[4];
    unsigned i;
    memcpy(key, (const void*)ADDR(vertex), 3 * sizeof key[0]);
    key[3] = MEM32(vertex + VERTEX_VALUE);
    i = (key[0] * 2654435761u ^ key[1] * 2246822519u ^ key[2] * 3266489917u ^ key[3]) * 2654435761u >> 17;
    for (; L.points[i].used; i = (i + 1u) & (LIT_POINTS - 1u))
        if (!memcmp(L.points[i].key, key, sizeof key)) return L.points[i].normal;
    if (L.point_count >= LIT_POINTS / 2u) return NULL;
    L.point_count++;
    L.points[i].used = 1;
    memcpy(L.points[i].key, key, sizeof key);
    return L.points[i].normal;
}

static void world_at(uint32_t vertex, float out[3]) {
    const float* camera = (const float*)ADDR(vertex);
    for (int i = 0; i < 3; i++) out[i] = L.forward[i] * camera[1] + L.right[i] * camera[0] + L.up[i] * camera[2];
}

static int read_surface(uint32_t texture, lit_polygon_t* p) {
    uint32_t colors = MEM32(texture + TEXTURE_COLORS), material, table;
    if (!colors || !guest_readable(colors, COLORS_TABLE + WORD)) return 0;
    table = MEM32(colors + COLORS_TABLE);
    if (!table || !guest_readable(table, TABLE_LEAST + WORD) || !(material = MEM32(table)) || !guest_readable(material, MATERIAL_EMISSIVE + WORD)) return 0;
    if (MEM32(material + MATERIAL_FLAGS) & MATERIAL_REFRACTS) return 0;
    p->material.diffuse = rdf(material);
    p->material.specular = rdf(material + MATERIAL_SPECULAR);
    p->material.highlight = rdf(material + MATERIAL_HIGHLIGHT);
    p->material.emissive = rdf(material + MATERIAL_EMISSIVE);
    p->shade.exponent = rdf(GAMMA + GAMMA_EXPONENT);
    p->shade.scale = rdf(GAMMA + GAMMA_SCALE) * GAMMA_HALF;
    p->shade.least = rdf(table + TABLE_LEAST);
    p->shade.shaded = (float)(int32_t)MEM32(table + TABLE_SHADED);
    p->draw.dark = MEM32(table + TABLE_DARK) & 0xFFFFFFu;
    p->draw.full = MEM32(table + TABLE_FULL) & 0xFFFFFFu;
    return p->material.diffuse > 0.0f && p->shade.exponent > 0.0f;
}

static void lit_add(uint32_t polygon, int normal) {
    const uint32_t features = MEM32(polygon + POLYGON_FEATURES), count = MEM32(polygon + POLYGON_COUNT), list = MEM32(polygon + POLYGON_VERTICES);
    float at[CORNERS_MAX][3], uv[CORNERS_MAX][2];
    lit_polygon_t made, *p;
    memset(&made, 0, sizeof made);
    if (!(features & FEATURE_LIGHT) || (features & FEATURE_TERRAIN) || count < 3 || count > CORNERS_MAX || L.vertex_count + count > LIT_VERTICES) return;
    if (!read_surface(MEM32(polygon + POLYGON_TEXTURE), &made)) return;
    made.shaded = (features & FEATURE_SHADE) != 0;
    if (!made.shaded && rdf(polygon + POLYGON_VALUE) >= made.shade.shaded) return; /* of an unlit shape */
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t vertex = MEM32(list + 4u * i);
        world_at(vertex, at[i]);
        for (int k = 0; k < 3; k++) made.middle[k] += at[i][k] / (float)count;
        for (uint32_t k = 0; k < 2; k++) uv[i][k] = rdf(vertex + VERTEX_U + 4u * k);
    }
    if (!jpt_lit_face((const float(*)[3])at, (const float(*)[2])uv, count, &made.face) || (p = lit_polygon(polygon, 1)) == NULL) return;
    made.polygon = polygon;
    made.first = L.vertex_count;
    made.draw.normal = normal;
    made.draw.count = count;
    made.draw.vertices = L.vertices + L.vertex_count;
    L.vertex_count += count;
    *p = made;
    for (uint32_t i = 0; made.shaded && i < count; i++) {
        float* sum = point_normal(MEM32(list + 4u * i));
        for (int k = 0; sum && k < 3; k++) sum[k] += made.face.out[k];
    }
}

static void vertex_normal(const lit_polygon_t* p, uint32_t vertex, float out[3]) {
    const float* sum = p->shaded ? point_normal(vertex) : NULL;
    float length = 0.0f;
    for (int k = 0; sum && k < 3; k++) length += sum[k] * sum[k];
    if (!(length > 1e-6f)) sum = p->face.out, length = 1.0f;
    for (int k = 0; k < 3; k++) out[k] = sum[k] / sqrtf(length);
}

static void lit_finish(const lit_polygon_t* p) {
    const uint32_t list = MEM32(p->polygon + POLYGON_VERTICES);
    jpt_lit_bump_t bump = L.bump;
    if (p->material.specular > 0.0f) {
        float eye[3], length = 0.0f;
        for (int k = 0; k < 3; k++) length += p->middle[k] * p->middle[k];
        for (int k = 0; k < 3; k++) eye[k] = length > 0.0f ? -p->middle[k] / sqrtf(length) : 0.0f;
        jpt_lit_specular(&bump, &p->material, eye);
    }
    for (unsigned i = 0; i < p->draw.count; i++) {
        const uint32_t vertex = MEM32(list + 4u * i);
        jpt_lit_vertex_t* v = &L.vertices[p->first + i];
        float normal[3];
        vertex_normal(p, vertex, normal);
        jpt_lit_towards(&p->face, normal, &bump, p->shaded ? jpt_lit_cosine(&p->shade, &p->material, &bump, rdf(vertex + VERTEX_VALUE)) : -1.0f, v->light);
        v->light[3] = bump.strength;
        v->material[0] = p->material.diffuse;
        v->material[1] = p->material.specular;
        v->material[2] = p->material.highlight;
        v->material[3] = bump.ambient;
        v->shade[0] = p->shade.exponent;
        v->shade[1] = p->shade.least;
    }
}

void texhw_lit_begin(uint32_t polygon) {
    const lit_polygon_t* p = L.count ? lit_polygon(polygon, 0) : NULL;
    if (p) jpt_lit_set_draw(&p->draw);
}

void texhw_lit_end(void) { jpt_lit_set_draw(NULL); }

static uint32_t drawn_raster(uint32_t polygon, uint32_t texture) {
    const uint32_t count = MEM32(texture + TEXTURE_MIP_COUNT), mip = MEM32(polygon + POLYGON_MIP);
    if (!count || count > TEXTURE_MIPS_MOST) return 0;
    if (count > 1 && mip >= count) return 0;
    return MEM32(texture + TEXTURE_MIPS + 4u * (count > 1 ? mip : 0));
}

static void tag_polygon(uint32_t polygon, texhw_tag_fn tag) {
    const uint32_t texture = MEM32(polygon + POLYGON_TEXTURE), draw = MEM32(polygon + POLYGON_DRAW);
    uint32_t hash, raster, linked, iface;
    int id;
    if (!texture || draw < DRAW_FIRST || draw > DRAW_LAST) return;
    hash = MEM32(texture + TEXTURE_HASH);
    if ((id = jpt_texhw_find(hash)) == 0 || !owns(hash, texture)) return;
    raster = drawn_raster(polygon, texture);
    if (!raster || raster == NULL_RASTER) return;
    linked = MEM32(raster + RASTER_LINKED);
    if (!linked || linked == NULL_RASTER || (iface = MEM32(linked + RASTER_D3D_TEXTURE)) == 0) return;
    if (MEM32(raster + RASTER_WIDTH) != MEM32(linked + RASTER_WIDTH) || MEM32(raster + RASTER_HEIGHT) != MEM32(linked + RASTER_HEIGHT)) {
        if (!s_padded_logged++) host_log("custom-level", "texhw: a texture whose sides are not powers of two keeps the level's own picture");
        return;
    }
    tag(iface, id);
    if (L.usable && (id = jpt_texhw_normal(hash)) != 0) lit_add(polygon, id);
}

void texhw_tag_polygons(uint32_t count, uint32_t polygons, texhw_tag_fn tag) {
    uint32_t polygon;
    if (!jpt_texhw_active()) {
        L.count = 0;
        return;
    }
    jpt_texhw_frame();
    lit_frame();
    if (s_generation != jpt_texhw_generation()) {
        s_generation = jpt_texhw_generation();
        memset(s_owner, 0, sizeof s_owner);
    }
    for (uint32_t i = 0; i < count; i++)
        if (guest_read32(polygons + 4u * i, &polygon) && polygon) tag_polygon(polygon, tag);
    for (unsigned i = 0; L.count && i < LIT_POLYGONS; i++)
        if (L.polygons[i].polygon) lit_finish(&L.polygons[i]);
}

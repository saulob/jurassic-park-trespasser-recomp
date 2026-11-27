#include "jpt_groff.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_alloc.h"
#include "jpt_compat.h"
#include "jpt_hash.h"
#include "jpt_save.h"

#define MAGIC 0x0ACEBABEu
#define FILE_HEADER 48u
#define SECTION_ENTRY 32u
#define SECTIONS_MAX (1u << 20)
#define SYMBOL_HEAD 12u
#define REGION_OBJECT 44u
#define OBJECT_WORDS 7u
#define OPTIMIZED 0x80000000u
#define MESH_HEAD 44u
#define MESH_POINT 12u
#define MESH_VERTEX 32u
#define MESH_POLYGON 40u
#define POLYGON_SURFACE 24u
#define POLYGON_FLAGS 28u
#define FRAMES_MAX 30
#define POLYGON_CURVED 4u
#define GEOMETRY_HEAD 40u
#define PID_HEAD 32u
#define PID_ENTRY 56u
#define PID_WIDTH 8u
#define PID_HEIGHT 12u
#define PID_HASH 48u
#define NO_PICTURE 0xFFFFFFFFu
#define UNUSED_MIP 0xFFFFFFFFu
#define RECORDS_MAX (1u << 22)
#define DEFAULT_DIFFUSE 1.0f
#define SURFACES_OF_MODEL 32u
#define CORNERS_MAX 58u
#define ACTIONS_MAX 16u
#define EXPRESSION_ELEMENTS_MAX 16u
#define EXPRESSION_TEXT_MAX 255u
#define EXPRESSION_SYMBOL_MAX 127u
#define MODELS_MAX 10u
#define MODEL_NAMES 12 /* the names the engine knows */
#define OBJECT_NAME_MAX 95u
#define JOINTS_MAX 32u
#define SUBSTITUTE_POINTS_MAX 1636u /* 1024 flags, then three texts of the loader's frame that nothing reads any more */
#define FACES_AT_POINT_MAX 64u
#define NORMALS_AT_POINT_MAX 16u
#define PLANTED_POINTS_MAX 64u
#define MAGNET_BOXES_MAX 10u
#define POOL_BYTES (JPT_GROFF_POOL_BYTES_MAX - JPT_GROFF_POOL_ROOM)
#define OWN_DINOSAURS 7
#define SCENE_RECORD 32u /* of an object in a saved level, at most */
#define PID_PALETTES 20u
#define PID_FIRST_PAGES 24u
#define NO_PALETTE 0xFFFFFFFFu

enum { VALUE_BOOL = 1, VALUE_CHAR, VALUE_INT, VALUE_FLOAT, VALUE_STRING, VALUE_OBJECT };
enum { NAME_OTHER, NAME_VISIBLE, NAME_JOINTS, NAME_BUMPMAPS, NAME_DIFFUSE, NAME_CLASS, NAME_FILE, NAME_ACTION, NAME_LEVEL, NAME_CURVED, NAME_EXPRESSION, NAME_TRIGGER_ACTION, NAME_LINK, NAME_NORMALS, NAME_PLANTED, NAME_ANIMATED_SURFACE, NAME_MASTER, NAME_SLAVE, NAME_TYPE, NAME_MESH, NAME_PHYSICS, NAME_BOUND, NAME_DINOSAUR, NAME_SAMPLE, NAME_SCRIPT, NAME_SCRIPT_AT, NAME_MODEL, NAME_SURFACE = NAME_MODEL + MODEL_NAMES, NAME_SURFACE_LAST = NAME_SURFACE + 99, NAME_FRAME, NAME_FRAME_BUMP = NAME_FRAME + FRAMES_MAX };

typedef struct {
    uint32_t key, value;
} slot_t;

typedef struct {
    slot_t* slots;
    uint32_t mask;
} map_t;

typedef struct {
    uint32_t handle, offset, size;
} section_t;

typedef struct {
    const char* text;
    uint8_t name;
} symbol_t;

typedef struct {
    uint32_t key;
    uint32_t at; /* of its value, from the start of the file */
    const char* label;
    uint8_t name, type;
    union {
        int32_t i;
        float f;
        const char* s;
        const uint8_t* children; /* of them: 8 bytes each */
    } v;
    uint32_t child_count;
} record_t;

struct jpt_groff {
    const uint8_t* bytes;
    size_t size;
    section_t* sections;
    symbol_t *symbols, *names; /* the file's, and the value table's as NAME_... */
    record_t* records;
    uint32_t section_count, symbol_count, name_count, record_count;
    map_t by_section, by_symbol, by_name, by_record;
    const uint8_t* objects;
    uint32_t object_count;
    int optimized;
    char folder[JPT_GROFF_TEXT_MAX]; /* of the textures it names without one */
};

typedef struct {
    uint64_t hash;
    const uint8_t* entry;
} chunk_t;

struct jpt_texture_set {
    const uint8_t* bytes;
    uint32_t count;
    chunk_t* chunks; /* by hash */
    int borrowed;
    map_t models; /* of the world it was made for: shapes the engine builds itself, with their textures */
};

typedef struct {
    const uint8_t* p;
    size_t left;
    int failed;
} reader_t;

static uint32_t rd(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t* p) { return rd(p) | (uint64_t)rd(p + 4) << 32; }

static void wr(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static float rdf(const uint8_t* p) {
    const uint32_t bits = rd(p);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static uint32_t mixed(uint32_t key) { return (key ^ key >> 15) * 0x9E3779B1u; }

static int map_make(map_t* map, uint32_t count) {
    uint32_t cap = 16;
    while (cap < count * 2u) cap *= 2u;
    map->slots = (slot_t*)jpt_alloc(cap, sizeof *map->slots);
    if (!map->slots) return 0;
    memset(map->slots, 0xFF, (size_t)cap * sizeof *map->slots);
    map->mask = cap - 1;
    return 1;
}

/* value: never UINT32_MAX. The first of a key stays. */
static void map_put(map_t* map, uint32_t key, uint32_t value) {
    uint32_t i = mixed(key) & map->mask;
    while (map->slots[i].value != UINT32_MAX && map->slots[i].key != key) i = (i + 1) & map->mask;
    if (map->slots[i].value != UINT32_MAX) return;
    map->slots[i].key = key;
    map->slots[i].value = value;
}

static uint32_t map_get(const map_t* map, uint32_t key) {
    uint32_t i = mixed(key) & map->mask;
    while (map->slots[i].value != UINT32_MAX && map->slots[i].key != key) i = (i + 1) & map->mask;
    return map->slots[i].value;
}

static const uint8_t* take(reader_t* r, size_t n) {
    const uint8_t* p = r->p;
    if (r->failed || n > r->left) {
        r->failed = 1;
        return NULL;
    }
    r->p += n;
    r->left -= n;
    return p;
}

static uint32_t word(reader_t* r) {
    const uint8_t* p = take(r, 4);
    return p ? rd(p) : 0;
}

static uint8_t byte(reader_t* r) {
    const uint8_t* p = take(r, 1);
    return p ? *p : 0;
}

static const char* string(reader_t* r) {
    const uint8_t* end = r->failed ? NULL : (const uint8_t*)memchr(r->p, 0, r->left);
    const uint8_t* p;
    if (!end) {
        r->failed = 1;
        return "";
    }
    p = take(r, (size_t)(end - r->p) + 1);
    return p ? (const char*)p : "";
}

static uint32_t handle(reader_t* r) {
    const uint32_t h = word(r);
    word(r);
    return h;
}

static void profile(reader_t* r) {
    string(r);
    take(r, 13);
}

static const section_t* section_of(const jpt_groff_t* g, uint32_t h) {
    const uint32_t i = map_get(&g->by_section, h);
    return i == UINT32_MAX ? NULL : &g->sections[i];
}

static const char* symbol_of(const jpt_groff_t* g, uint32_t h) {
    const uint32_t i = map_get(&g->by_symbol, h);
    return i == UINT32_MAX ? "" : g->symbols[i].text;
}

static const record_t* record_of(const jpt_groff_t* g, uint32_t h) {
    const uint32_t i = map_get(&g->by_record, h);
    return i == UINT32_MAX ? NULL : &g->records[i];
}

static int numbered(const char* text, const char* prefix) {
    const size_t n = strlen(prefix);
    if (strncmp(text, prefix, n) || text[n] < '0' || text[n] > '9' || text[n + 1] < '0' || text[n + 1] > '9' || text[n + 2]) return -1;
    return (text[n] - '0') * 10 + text[n + 1] - '0';
}

static uint8_t known_name(const char* text) {
    static const char* const k_names[] = { "", "Visible", "NumJoints", "Bumpmaps", "Diffuse", "Class", "File", "ActionType", "LevelName", "Curved", "FireExpression" };
    static const char* const k_late[] = { "Normals", "Planted", "AnimSubMaterial", "MasterObject", "SlaveObject", "Type", "Mesh", "Physics", "BoundVol", "Dinosaur", "Sample", "Script", "ScriptAT" };
    const int surface = numbered(text, "SubMaterial"), frame = numbered(text, "Anim"), bump = numbered(text, "AnimB"), model = numbered(text, "Model");
    if (surface >= 0) return (uint8_t)(NAME_SURFACE + surface);
    if (numbered(text, "Action") >= 0) return NAME_TRIGGER_ACTION;
    if (model >= 0) return model < MODEL_NAMES ? (uint8_t)(NAME_MODEL + model) : NAME_OTHER;
    if (numbered(text, "A") >= 0) return NAME_LINK;
    for (uint8_t i = 0; i < sizeof k_late / sizeof k_late[0]; i++)
        if (!strcmp(text, k_late[i])) return (uint8_t)(NAME_NORMALS + i);
    if (frame >= 0) return frame < FRAMES_MAX ? (uint8_t)(NAME_FRAME + frame) : NAME_OTHER;
    if (bump >= 0) return bump < FRAMES_MAX ? (uint8_t)(NAME_FRAME_BUMP + bump) : NAME_OTHER;
    for (uint8_t i = NAME_VISIBLE; i < sizeof k_names / sizeof k_names[0]; i++)
        if (!strcmp(text, k_names[i])) return i;
    return NAME_OTHER;
}

static int read_symbols(jpt_groff_t* g, uint32_t at, uint32_t count) {
    reader_t r = { g->bytes + at, g->size - at, 0 };
    if (count > r.left / SYMBOL_HEAD || (g->symbols = (symbol_t*)jpt_alloc_zero(count, sizeof *g->symbols)) == NULL) return 0;
    if (!map_make(&g->by_symbol, count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *head = take(&r, SYMBOL_HEAD), *text = head ? take(&r, rd(head + 4)) : NULL;
        if (!text || !rd(head + 4) || text[rd(head + 4) - 1]) return 0;
        g->symbols[i].text = (const char*)text;
        map_put(&g->by_symbol, rd(head), i);
    }
    g->symbol_count = count;
    return 1;
}

static int read_sections(jpt_groff_t* g, uint32_t count) {
    if (count > SECTIONS_MAX || count > (g->size - FILE_HEADER) / SECTION_ENTRY) return 0;
    g->sections = (section_t*)jpt_alloc_zero(count, sizeof *g->sections);
    if (!g->sections || !map_make(&g->by_section, count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = g->bytes + FILE_HEADER + (size_t)i * SECTION_ENTRY;
        section_t* s = &g->sections[i];
        s->offset = rd(e + 8);
        s->size = rd(e + 12);
        s->handle = rd(e + 28);
        if (s->offset > g->size || s->size > g->size - s->offset) return 0;
        map_put(&g->by_section, s->handle, i);
    }
    g->section_count = count;
    return 1;
}

static const section_t* section_named(const jpt_groff_t* g, const char* name) {
    for (uint32_t i = 0; i < g->section_count; i++)
        if (!strcmp(symbol_of(g, rd(g->bytes + FILE_HEADER + (size_t)i * SECTION_ENTRY)), name)) return &g->sections[i];
    return NULL;
}

static int read_value(reader_t* r, record_t* record) {
    switch (record->type) {
    case VALUE_BOOL:
    case VALUE_CHAR: record->v.i = byte(r); break;
    case VALUE_INT: record->v.i = (int32_t)word(r); break;
    case VALUE_FLOAT: {
        const uint8_t* p = take(r, 4);
        record->v.f = p ? rdf(p) : 0;
        break;
    }
    case VALUE_STRING: record->v.s = string(r); break;
    case VALUE_OBJECT:
        record->child_count = word(r);
        if (record->child_count > r->left / 8) return 0;
        record->v.children = take(r, (size_t)record->child_count * 8);
        break;
    default: return 0;
    }
    return !r->failed;
}

static int read_values(jpt_groff_t* g, const section_t* s) {
    reader_t r = { g->bytes + s->offset, s->size, 0 };
    uint32_t count;
    string(&r);
    profile(&r);
    string(&r);
    count = word(&r);
    if (r.failed || count > r.left / 9 || (g->names = (symbol_t*)jpt_alloc_zero(count, sizeof *g->names)) == NULL || !map_make(&g->by_name, count)) return 0;
    for (g->name_count = 0; g->name_count < count; g->name_count++) {
        const char* text = string(&r);
        const uint32_t h = handle(&r);
        if (r.failed) return 0;
        g->names[g->name_count].text = text;
        g->names[g->name_count].name = known_name(text);
        map_put(&g->by_name, h, g->name_count);
    }
    profile(&r);
    count = word(&r);
    if (r.failed || count > RECORDS_MAX || count > r.left / 36 || (g->records = (record_t*)jpt_alloc_zero(count, sizeof *g->records)) == NULL) return 0;
    if (!map_make(&g->by_record, count)) return 0;
    for (g->record_count = 0; g->record_count < count; g->record_count++) {
        record_t* record = &g->records[g->record_count];
        uint32_t name;
        record->type = byte(&r);
        take(&r, 2);
        handle(&r);
        name = map_get(&g->by_name, handle(&r));
        record->name = name == UINT32_MAX ? NAME_OTHER : g->names[name].name;
        record->label = name == UINT32_MAX ? "" : g->names[name].text;
        handle(&r);
        record->at = (uint32_t)(r.p - g->bytes);
        if (!read_value(&r, record)) return 0;
        record->key = handle(&r);
        if (r.failed) return 0;
        map_put(&g->by_record, record->key, g->record_count);
    }
    return 1;
}

static const char* read_world(jpt_groff_t* g) {
    const section_t *header, *region, *values;
    uint32_t symbols_at;
    if (g->size < FILE_HEADER || rd(g->bytes) != MAGIC) return "not a world file";
    symbols_at = rd(g->bytes + 20);
    if (symbols_at > g->size || !read_symbols(g, symbols_at, rd(g->bytes + 12))) return "its symbol table is damaged";
    if (!read_sections(g, rd(g->bytes + 8))) return "its section table is damaged";
    header = section_named(g, ".header");
    region = section_named(g, ".region");
    if (!header || header->size < 8 || !region || region->size < 8) return "it has no header or no region";
    g->optimized = (rd(g->bytes + header->offset + 4) & OPTIMIZED) != 0;
    g->object_count = rd(g->bytes + region->offset);
    if (g->object_count > (region->size - 8) / REGION_OBJECT) return "its list of objects is damaged";
    g->objects = g->bytes + region->offset + 4;
    values = section_of(g, rd(g->objects + (size_t)g->object_count * REGION_OBJECT));
    if (!values || !read_values(g, values)) return "its properties are damaged";
    return NULL;
}

jpt_groff_t* jpt_groff_open(const char* name, const uint8_t* bytes, size_t size, char* problem, size_t cap) {
    jpt_groff_t* g = (jpt_groff_t*)jpt_alloc_zero(1, sizeof *g);
    const char *wrong = "out of memory", *dot = strrchr(name, '.');
    if (g) {
        snprintf(g->folder, sizeof g->folder, "Map\\%.*s\\", (int)(dot ? (size_t)(dot - name) : strlen(name)), name);
        g->bytes = bytes;
        g->size = size > UINT32_MAX ? UINT32_MAX : size;
        wrong = read_world(g);
    }
    if (cap) snprintf(problem, cap, "%s", wrong ? wrong : "");
    if (!wrong) return g;
    jpt_groff_close(g);
    return NULL;
}

void jpt_groff_close(jpt_groff_t* g) {
    if (!g) return;
    free(g->sections);
    free(g->symbols);
    free(g->names);
    free(g->records);
    free(g->by_section.slots);
    free(g->by_symbol.slots);
    free(g->by_name.slots);
    free(g->by_record.slots);
    free(g);
}

/* As the engine looks one up: from the index on, then from the first. */
static const record_t* property_from(const jpt_groff_t* g, const record_t* object, int name, uint32_t first) {
    if (!object || object->type != VALUE_OBJECT) return NULL;
    for (uint32_t i = 0; i < object->child_count; i++) {
        const uint32_t at = first < object->child_count ? (first + i) % object->child_count : i;
        const record_t* child = record_of(g, rd(object->v.children + (size_t)at * 8));
        if (child && child->name == name) return child;
    }
    return NULL;
}

static const record_t* property(const jpt_groff_t* g, const record_t* object, int name) { return property_from(g, object, name, 0); }

static const char* text_property(const jpt_groff_t* g, const record_t* object, int name) {
    const record_t* r = property(g, object, name);
    return r && r->type == VALUE_STRING ? r->v.s : "";
}

static int is_false(const jpt_groff_t* g, const record_t* object, int name) {
    const record_t* r = property(g, object, name);
    return r && r->type == VALUE_BOOL && !r->v.i;
}

static int whole(const jpt_groff_t* g, const record_t* object, int name) {
    const record_t* r = property(g, object, name);
    return r && r->type == VALUE_INT ? r->v.i : 0;
}

static void copy(char out[JPT_GROFF_TEXT_MAX], const char* text) { snprintf(out, JPT_GROFF_TEXT_MAX, "%s", text); }

static void add_action(jpt_groff_report_t* report, int type) {
    for (unsigned i = 0; i < report->action_count; i++)
        if (report->actions[i].type == type) {
            report->actions[i].count++;
            return;
        }
    if (report->action_count == JPT_GROFF_ACTIONS_MAX) return;
    report->actions[report->action_count].type = type;
    report->actions[report->action_count++].count = 1;
}

static void add_level(jpt_groff_report_t* report, const char* name) {
    for (unsigned i = 0; i < report->level_count; i++)
        if (!_stricmp(report->levels[i], name)) return;
    if (name[0] && report->level_count < JPT_GROFF_LEVELS_MAX) copy(report->levels[report->level_count++], name);
}

/* ATX reads the picture's name up to a quote. */
static void add_map(jpt_groff_report_t* report, const char* sample) {
    char name[JPT_GROFF_TEXT_MAX];
    snprintf(name, sizeof name, "%.*s", (int)strcspn(sample, "\""), sample);
    for (unsigned i = 0; i < report->map_count; i++)
        if (!_stricmp(report->maps[i], name)) return;
    if (name[0] && report->map_count < JPT_GROFF_LEVELS_MAX) copy(report->maps[report->map_count++], name);
}

static void add_script(jpt_groff_report_t* report, const char* class_name, const char* object, int action) {
    if (!class_name[0]) return;
    for (unsigned i = 0; i < report->script_count; i++)
        if (report->scripts[i].action == action && !strcmp(report->scripts[i].class_name, class_name)) {
            report->scripts[i].count++;
            return;
        }
    if (report->script_count == JPT_GROFF_SCRIPTS_MAX) {
        report->scripts_overflow = 1;
        return;
    }
    copy(report->scripts[report->script_count].class_name, class_name);
    copy(report->scripts[report->script_count].object, object);
    report->scripts[report->script_count].action = action;
    report->scripts[report->script_count++].count = 1;
}

/* Trespasser CE: Script on any object, ScriptAT on a trigger's action 99. */
static void describe_scripts(const jpt_groff_t* g, const record_t* values, const char* name, jpt_groff_report_t* report) {
    add_script(report, text_property(g, values, NAME_SCRIPT), name, 0);
    if (!values || values->type != VALUE_OBJECT) return;
    for (uint32_t i = 0; i < values->child_count; i++) {
        const record_t* child = record_of(g, rd(values->v.children + (size_t)i * 8));
        if (child && child->type == VALUE_OBJECT && child->name == NAME_TRIGGER_ACTION && whole(g, child, NAME_ACTION) == 99)
            add_script(report, text_property(g, child, NAME_SCRIPT_AT), name, 1);
    }
}

static int by_type(const void* a, const void* b) {
    const jpt_groff_action_t *x = (const jpt_groff_action_t*)a, *y = (const jpt_groff_action_t*)b;
    return (x->type > y->type) - (x->type < y->type);
}

typedef struct {
    const uint8_t* region;
    const record_t* values;
    const section_t* geometry;
    uint32_t mapping;
} object_t;

static int is_text(const record_t* r) { return r && r->type == VALUE_STRING; }

/* A trigger without a bounding volume of its own has the shape all of them share. */
static int shares_shape(const jpt_groff_t* g, const record_t* values) {
    static const char* const k_triggers[] = { "CLocationTrigger", "CCollisionTrigger", "CStartTrigger", "CCreatureTrigger", "CSequenceTrigger",
                                              "CBooleanTrigger", "CTimerTrigger", "CVariableTrigger", "CObjectTrigger", "CMoreMassTrigger" };
    const char* type = text_property(g, values, NAME_CLASS);
    for (size_t i = 0; i < sizeof k_triggers / sizeof k_triggers[0]; i++)
        if (!strcmp(type, k_triggers[i])) return !whole(g, values, NAME_BOUND);
    return 0;
}

static object_t object_with_mesh(const jpt_groff_t* g, uint32_t index) {
    object_t o = { g->objects + (size_t)index * REGION_OBJECT, NULL, NULL, 0 };
    const section_t* s = section_of(g, rd(o.region));
    o.values = record_of(g, rd(o.region + 36));
    if (s && s->size >= OBJECT_WORDS * 4) {
        o.geometry = section_of(g, rd(g->bytes + s->offset + 4));
        o.mapping = rd(g->bytes + s->offset + 8);
    }
    return o;
}

/* Without a mesh when the engine does not read the object's own. */
static object_t object_at(const jpt_groff_t* g, uint32_t index) {
    object_t o = object_with_mesh(g, index);
    if (is_text(property(g, o.values, NAME_MESH)) || shares_shape(g, o.values)) o.geometry = NULL, o.mapping = 0;
    return o;
}

typedef struct {
    const uint8_t *points, *vertices, *pointers, *polygons;
    uint32_t point_count, vertex_count, pointer_count, polygon_count, material;
} mesh_t;

/* A mesh of an optimized world; 0 for one the engine builds itself. */
static int mesh_of(const jpt_groff_t* g, const object_t* o, mesh_t* m) {
    const uint8_t* p;
    uint64_t end;
    uint32_t wrap;
    if (!g->optimized || !o->geometry || o->geometry->size < MESH_HEAD || is_false(g, o->values, NAME_VISIBLE) || whole(g, o->values, NAME_JOINTS)) return 0;
    p = g->bytes + o->geometry->offset;
    m->material = rd(p);
    m->point_count = rd(p + 24);
    m->vertex_count = rd(p + 28);
    m->pointer_count = rd(p + 32);
    wrap = rd(p + 36);
    m->polygon_count = rd(p + 40);
    end = MESH_HEAD + (uint64_t)m->point_count * MESH_POINT + (uint64_t)m->vertex_count * MESH_VERTEX + (uint64_t)m->pointer_count * 4 +
          (uint64_t)wrap * MESH_POINT + (uint64_t)m->polygon_count * MESH_POLYGON;
    if (end != o->geometry->size) return 0;
    m->points = p + MESH_HEAD;
    m->vertices = m->points + (size_t)m->point_count * MESH_POINT;
    m->pointers = m->vertices + (size_t)m->vertex_count * MESH_VERTEX;
    m->polygons = m->pointers + (size_t)m->pointer_count * 4;
    return 1;
}

/* Vertices, two lists of faces, normals and wrap points: what an object stores before it is optimized. */
static int plain_faces(const jpt_groff_t* g, const section_t* s, uint32_t* faces) {
    const uint8_t* p = g->bytes + s->offset;
    uint64_t end;
    if (s->size < GEOMETRY_HEAD) return 0;
    *faces = rd(p + 16);
    end = GEOMETRY_HEAD + 12 * ((uint64_t)rd(p + 12) + 2 * (uint64_t)*faces + rd(p + 20) + rd(p + 24));
    return end == s->size;
}

typedef struct {
    uint32_t points, vertices, faces;
} sizes_t;

static int sizes_of(const jpt_groff_t* g, const object_t* o, sizes_t* sizes) {
    mesh_t mesh;
    if (mesh_of(g, o, &mesh)) {
        sizes->points = mesh.point_count;
        sizes->vertices = mesh.vertex_count;
        sizes->faces = mesh.polygon_count;
        return 1;
    }
    if (!o->geometry || is_false(g, o->values, NAME_VISIBLE) || !plain_faces(g, o->geometry, &sizes->faces)) {
        sizes->faces = 0;
        return 0;
    }
    sizes->points = rd(g->bytes + o->geometry->offset + 12);
    sizes->vertices = sizes->faces > UINT32_MAX / 3 ? UINT32_MAX : sizes->faces * 3;
    return 1;
}

static uint32_t hash_name(uint32_t crc, const char* text) {
    for (; *text; text++) {
        const char c = *text == '\\' ? '/' : *text >= 'A' && *text <= 'Z' ? (char)(*text + 32) : *text;
        crc = jpt_crc32(crc, &c, 1);
    }
    return crc;
}

static void fault(jpt_groff_report_t* report, const char* code, const char* subject, uint32_t value, uint32_t limit) {
    jpt_groff_fault_t* f = report->faults;
    while (f < report->faults + report->fault_count && strcmp(f->code, code)) f++;
    if (f == report->faults + JPT_GROFF_FAULTS_MAX) return;
    if (f == report->faults + report->fault_count) {
        report->fault_count++;
        f->code = code;
        copy(f->subject, subject);
        f->value = value;
        f->limit = limit;
    }
    f->count++;
}

static void at_most(jpt_groff_report_t* report, const char* code, const char* subject, uint32_t value, uint32_t limit) {
    if (value > limit) fault(report, code, subject, value, limit);
}

/* The builder's arrays for this many faces hold 16 pointers, 4 vertices and 4 points of each. */
static void need_build(jpt_groff_report_t* report, uint32_t count, uint32_t of_each) {
    const unsigned faces = count / of_each + (count % of_each != 0);
    if (faces > report->mesh_build) report->mesh_build = faces;
}

/* What the mesh builder's arrays hold, and the indices the engine follows without looking. */
static void check_mesh(const jpt_groff_t* g, const object_t* o, const mesh_t* m, const char* name, jpt_groff_report_t* report) {
    const section_t* material = section_of(g, m->material);
    const uint32_t surfaces = (material && material->size >= 4 ? rd(g->bytes + material->offset) : 0) + (rd(g->bytes + o->geometry->offset + 4) ? 1 : 0);
    const uint32_t wrap = rd(g->bytes + o->geometry->offset + 36);
    at_most(report, "MESH_EXCEEDS_LIMIT", name, m->polygon_count, JPT_GROFF_MESH_FACES_MAX);
    at_most(report, "MESH_EXCEEDS_LIMIT", name, m->pointer_count, 16 * JPT_GROFF_MESH_FACES_MAX);
    at_most(report, "MESH_EXCEEDS_LIMIT", name, m->vertex_count, 4 * JPT_GROFF_MESH_FACES_MAX);
    at_most(report, "MESH_EXCEEDS_LIMIT", name, m->point_count, 4 * JPT_GROFF_MESH_FACES_MAX);
    at_most(report, "MESH_EXCEEDS_LIMIT", name, wrap, 4 * JPT_GROFF_MESH_FACES_MAX);
    at_most(report, "MESH_EXCEEDS_LIMIT", name, surfaces, JPT_GROFF_MESH_FACES_MAX);
    need_build(report, m->polygon_count, 1);
    need_build(report, m->pointer_count, 16);
    need_build(report, m->vertex_count, 4);
    need_build(report, m->point_count, 4);
    need_build(report, wrap, 4);
    need_build(report, surfaces, 1);
    for (uint32_t i = 0; i < m->vertex_count; i++)
        if (rd(m->vertices + (size_t)i * MESH_VERTEX) >= m->point_count) fault(report, "INVALID_MESH_INDEX", name, rd(m->vertices + (size_t)i * MESH_VERTEX), m->point_count);
    for (uint32_t i = 0; i < m->pointer_count; i++)
        if (rd(m->pointers + (size_t)i * 4) >= m->vertex_count) fault(report, "INVALID_MESH_INDEX", name, rd(m->pointers + (size_t)i * 4), m->vertex_count);
    for (uint32_t i = 0; i < m->polygon_count; i++) {
        const uint8_t* p = m->polygons + (size_t)i * MESH_POLYGON;
        if (rd(p + 4) > m->pointer_count || rd(p) > m->pointer_count - rd(p + 4)) fault(report, "INVALID_MESH_INDEX", name, rd(p + 4), m->pointer_count);
        if (rd(p + POLYGON_SURFACE) >= (surfaces ? surfaces : 1)) fault(report, "INVALID_SURFACE_INDEX", name, rd(p + POLYGON_SURFACE), surfaces);
        at_most(report, "POLYGON_EXCEEDS_LIMIT", name, rd(p), CORNERS_MAX);
    }
    report->mesh_bytes += 40ull * m->polygon_count + 4ull * m->pointer_count + 32ull * m->vertex_count + 12ull * m->point_count + 12ull * wrap + 4ull * surfaces + 0x90;
}

/* The engine keeps an expression as 16 elements, its text without blanks in 256 bytes and each symbol in 128. */
static void check_expression(const char* text, const char* name, jpt_groff_report_t* report) {
    uint32_t elements = 0, length = 0, symbol = 0, longest = 0;
    for (const char* c = text;; c++) {
        const int is_operator = *c && strchr("&|^=!", *c) != NULL;
        if (!*c || is_operator || strchr("() \t", *c)) {
            elements += symbol != 0;
            longest = symbol > longest ? symbol : longest;
            symbol = 0;
        } else {
            symbol++;
        }
        if (!*c) break;
        elements += is_operator;
        length += *c != ' ' && *c != '\t';
    }
    at_most(report, "EXPRESSION_EXCEEDS_LIMIT", name, elements, EXPRESSION_ELEMENTS_MAX);
    at_most(report, "EXPRESSION_EXCEEDS_LIMIT", name, length, EXPRESSION_TEXT_MAX);
    at_most(report, "EXPRESSION_EXCEEDS_LIMIT", name, longest, EXPRESSION_SYMBOL_MAX);
}

static const char* object_name(const jpt_groff_t* g, uint32_t index) { return symbol_of(g, rd(g->objects + (size_t)index * REGION_OBJECT + 4)); }

/* UINT32_MAX: none. */
static uint32_t object_named(const jpt_groff_t* g, const map_t* names, const char* name) {
    const uint32_t i = map_get(names, hash_name(0, name));
    return i != UINT32_MAX && !_stricmp(object_name(g, i), name) ? i : UINT32_MAX;
}

/* The loader tells upper from lower case. */
static int object_exactly(const jpt_groff_t* g, const map_t* names, const char* name) {
    const uint32_t first = map_get(names, hash_name(0, name));
    if (first == UINT32_MAX) return 0;
    if (!strcmp(object_name(g, first), name)) return 1;
    for (uint32_t i = 0; i < g->object_count; i++)
        if (!strcmp(object_name(g, i), name)) return 1;
    return 0;
}

static uint32_t points_of(const jpt_groff_t* g, uint32_t index) {
    const object_t o = object_with_mesh(g, index);
    uint32_t faces;
    return o.geometry && plain_faces(g, o.geometry, &faces) ? rd(g->bytes + o.geometry->offset + 12) : 0;
}

static int is_true(const jpt_groff_t* g, const record_t* object, int name) {
    const record_t* r = property(g, object, name);
    return r && r->type == VALUE_BOOL && r->v.i;
}

typedef struct {
    float x, y, z;
} vector_t;

/* 0, with nothing written, for a face with a corner that is not one of the points (INVALID_MESH_INDEX). */
static int face_normal(const uint8_t* points, uint32_t point_count, const uint8_t* face, vector_t* normal) {
    vector_t p[3], n;
    float length;
    for (int k = 0; k < 3; k++)
        if (rd(face + k * 4) >= point_count) return 0;
    for (int k = 0; k < 3; k++) {
        const uint8_t* at = points + (size_t)rd(face + k * 4) * 12;
        p[k].x = rdf(at), p[k].y = rdf(at + 4), p[k].z = rdf(at + 8);
    }
    n.x = (p[1].y - p[0].y) * (p[2].z - p[0].z) - (p[1].z - p[0].z) * (p[2].y - p[0].y);
    n.y = (p[1].z - p[0].z) * (p[2].x - p[0].x) - (p[1].x - p[0].x) * (p[2].z - p[0].z);
    n.z = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
    length = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length > 0) n.x /= length, n.y /= length, n.z /= length;
    *normal = n;
    return 1;
}

/* The normals the engine makes of the faces that meet in a point: one for each group within the crease of one another. */
static uint32_t groups_at(const uint8_t* points, uint32_t point_count, const uint8_t* faces, uint32_t face_count, uint32_t point, float crease) {
    vector_t member[FACES_AT_POINT_MAX];
    uint8_t group[FACES_AT_POINT_MAX];
    uint32_t count = 0, groups = 0;
    for (uint32_t f = 0; f < face_count && count < FACES_AT_POINT_MAX; f++) {
        const uint8_t* face = faces + (size_t)f * 12;
        uint32_t joined = 0;
        if (rd(face) != point && rd(face + 4) != point && rd(face + 8) != point) continue;
        if (!face_normal(points, point_count, face, &member[count])) continue;
        for (; joined < groups; joined++) {
            uint32_t k = 0;
            for (; k < count; k++)
                if (group[k] == joined && member[k].x * member[count].x + member[k].y * member[count].y + member[k].z * member[count].z <= crease) break;
            if (k == count) break;
        }
        group[count++] = (uint8_t)joined;
        groups += joined == groups;
    }
    return groups;
}

/* Of a mesh the engine builds itself. 0: out of memory. */
static int check_plain(const jpt_groff_t* g, const object_t* o, int skeleton, const char* name, jpt_groff_report_t* report) {
    const uint8_t* geometry = g->bytes + o->geometry->offset;
    const uint32_t point_count = rd(geometry + 12), face_count = rd(geometry + 16);
    const uint8_t *points = geometry + GEOMETRY_HEAD, *faces = points + (size_t)point_count * 12;
    const int normals = skeleton ? 2 : whole(g, o->values, NAME_NORMALS);
    const float crease = normals == 1 ? 0.999f : normals == 2 ? -1.0f : 0.258819f;
    uint32_t* met = (uint32_t*)jpt_alloc_zero(point_count, sizeof *met);
    if (!met) return 0;
    for (uint32_t i = 0; i < face_count * 3; i++) {
        const uint32_t p = rd(faces + (size_t)i * 4);
        if (p < point_count) met[p]++;
        else fault(report, "INVALID_MESH_INDEX", name, p, point_count);
    }
    for (uint32_t p = 0; p < point_count; p++) {
        at_most(report, "NORMALS_EXCEED_LIMIT", name, met[p], FACES_AT_POINT_MAX);
        if (!skeleton && met[p] > NORMALS_AT_POINT_MAX && met[p] <= FACES_AT_POINT_MAX)
            at_most(report, "NORMALS_EXCEED_LIMIT", name, groups_at(points, point_count, faces, face_count, p, crease), NORMALS_AT_POINT_MAX);
    }
    if (!skeleton && is_true(g, o->values, NAME_PLANTED)) at_most(report, "PLANTED_POINTS_EXCEED_LIMIT", name, point_count, PLANTED_POINTS_MAX);
    free(met);
    return 1;
}

/* A skeleton's joints are objects of their own, named after it; what they link are points of its mesh. */
static int joint_named(const jpt_groff_t* g, const map_t* names, const char* name, int joint, uint32_t* index) {
    const size_t n = strlen(name);
    char text[JPT_GROFF_TEXT_MAX];
    const int cut = n > 3 && !strcmp(name + n - 3, "-00") ? 3 : 0;
    if (n > OBJECT_NAME_MAX) return 0;
    snprintf(text, sizeof text, "$J%.*s%02d", (int)(n - cut), name, joint);
    *index = object_named(g, names, text);
    return *index != UINT32_MAX;
}

static void check_links(const jpt_groff_t* g, const record_t* joint, uint32_t points, const char* name, jpt_groff_report_t* report) {
    if (!joint || joint->type != VALUE_OBJECT) return;
    for (uint32_t i = 0; i < joint->child_count; i++) {
        const record_t* child = record_of(g, rd(joint->v.children + (size_t)i * 8));
        if (child && child->name == NAME_LINK && child->type == VALUE_INT && (child->v.i < 0 || (uint32_t)child->v.i >= points))
            fault(report, "INVALID_SKELETON_INDEX", name, (uint32_t)child->v.i, points);
    }
}

static void check_skeleton(const jpt_groff_t* g, const object_t* o, const map_t* names, uint32_t points, const char* name, jpt_groff_report_t* report) {
    const int joints = whole(g, o->values, NAME_JOINTS);
    at_most(report, "SKELETON_EXCEEDS_LIMIT", name, (uint32_t)joints, JOINTS_MAX);
    for (int k = 0; k < joints && k < (int)JOINTS_MAX; k++) {
        uint32_t index;
        if (joint_named(g, names, name, k, &index)) check_links(g, object_at(g, index).values, points, name, report);
    }
    for (uint32_t i = 0; o->values && o->values->type == VALUE_OBJECT && i < o->values->child_count; i++) {
        const record_t* child = record_of(g, rd(o->values->v.children + (size_t)i * 8));
        const uint32_t at = child && child->name == NAME_LINK && child->type == VALUE_STRING ? object_named(g, names, child->v.s) : UINT32_MAX;
        const uint32_t count = at == UINT32_MAX ? 0 : points_of(g, at);
        if (count) at_most(report, "SUBSTITUTE_EXCEEDS_LIMIT", child->v.s, count, points < SUBSTITUTE_POINTS_MAX ? points : SUBSTITUTE_POINTS_MAX);
    }
}

static uint32_t root_of(uint32_t* parent, uint32_t i) {
    while (parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
}

/* The k-th submodel of a compound; NULL: it has no more. */
static const record_t* submodel(const jpt_groff_t* g, const record_t* values, uint32_t k) {
    const record_t* r = k < MODEL_NAMES && !strcmp(text_property(g, values, NAME_TYPE), "Compound") ? property_from(g, values, NAME_MODEL + (int)k, k) : NULL;
    return is_text(r) ? r : NULL;
}

static uint32_t boxes_of(const jpt_groff_t* g, uint32_t index) {
    const object_t o = object_at(g, index);
    uint32_t models = 0;
    while (submodel(g, o.values, models)) models++;
    return models ? models : 1;
}

/* What magnets hold together moves as one, in arrays of ten boxes. */
static int check_magnets(const jpt_groff_t* g, const map_t* names, jpt_groff_report_t* report) {
    uint32_t* parent = (uint32_t*)jpt_alloc(g->object_count, 2 * sizeof *parent);
    uint32_t* boxes = parent ? parent + g->object_count : NULL;
    if (!parent) return 0;
    for (uint32_t i = 0; i < g->object_count; i++) parent[i] = i, boxes[i] = 0;
    for (uint32_t i = 0; i < g->object_count; i++) {
        const object_t o = object_at(g, i);
        uint32_t ends[2];
        if (strcmp(text_property(g, o.values, NAME_CLASS), "CMagnet")) continue;
        ends[0] = object_named(g, names, text_property(g, o.values, NAME_MASTER));
        ends[1] = object_named(g, names, text_property(g, o.values, NAME_SLAVE));
        for (int k = 0; k < 2; k++)
            if (ends[k] != UINT32_MAX) boxes[ends[k]] = 1;
        if (ends[0] != UINT32_MAX && ends[1] != UINT32_MAX) parent[root_of(parent, ends[0])] = root_of(parent, ends[1]);
    }
    for (uint32_t i = 0; i < g->object_count; i++) {
        const uint32_t root = root_of(parent, i);
        if (!boxes[i]) continue;
        boxes[i] = root == i ? boxes[i] - 1 : 0;
        boxes[root] += boxes_of(g, i);
    }
    for (uint32_t i = 0; i < g->object_count; i++)
        if (parent[i] == i) at_most(report, "MAGNET_GROUP_EXCEEDS_LIMIT", object_name(g, i), boxes[i], MAGNET_BOXES_MAX);
    free(parent);
    return 1;
}

static const section_t* material_of(const jpt_groff_t* g, const object_t* o);

/* The engine numbers the surfaces of a material from 1; 0 is all of them. */
static void check_animated_surface(const jpt_groff_t* g, const object_t* o, const char* name, jpt_groff_report_t* report) {
    const record_t* r = property(g, o->values, NAME_ANIMATED_SURFACE);
    const section_t* material = r && r->type == VALUE_INT ? material_of(g, o) : NULL;
    const uint32_t surfaces = material && material->size >= 4 ? rd(g->bytes + material->offset) : 0;
    const char* type = text_property(g, o->values, NAME_CLASS);
    const size_t n = strlen(type);
    if (n > 7 && !strcmp(type + n - 7, "Trigger")) return;
    if (material && (r->v.i < 0 || (uint32_t)r->v.i > surfaces)) fault(report, "INVALID_SURFACE_INDEX", name, (uint32_t)r->v.i, surfaces);
}

static void check_properties(const jpt_groff_t* g, const object_t* o, const map_t* names, const char* name, jpt_groff_report_t* report) {
    uint32_t actions = 0, models = 0;
    const record_t* r;
    at_most(report, "NAME_EXCEEDS_LIMIT", name, (uint32_t)strlen(name), OBJECT_NAME_MAX);
    if (!o->values || o->values->type != VALUE_OBJECT) return;
    check_expression(text_property(g, o->values, NAME_EXPRESSION), name, report);
    check_animated_surface(g, o, name, report);
    for (uint32_t i = 0; i < o->values->child_count; i++) {
        const record_t* child = record_of(g, rd(o->values->v.children + (size_t)i * 8));
        if (!child) continue;
        actions += child->name == NAME_TRIGGER_ACTION;
        if (child->name == NAME_DINOSAUR && child->type == VALUE_INT) report->more_dinosaurs += child->v.i >= OWN_DINOSAURS;
    }
    if (!shares_shape(g, o->values)) {
        for (; !is_text(property(g, o->values, NAME_PHYSICS)) && (r = submodel(g, o->values, models)) != NULL; models++)
            if (!object_exactly(g, names, r->v.s)) fault(report, "INVALID_MODEL_REFERENCE", r->v.s, 0, 0);
        for (int other = NAME_MESH; other <= NAME_PHYSICS; other++)
            if (is_text(r = property(g, o->values, other)) && !object_exactly(g, names, r->v.s)) fault(report, "INVALID_MODEL_REFERENCE", r->v.s, 0, 0);
    }
    at_most(report, "TRIGGER_ACTIONS_EXCEED_LIMIT", name, actions, ACTIONS_MAX);
    at_most(report, "SUBMODELS_EXCEED_LIMIT", name, models, MODELS_MAX);
}

static int mesh_is_known(const jpt_groff_t* g, const object_t* o) {
    object_t shown = *o;
    mesh_t mesh;
    uint32_t faces;
    shown.values = NULL;
    return plain_faces(g, o->geometry, &faces) || mesh_of(g, &shown, &mesh);
}

/* Objects whose names agree up to the first '-' have the mesh of the one that is read first. */
static int earlier_has_mesh(const jpt_groff_t* g, const object_t* o, const char* name) {
    const size_t n = strcspn(name, "-");
    for (uint32_t i = 0; i < g->object_count; i++) {
        const object_t other = object_at(g, i);
        uint32_t faces;
        if (other.region == o->region) break;
        if (!_strnicmp(object_name(g, i), name, n) && strcspn(object_name(g, i), "-") == n && other.geometry && plain_faces(g, other.geometry, &faces)) return 1;
    }
    return 0;
}

/* 0: out of memory. */
static int describe_object(const jpt_groff_t* g, const object_t* o, const map_t* names, map_t* seen, jpt_groff_report_t* report) {
    const char* name = symbol_of(g, rd(o->region + 4));
    const char* type = text_property(g, o->values, NAME_CLASS);
    const size_t n = strlen(type);
    const int skeleton = whole(g, o->values, NAME_JOINTS) > 0;
    sizes_t sizes;
    mesh_t mesh;
    report->animals += !strcmp(type, "CAnimal");
    report->triggers += n > 7 && !strcmp(type + n - 7, "Trigger");
    describe_scripts(g, o->values, name, report);
    if (!strcmp(type, "TerrainPlacement") && !report->terrains++) copy(report->terrain, text_property(g, o->values, NAME_FILE));
    memset(&sizes, 0, sizeof sizes);
    sizes_of(g, o, &sizes);
    check_properties(g, o, names, name, report);
    if (skeleton && sizes.points) check_skeleton(g, o, names, sizes.points, name, report);
    if (o->geometry && !is_false(g, o->values, NAME_VISIBLE) && !mesh_is_known(g, o)) fault(report, "MALFORMED_MESH", name, o->geometry->size, 0);
    else if (o->geometry && skeleton && !sizes.points && !earlier_has_mesh(g, o, name)) fault(report, "MALFORMED_MESH", name, o->geometry->size, 0);
    if (!sizes.points && !sizes.faces) return 1;
    if (map_get(seen, o->geometry->handle) != UINT32_MAX) return 1;
    map_put(seen, o->geometry->handle, 0);
    report->meshes++;
    report->points += sizes.points;
    report->vertices += sizes.vertices;
    report->faces += sizes.faces;
    if (mesh_of(g, o, &mesh)) check_mesh(g, o, &mesh, name, report);
    else if ((skeleton || !g->optimized) && !check_plain(g, o, skeleton, name, report)) return 0;
    if (!mesh_of(g, o, &mesh)) need_build(report, sizes.faces, 1), need_build(report, sizes.vertices, 4), need_build(report, sizes.points, 4);
    if (!mesh_of(g, o, &mesh)) at_most(report, "MESH_EXCEEDS_LIMIT", name, sizes.faces, JPT_GROFF_MESH_FACES_MAX), report->mesh_bytes += 40ull * sizes.faces + 12ull * sizes.faces + 32ull * sizes.vertices + 12ull * sizes.points + 0x90;
    if (sizes.faces <= report->largest_mesh) return 1;
    report->largest_mesh = sizes.faces;
    copy(report->largest_mesh_object, name);
    return 1;
}

int jpt_groff_describe(const jpt_groff_t* g, jpt_groff_report_t* report) {
    map_t seen, names;
    int ok = 1;
    memset(report, 0, sizeof *report);
    if (!map_make(&seen, g->section_count)) return 0;
    if (!map_make(&names, g->object_count)) return free(seen.slots), 0;
    for (uint32_t i = 0; i < g->object_count; i++) map_put(&names, hash_name(0, symbol_of(g, rd(g->objects + (size_t)i * REGION_OBJECT + 4))), i);
    report->objects = g->object_count;
    report->optimized = g->optimized;
    at_most(report, "OBJECTS_EXCEED_LIMIT", "", g->object_count, JPT_SAVE_SECTION_BYTES_MAX / SCENE_RECORD);
    for (uint32_t i = 0; ok && i < g->object_count; i++) {
        const object_t o = object_at(g, i);
        ok = describe_object(g, &o, &names, &seen, report);
    }
    ok = ok && check_magnets(g, &names, report);
    free(seen.slots);
    free(names.slots);
    if (!ok) return 0;
    for (uint32_t i = 0; i < g->record_count; i++) {
        const record_t* r = &g->records[i];
        if (r->type == VALUE_INT && r->name == NAME_ACTION) add_action(report, r->v.i);
        if (r->type == VALUE_OBJECT && whole(g, r, NAME_ACTION) == JPT_GROFF_ACTION_LOAD_LEVEL) add_level(report, text_property(g, r, NAME_LEVEL));
        if (r->type == VALUE_OBJECT && whole(g, r, NAME_ACTION) == JPT_GROFF_ACTION_MAP) add_map(report, text_property(g, r, NAME_SAMPLE));
    }
    qsort(report->actions, report->action_count, sizeof *report->actions, by_type);
    return 1;
}

static int by_hash(const void* a, const void* b) {
    const uint64_t x = ((const chunk_t*)a)->hash, y = ((const chunk_t*)b)->hash;
    return (x > y) - (x < y);
}

jpt_texture_set_t* jpt_texture_set_open(const uint8_t* bytes, size_t size) {
    jpt_texture_set_t* s;
    uint32_t first, count;
    if (size < PID_HEAD) return NULL;
    first = rd(bytes + 8);
    count = rd(bytes + 12);
    if (first > size || count > (size - first) / PID_ENTRY) return NULL;
    s = (jpt_texture_set_t*)jpt_alloc_zero(1, sizeof *s);
    if (!s || (s->chunks = (chunk_t*)jpt_alloc(count, sizeof *s->chunks)) == NULL) {
        free(s);
        return NULL;
    }
    s->bytes = bytes;
    for (s->count = 0; s->count < count; s->count++) {
        const uint8_t* entry = bytes + first + (size_t)s->count * PID_ENTRY;
        if (rd(entry) != PID_ENTRY) {
            jpt_texture_set_close(s);
            return NULL;
        }
        s->chunks[s->count].hash = rd64(entry + PID_HASH);
        s->chunks[s->count].entry = entry;
    }
    qsort(s->chunks, s->count, sizeof *s->chunks, by_hash);
    return s;
}

void jpt_texture_set_close(jpt_texture_set_t* s) {
    if (!s) return;
    free(s->chunks);
    free(s->models.slots);
    free(s);
}

static uint64_t entry_end(const uint8_t* e) {
    const uint64_t height = rd(e + 12);
    return rd(e + 4) + (height ? height - 1 : 0) * rd(e + 16) + ((uint64_t)rd(e + 8) * rd(e + 20) + 7) / 8;
}

uint64_t jpt_texture_set_bytes(const jpt_texture_set_t* s) {
    uint64_t bytes = rd(s->bytes + PID_FIRST_PAGES);
    for (uint32_t i = 0; i < s->count; i++)
        if (entry_end(s->chunks[i].entry) > bytes) bytes = entry_end(s->chunks[i].entry);
    return bytes;
}

int jpt_texture_set_fault(const jpt_texture_set_t* s, jpt_groff_fault_t* out) {
    const uint32_t palettes = rd(s->bytes + PID_PALETTES);
    jpt_groff_report_t report;
    memset(&report, 0, sizeof report);
    at_most(&report, "TEXTURE_POOL_EXCEEDS_LIMIT", "", rd(s->bytes + PID_FIRST_PAGES), POOL_BYTES);
    for (uint32_t i = 0; i < s->count; i++) {
        const uint8_t* e = s->chunks[i].entry;
        const uint32_t palette = rd(e + 40);
        const uint64_t end = entry_end(e);
        if (palette != NO_PALETTE && palette >= palettes) fault(&report, "INVALID_PALETTE_INDEX", "", palette, palettes);
        if (end > POOL_BYTES) fault(&report, "TEXTURE_POOL_EXCEEDS_LIMIT", "", end > UINT32_MAX ? UINT32_MAX : (uint32_t)end, POOL_BYTES);
    }
    if (report.fault_count) *out = report.faults[0];
    return report.fault_count != 0;
}

unsigned jpt_texture_set_count(const jpt_texture_set_t* s) { return s ? s->count : 0; }

static const uint8_t* find_entry(const jpt_texture_set_t* s, uint64_t hash) {
    uint32_t lo = 0, hi = s ? s->count : 0;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (s->chunks[mid].hash < hash) lo = mid + 1;
        else hi = mid;
    }
    return s && lo < s->count && s->chunks[lo].hash == hash ? s->chunks[lo].entry : NULL;
}

static int is_bump(const uint8_t* entry) { return rd(entry + 32) != 0; }

/* A bump map kept only as the parent of curved ones has no pixels in the swap file. */
static int has_picture(const jpt_texture_set_t* s, const uint8_t* entry) {
    const uint32_t hash = rd(entry + PID_HASH);
    if (!is_bump(entry) || rd(entry + 24) != NO_PICTURE) return 1;
    for (uint64_t mip = 1; mip < 12; mip++)
        if (find_entry(s, mip << 32 | hash)) return 1;
    return 0;
}

static uint32_t texture_hash(const char* name, const char* bump, float diffuse) {
    uint8_t raw[4];
    uint32_t bits;
    memcpy(&bits, &diffuse, sizeof bits);
    wr(raw, bits);
    return jpt_crc32(hash_name(hash_name(0, name), bump), raw, sizeof raw);
}

static float diffuse_of(const jpt_groff_t* g, const object_t* o, uint32_t surface) {
    const record_t* r = surface < 99 ? property(g, o->values, (int)(NAME_SURFACE + surface + 1)) : NULL;
    r = property(g, r && r->type == VALUE_OBJECT ? r : o->values, NAME_DIFFUSE);
    return r && r->type == VALUE_FLOAT ? r->v.f : r && r->type == VALUE_INT ? (float)r->v.i : DEFAULT_DIFFUSE;
}

typedef struct {
    const char *name, *bump;
    uint32_t hash;
} surface_t;

static int surface_of(const jpt_groff_t* g, const object_t* o, const section_t* material, uint32_t index, surface_t* out) {
    const uint8_t* p = g->bytes + material->offset;
    const uint32_t count = material->size >= 4 ? rd(p) : 0;
    if (index >= count || count > (material->size - 4) / 12) return 0;
    out->name = symbol_of(g, rd(p + 4 + (size_t)index * 4));
    out->bump = is_false(g, o->values, NAME_BUMPMAPS) ? "" : symbol_of(g, rd(p + 4 + (size_t)(2 * count + index) * 4));
    if (!out->name[0]) return 0;
    out->hash = texture_hash(out->name, out->bump, diffuse_of(g, o, index));
    return 1;
}

typedef struct {
    const uint8_t *points, *faces, *corners, *surfaces;
    uint32_t point_count, face_count, corner_count, textured;
} model_t;

/* A mesh the engine builds itself. */
static int model_of(const jpt_groff_t* g, const object_t* o, model_t* m) {
    const section_t* mapping = section_of(g, o->mapping);
    const uint8_t* p;
    if (!o->geometry || !mapping || mapping->size < 12 || !plain_faces(g, o->geometry, &m->face_count)) return 0;
    p = g->bytes + o->geometry->offset;
    m->point_count = rd(p + 12);
    m->points = p + GEOMETRY_HEAD;
    m->faces = m->points + (size_t)m->point_count * 12;
    p = g->bytes + mapping->offset;
    m->corner_count = rd(p + 4);
    m->textured = rd(p + 8);
    if (12 + (uint64_t)m->corner_count * 8 + (uint64_t)m->textured * 16 > mapping->size) return 0;
    m->corners = p + 12;
    m->surfaces = m->corners + (size_t)m->corner_count * 8 + (size_t)m->textured * 12;
    return 1;
}

static const section_t* material_of(const jpt_groff_t* g, const object_t* o) {
    mesh_t mesh;
    const section_t* mapping;
    if (mesh_of(g, o, &mesh)) return section_of(g, mesh.material);
    mapping = section_of(g, o->mapping);
    return mapping && mapping->size >= 4 ? section_of(g, rd(g->bytes + mapping->offset)) : NULL;
}

static uint32_t hundredths(double value) {
    const double scaled = value * 100.0;
    return scaled >= -2147483648.0 && scaled < 2147483648.0 ? (uint32_t)(int32_t)scaled : 0x80000000u;
}

/* The engine's key of a polygon's curved bump map; 0 when the polygon is malformed. */
static int curved_key(const mesh_t* m, uint32_t index, uint32_t texture, uint32_t width, uint32_t height, uint32_t* key) {
    const uint8_t* polygon = m->polygons + (size_t)index * MESH_POLYGON;
    const uint32_t corners = rd(polygon), first = rd(polygon + 4);
    double lowest[2] = { 0, 0 }, highest[2] = { 0, 0 };
    uint8_t data[64];
    if (corners < 3 || first > m->pointer_count || corners > m->pointer_count - first) return 0;
    for (uint32_t i = 0; i < corners; i++) {
        const uint32_t vertex = rd(m->pointers + (size_t)(first + i) * 4);
        const uint8_t* v;
        if (vertex >= m->vertex_count) return 0;
        v = m->vertices + (size_t)vertex * MESH_VERTEX;
        for (int axis = 0; axis < 2; axis++) {
            const double at = (double)rdf(v + 16 + axis * 4) * (axis ? height : width);
            if (!i || at < lowest[axis]) lowest[axis] = at;
            if (!i || at > highest[axis]) highest[axis] = at;
        }
        if (i >= 3) continue;
        if (rd(v) >= m->point_count) return 0;
        for (int axis = 0; axis < 3; axis++) wr(data + 28 + i * 12 + axis * 4, hundredths(rdf(m->points + (size_t)rd(v) * MESH_POINT + axis * 4)));
    }
    wr(data, texture);
    wr(data + 4, hundredths(lowest[0]));
    wr(data + 8, hundredths(lowest[1]));
    wr(data + 12, hundredths(highest[0]));
    wr(data + 16, hundredths(highest[1]));
    wr(data + 20, (index + 1) * texture);
    wr(data + 24, corners * texture);
    *key = jpt_crc32(0, data, sizeof data);
    return 1;
}

static const record_t* curved_property(const jpt_groff_t* g, const object_t* o) {
    const record_t* r = property(g, o->values, NAME_CURVED);
    return r && r->type == VALUE_BOOL && r->v.i ? r : NULL;
}

/* Faces of one texture on models of one shape have the same curved bump maps. */
static uint32_t shape_of(const model_t* m) {
    uint32_t crc = jpt_crc32(0, m->points, (size_t)m->point_count * 12);
    crc = jpt_crc32(crc, m->faces, (size_t)m->face_count * 12);
    return jpt_crc32(crc, m->corners, (size_t)m->corner_count * 8 + (size_t)m->textured * 16);
}

static uint32_t maps_key(uint32_t shape, uint32_t texture) {
    uint8_t raw[4];
    wr(raw, texture);
    return jpt_crc32(shape, raw, sizeof raw);
}

int jpt_texture_set_borrow(jpt_texture_set_t* set, const jpt_groff_t* original) {
    const uint32_t count = original ? original->object_count : 0;
    free(set->models.slots);
    set->models.slots = NULL;
    set->borrowed = map_make(&set->models, count * SURFACES_OF_MODEL);
    for (uint32_t i = 0; set->borrowed && i < count; i++) {
        const object_t o = object_at(original, i);
        const section_t* material = curved_property(original, &o) ? material_of(original, &o) : NULL;
        surface_t s;
        model_t m;
        if (!material || !model_of(original, &o, &m)) continue;
        for (uint32_t k = 0; k < SURFACES_OF_MODEL; k++)
            if (surface_of(original, &o, material, k, &s)) map_put(&set->models, maps_key(shape_of(&m), s.hash), i);
    }
    return set->borrowed;
}

static int model_is_made(const jpt_texture_set_t* set, uint32_t shape, uint32_t texture) {
    return !set->borrowed || map_get(&set->models, maps_key(shape, texture)) != UINT32_MAX;
}

typedef enum { CURVED_NOT, CURVED_IN_SET, CURVED_MISSING } curved_t;

static curved_t curved_state(const jpt_texture_set_t* set, const mesh_t* m, uint32_t index, const surface_t* surface, const uint8_t* entry) {
    const uint8_t* polygon = m->polygons + (size_t)index * MESH_POLYGON;
    uint32_t key;
    if (!(rd(polygon + POLYGON_FLAGS) & POLYGON_CURVED) || !entry || !is_bump(entry)) return CURVED_NOT;
    if (!curved_key(m, index, surface->hash, rd(entry + 8), rd(entry + 12), &key)) return CURVED_NOT;
    return find_entry(set, key) ? CURVED_IN_SET : CURVED_MISSING;
}

static int add_texture(jpt_groff_textures_t* out, uint32_t* cap, const surface_t* s, map_t* seen, uint32_t* index) {
    const uint32_t known = map_get(seen, s->hash);
    jpt_groff_texture_t* item;
    if (known != UINT32_MAX) {
        *index = known;
        return 1;
    }
    if (out->count == *cap) {
        map_t larger;
        jpt_groff_texture_t* grown = (jpt_groff_texture_t*)jpt_alloc_again(out->items, (size_t)*cap * 2, sizeof *grown);
        if (!grown) return 0;
        out->items = grown;
        if (!map_make(&larger, *cap * 2)) return 0;
        *cap *= 2;
        for (uint32_t i = 0; i < out->count; i++) map_put(&larger, out->items[i].hash, i);
        free(seen->slots);
        *seen = larger;
    }
    item = &out->items[out->count];
    memset(item, 0, sizeof *item);
    copy(item->name, s->name);
    copy(item->bump, s->bump);
    item->hash = s->hash;
    map_put(seen, s->hash, out->count);
    *index = out->count++;
    return 1;
}

static int by_texture(const void* a, const void* b) {
    const jpt_groff_texture_t *x = (const jpt_groff_texture_t*)a, *y = (const jpt_groff_texture_t*)b;
    const int order = _stricmp(x->name, y->name);
    return order ? order : (x->hash > y->hash) - (x->hash < y->hash);
}

static void add_model(const jpt_groff_t* g, const jpt_texture_set_t* set, const object_t* o, const section_t* material, jpt_groff_textures_t* out, const map_t* seen) {
    model_t m;
    surface_t s;
    uint32_t shape;
    if (!set || !curved_property(g, o) || !model_of(g, o, &m)) return;
    shape = shape_of(&m);
    for (uint32_t i = 0; i < m.textured; i++) {
        const uint8_t* entry;
        uint32_t at;
        if (!surface_of(g, o, material, rd(m.surfaces + (size_t)i * 4), &s) || (entry = find_entry(set, s.hash)) == NULL || !is_bump(entry)) continue;
        if ((at = map_get(seen, s.hash)) == UINT32_MAX) continue;
        out->items[at].curved++;
        out->items[at].curved_missing += !model_is_made(set, shape, s.hash);
    }
}

/* Whether a polygon of the object has the surface. */
static int surface_drawn(const jpt_groff_t* g, const object_t* o, uint32_t surface) {
    mesh_t mesh;
    model_t model;
    if (mesh_of(g, o, &mesh)) {
        for (uint32_t i = 0; i < mesh.polygon_count; i++)
            if (rd(mesh.polygons + (size_t)i * MESH_POLYGON + POLYGON_SURFACE) == surface) return 1;
        return 0;
    }
    if (!model_of(g, o, &model)) return 1;
    for (uint32_t i = 0; i < model.textured; i++)
        if (rd(model.surfaces + (size_t)i * 4) == surface) return 1;
    return 0;
}

static void draw_texture(const jpt_groff_t* g, uint32_t index, jpt_groff_texture_t* item) {
    if (!item->drawn) copy(item->object, object_name(g, index));
    item->drawn = 1;
}

/* The frames of an animating mesh are named without a folder. */
static int add_frames(const jpt_groff_t* g, const jpt_texture_set_t* set, uint32_t index, jpt_groff_textures_t* out, uint32_t* cap, map_t* seen) {
    const object_t object = object_at(g, index), *o = &object;
    const record_t* diffuse = property(g, o->values, NAME_DIFFUSE);
    const float f = diffuse && diffuse->type == VALUE_FLOAT ? diffuse->v.f : diffuse && diffuse->type == VALUE_INT ? (float)diffuse->v.i : DEFAULT_DIFFUSE;
    for (int i = 0; i < FRAMES_MAX; i++) {
        char name[2 * JPT_GROFF_TEXT_MAX], bump[2 * JPT_GROFF_TEXT_MAX] = "";
        const char *frame = text_property(g, o->values, NAME_FRAME + i), *map = text_property(g, o->values, NAME_FRAME_BUMP + i);
        surface_t s = { name, bump, 0 };
        const uint8_t* entry;
        uint32_t at;
        if (!frame[0]) break;
        snprintf(name, sizeof name, "%s%.*s", g->folder, JPT_GROFF_TEXT_MAX, frame);
        if (map[0]) snprintf(bump, sizeof bump, "%s%.*s", g->folder, JPT_GROFF_TEXT_MAX, map);
        s.hash = texture_hash(name, bump, f);
        if (!add_texture(out, cap, &s, seen, &at)) return 0;
        draw_texture(g, index, &out->items[at]);
        entry = find_entry(set, s.hash);
        out->items[at].in_set = entry != NULL;
        out->items[at].picture = entry && has_picture(set, entry);
    }
    return 1;
}

static int power_of_two(uint32_t n) { return n && !(n & (n - 1u)); }

/* The engine loads a '$' object only for a value that names it. */
static int is_named(const jpt_groff_t* g, const map_t* names, const char* name) {
    const uint32_t i = map_get(names, hash_name(0, name));
    return i != UINT32_MAX && !_stricmp(g->records[i].v.s, name);
}

static int names_of(const jpt_groff_t* g, map_t* names) {
    if (!map_make(names, g->record_count)) return 0;
    for (uint32_t i = 0; i < g->record_count; i++)
        if (g->records[i].type == VALUE_STRING && g->records[i].v.s[0] == '$') map_put(names, hash_name(0, g->records[i].v.s), i);
    return 1;
}

static int add_object(const jpt_groff_t* g, const jpt_texture_set_t* set, uint32_t index, const map_t* names, jpt_groff_textures_t* out, uint32_t* cap, map_t* seen) {
    const object_t o = object_at(g, index);
    const section_t* material = is_false(g, o.values, NAME_VISIBLE) ? NULL : material_of(g, &o);
    const uint32_t surfaces = material && material->size >= 4 ? rd(g->bytes + material->offset) : 0;
    const int hidden = !strcmp(text_property(g, o.values, NAME_CLASS), "CMagnet") || (object_name(g, index)[0] == '$' && !is_named(g, names, object_name(g, index)));
    mesh_t mesh;
    surface_t s;
    uint32_t at;
    int bumped = 0;
    if (!material) return 1;
    for (uint32_t i = 0; i < surfaces && i < material->size / 12; i++) bumped |= surface_of(g, &o, material, i, &s) && s.bump[0];
    for (uint32_t i = 0; i < surfaces && i < material->size / 12; i++) {
        const uint8_t* entry;
        if (!surface_of(g, &o, material, i, &s)) continue;
        if (!add_texture(out, cap, &s, seen, &at)) return 0;
        entry = find_entry(set, s.hash);
        out->items[at].bumped |= bumped;
        if (!out->items[at].drawn && !hidden && surface_drawn(g, &o, i)) draw_texture(g, index, &out->items[at]);
        out->items[at].odd_size = entry && (!power_of_two(rd(entry + PID_WIDTH)) || !power_of_two(rd(entry + PID_HEIGHT)));
        out->items[at].in_set = entry != NULL;
        out->items[at].picture = entry && has_picture(set, entry);
    }
    if (!add_frames(g, set, index, out, cap, seen)) return 0;
    if (!mesh_of(g, &o, &mesh)) {
        add_model(g, set, &o, material, out, seen);
        return 1;
    }
    for (uint32_t i = 0; i < mesh.polygon_count; i++) {
        const uint32_t surface = rd(mesh.polygons + (size_t)i * MESH_POLYGON + POLYGON_SURFACE);
        curved_t state;
        if (!(rd(mesh.polygons + (size_t)i * MESH_POLYGON + POLYGON_FLAGS) & POLYGON_CURVED) || !surface_of(g, &o, material, surface, &s)) continue;
        state = curved_state(set, &mesh, i, &s, find_entry(set, s.hash));
        if (state == CURVED_NOT || (at = map_get(seen, s.hash)) == UINT32_MAX) continue;
        out->items[at].curved++;
        out->items[at].curved_missing += state == CURVED_MISSING;
    }
    return 1;
}

int jpt_groff_textures(const jpt_groff_t* g, const jpt_texture_set_t* set, jpt_groff_textures_t* out) {
    uint32_t cap = 256;
    map_t seen = { NULL, 0 }, names = { NULL, 0 };
    int ok;
    memset(out, 0, sizeof *out);
    out->items = (jpt_groff_texture_t*)jpt_alloc(cap, sizeof *out->items);
    ok = out->items && map_make(&seen, cap) && names_of(g, &names);
    for (uint32_t i = 0; ok && i < g->object_count; i++) ok = add_object(g, set, i, &names, out, &cap, &seen);
    free(seen.slots);
    free(names.slots);
    if (!ok) {
        jpt_groff_textures_free(out);
        return 0;
    }
    qsort(out->items, out->count, sizeof *out->items, by_texture);
    for (unsigned i = 0; i < out->count; i++) {
        out->curved += out->items[i].curved;
        out->curved_missing += out->items[i].curved_missing;
    }
    return 1;
}

void jpt_groff_textures_free(jpt_groff_textures_t* textures) {
    free(textures->items);
    memset(textures, 0, sizeof *textures);
}

static unsigned flatten_mesh(const jpt_groff_t* g, const jpt_texture_set_t* set, const object_t* o, const mesh_t* mesh, uint8_t* world) {
    const section_t* material = section_of(g, mesh->material);
    unsigned changed = 0;
    for (uint32_t i = 0; material && i < mesh->polygon_count; i++) {
        const size_t at = (size_t)(mesh->polygons - g->bytes) + (size_t)i * MESH_POLYGON;
        const uint8_t* entry;
        surface_t s;
        if (!(rd(g->bytes + at + POLYGON_FLAGS) & POLYGON_CURVED) || !surface_of(g, o, material, rd(g->bytes + at + POLYGON_SURFACE), &s)) continue;
        entry = find_entry(set, s.hash);
        if (curved_state(set, mesh, i, &s, entry) != CURVED_MISSING || !has_picture(set, entry)) continue;
        wr(world + at + POLYGON_FLAGS, rd(world + at + POLYGON_FLAGS) & ~POLYGON_CURVED);
        changed++;
    }
    return changed;
}

static unsigned flatten_model(const jpt_groff_t* g, const jpt_texture_set_t* set, const object_t* o, uint8_t* world) {
    const section_t* material = is_false(g, o->values, NAME_VISIBLE) ? NULL : material_of(g, o);
    const record_t* curved = curved_property(g, o);
    unsigned faces = 0;
    uint32_t shape;
    surface_t s;
    model_t m;
    if (!material || !curved || !set->borrowed || !model_of(g, o, &m)) return 0;
    shape = shape_of(&m);
    for (uint32_t i = 0; i < m.textured; i++) {
        const uint8_t* entry = surface_of(g, o, material, rd(m.surfaces + (size_t)i * 4), &s) ? find_entry(set, s.hash) : NULL;
        faces += entry && is_bump(entry) && has_picture(set, entry) && !model_is_made(set, shape, s.hash);
    }
    if (faces) world[curved->at] = 0;
    return faces;
}

unsigned jpt_groff_flatten_curved(const jpt_groff_t* g, const jpt_texture_set_t* set, uint8_t* world) {
    unsigned changed = 0;
    for (uint32_t i = 0; i < g->object_count; i++) {
        const object_t o = object_at(g, i);
        mesh_t mesh;
        changed += mesh_of(g, &o, &mesh) ? flatten_mesh(g, set, &o, &mesh, world) : flatten_model(g, set, &o, world);
    }
    return changed;
}

int jpt_groff_hide_unusable(const jpt_groff_t* g, const jpt_texture_set_t* set, uint8_t* pid, unsigned* hidden) {
    jpt_groff_textures_t textures;
    *hidden = 0;
    if (!jpt_groff_textures(g, set, &textures)) return 0;
    for (unsigned i = 0; i < textures.count; i++) {
        const jpt_groff_texture_t* t = &textures.items[i];
        if (!t->in_set || t->picture || !t->curved_missing) continue;
        wr(pid + (find_entry(set, t->hash) - set->bytes) + PID_HASH + 4, UNUSED_MIP);
        ++*hidden;
    }
    jpt_groff_textures_free(&textures);
    return 1;
}

unsigned jpt_groff_object_count(const jpt_groff_t* g) { return g->object_count; }

void jpt_groff_object(const jpt_groff_t* g, unsigned index, jpt_groff_object_t* out) {
    const uint8_t* r = g->objects + (size_t)index * REGION_OBJECT;
    const record_t* values = record_of(g, rd(r + 36));
    out->name = object_name(g, index);
    out->has_values = values && values->type == VALUE_OBJECT;
    out->type = out->has_values ? text_property(g, values, NAME_CLASS) : "";
    for (int k = 0; k < 3; k++) {
        out->position[k] = rdf(r + 8 + 4 * k);
        out->rotation[k] = rdf(r + 20 + 4 * k);
    }
    out->scale = rdf(r + 32);
    out->values = out->has_values ? values : NULL;
}

int jpt_groff_value_at(const jpt_groff_t* g, const void* group, unsigned index, jpt_groff_value_t* out) {
    const record_t* parent = (const record_t*)group;
    const record_t* r;
    memset(out, 0, sizeof *out);
    if (!parent || parent->type != VALUE_OBJECT || index >= parent->child_count) return 0;
    r = record_of(g, rd(parent->v.children + (size_t)index * 8));
    if (!r) return 1;
    out->key = r->label;
    out->type = r->type;
    if (r->type == VALUE_FLOAT) out->real = r->v.f;
    else if (r->type == VALUE_STRING) out->text = r->v.s;
    else if (r->type == VALUE_OBJECT) out->group = r;
    else out->number = r->v.i;
    return 1;
}

int jpt_groff_value_named(const jpt_groff_t* g, const void* group, const char* key, jpt_groff_value_t* out) {
    jpt_groff_value_t v;
    for (unsigned i = 0; jpt_groff_value_at(g, group, i, &v); i++)
        if (v.key && !strcmp(v.key, key)) {
            *out = v;
            return 1;
        }
    memset(out, 0, sizeof *out);
    return 0;
}

static const uint8_t* geometry_bytes(const jpt_groff_t* g, unsigned index, uint32_t* size) {
    const section_t *s = section_of(g, rd(g->objects + (size_t)index * REGION_OBJECT)), *geometry;
    if (!s || s->size < OBJECT_WORDS * 4 || (geometry = section_of(g, rd(g->bytes + s->offset + 4))) == NULL) return NULL;
    *size = geometry->size;
    return g->bytes + geometry->offset;
}

static float* read_points(const uint8_t* at, uint32_t count) {
    float* out = (float*)jpt_alloc(count ? count : 1, 3 * sizeof *out);
    for (uint32_t i = 0; out && i < 3 * count; i++) out[i] = rdf(at + 4 * (size_t)i);
    return out;
}

static int plain_geometry(const uint8_t* p, uint32_t size) {
    return size >= GEOMETRY_HEAD && GEOMETRY_HEAD + 12 * ((uint64_t)rd(p + 12) + 2 * (uint64_t)rd(p + 16) + rd(p + 20) + rd(p + 24)) == size;
}

int jpt_groff_object_points(const jpt_groff_t* g, unsigned index, float** points, unsigned* count) {
    uint32_t size = 0, n, wrap;
    const uint8_t* p = geometry_bytes(g, index, &size);
    *points = NULL;
    *count = 0;
    if (!p) return 0;
    if (plain_geometry(p, size) && rd(p + 12)) {
        n = rd(p + 12);
        *points = read_points(p + GEOMETRY_HEAD, n);
    } else if (size >= MESH_HEAD) {
        n = rd(p + 24);
        wrap = rd(p + 36);
        if (!n || MESH_HEAD + 12 * ((uint64_t)n + wrap) > size) return 0;
        if (wrap) n = wrap;
        *points = read_points(wrap ? p + size - 12 * (size_t)wrap : p + MESH_HEAD, n);
    } else {
        return 0;
    }
    if (!*points) return 0;
    *count = n;
    return 1;
}

int jpt_groff_object_mesh(const jpt_groff_t* g, unsigned index, float** points, unsigned* count, uint32_t** triangles, unsigned* triangle_count) {
    uint32_t size = 0, n, corners, wrap;
    uint64_t records;
    const uint8_t* p = geometry_bytes(g, index, &size);
    *points = NULL;
    *triangles = NULL;
    *count = *triangle_count = 0;
    if (!p || size < MESH_HEAD || plain_geometry(p, size)) return 0;
    n = rd(p + 24);
    corners = rd(p + 28);
    wrap = rd(p + 36);
    records = MESH_HEAD + 12ull * n;
    if (!n || !corners || corners % 3 || records + 32ull * corners > size || MESH_HEAD + 12 * ((uint64_t)n + wrap) > size) return 0;
    *triangles = (uint32_t*)jpt_alloc(corners, sizeof **triangles);
    if (!*triangles) return 0;
    for (uint32_t i = 0; i < corners; i++)
        if (((*triangles)[i] = rd(p + records + 32ull * i)) >= n) {
            free(*triangles);
            *triangles = NULL;
            return 0;
        }
    *points = read_points(p + MESH_HEAD, n);
    if (!*points) {
        free(*triangles);
        *triangles = NULL;
        return 0;
    }
    *count = n;
    *triangle_count = corners / 3;
    return 1;
}

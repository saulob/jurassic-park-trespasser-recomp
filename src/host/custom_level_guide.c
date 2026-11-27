#include "custom_level_guide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_tr.h"

void host_log(const char* tag, const char* fmt, ...);
int guest_load_string(unsigned id, char* out, int cap);
const char* l10n_text_utf8(unsigned id);

#define GUIDE_BYTES_MAX (8u << 20)
#define TITLE_MAX 120

static struct {
    char level[DM_LEVEL_ID_MAX];
    int decided_map, decided_goals;
    level_guide_t guide;
    int parsed;
    ob_objective_t items[DM_GOALS_MAX];
    char titles[DM_GOALS_MAX][TITLE_MAX];
    ob_level_t table;
    int titled;
} G;

static int same_level(const char* scene, const char* level_id) {
    const char* slash = strrchr(scene, '/');
    const char* back = strrchr(scene, '\\');
    const char* name = back > slash ? back + 1 : slash ? slash + 1 : scene;
    const char* dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name), i;
    if (n != strlen(level_id)) return 0;
    for (i = 0; i < n; i++)
        if (toupper((unsigned char)name[i]) != (unsigned char)level_id[i]) return 0;
    return 1;
}

static char* next_line(char** cursor) {
    char *line = *cursor, *end;
    if (!line || !*line) return NULL;
    end = strchr(line, '\n');
    if (end) {
        *end = 0;
        *cursor = end + 1;
    } else {
        *cursor = line + strlen(line);
    }
    if (end && end > line && end[-1] == '\r') end[-1] = 0;
    return line;
}

static int grow(void** items, int count, size_t size) {
    void* bigger;
    if (count & (count - 1)) return 1;
    bigger = realloc(*items, (size_t)(count ? count * 2 : 1) * size);
    if (!bigger) return 0;
    *items = bigger;
    return 1;
}

int level_guide_parse(const char* text, level_guide_t* out) {
    char *copy = text ? (char*)malloc(strlen(text) + 1) : NULL, *cursor, *line;
    int version = 0, ok = 1, runs = 0, landmarks = 0, vehicles = 0;
    memset(out, 0, sizeof *out);
    if (!copy) return 0;
    strcpy(copy, text);
    cursor = copy;
    line = next_line(&cursor);
    if (!line || sscanf(line, "level-guide %d", &version) != 1 || version != LEVEL_GUIDE_VERSION) {
        free(copy);
        return 0;
    }
    while (ok && (line = next_line(&cursor)) != NULL) {
        char word[16] = "";
        int n = 0;
        if (sscanf(line, "%15s%n", word, &n) != 1) continue;
        if (!strcmp(word, "world")) {
            out->in_game = !strncmp(line + n, " game", 5);
        } else if (!strcmp(word, "map")) {
            int cols, rows;
            if (sscanf(line + n, "%f %f %f %f %d %d", &out->bounds.min_x, &out->bounds.min_y, &out->bounds.max_x, &out->bounds.max_y, &cols, &rows) != 6 ||
                cols < 1 || cols > 0xFFFF || rows < 1 || rows > 256 || !(out->bounds.max_x > out->bounds.min_x) || !(out->bounds.max_y > out->bounds.min_y))
                ok = 0;
            out->map.cols = (uint16_t)cols, out->map.rows = (uint16_t)rows;
            out->has_map = ok;
        } else if (!strcmp(word, "run")) {
            int a, b, c, d;
            if (sscanf(line + n, "%d %d %d %d", &a, &b, &c, &d) != 4 || a < 0 || b < 1 || a + b > out->map.cols || c < 0 || c >= out->map.rows || d < 1 || d > 2 ||
                runs >= 0xFFFF || !grow((void**)&out->runs, runs, sizeof *out->runs))
                ok = 0;
            else out->runs[runs++] = (minimap_run_t){ (uint16_t)a, (uint16_t)b, (uint8_t)c, (uint8_t)d };
        } else if (!strcmp(word, "landmark")) {
            minimap_landmark_t l;
            int kind;
            if (sscanf(line + n, "%d %f %f %f %f %f %f %f %f", &kind, &l.x[0], &l.y[0], &l.x[1], &l.y[1], &l.x[2], &l.y[2], &l.x[3], &l.y[3]) != 9 || kind < 0 ||
                kind > 3 || landmarks >= 0xFFFF || !grow((void**)&out->landmarks, landmarks, sizeof *out->landmarks))
                ok = 0;
            else l.kind = (uint8_t)kind, out->landmarks[landmarks++] = l;
        } else if (!strcmp(word, "vehicle")) {
            minimap_vehicle_t v;
            if (sscanf(line + n, "%f %f %f %f %d", &v.x, &v.y, &v.ax, &v.ay, &v.size) != 5 || v.size < 0 || v.size > 2 || vehicles >= 0xFFFF ||
                !grow((void**)&out->vehicles, vehicles, sizeof *out->vehicles))
                ok = 0;
            else out->vehicles[vehicles++] = v;
        } else if (!strcmp(word, "objective") && out->objective_count < DM_GOALS_MAX) {
            const int i = out->objective_count;
            int used = 0, k = 0;
            char name[64];
            if (sscanf(line + n, "%d%n", &out->hints[i], &used) != 1) {
                ok = 0;
                break;
            }
            for (char* p = line + n + used; k < OB_NAMES_MAX && sscanf(p, "%63s%n", name, &used) == 1; p += used) snprintf(out->targets[i][k++], 64, "%s", name);
            out->objective_count++;
        }
    }
    free(copy);
    out->map.runs = out->runs, out->map.count = (uint16_t)runs;
    out->map.landmarks = out->landmarks, out->map.landmark_count = (uint16_t)landmarks;
    out->map.vehicles = out->vehicles, out->map.vehicle_count = (uint16_t)vehicles;
    out->has_map = out->has_map && runs > 0;
    if (!ok) level_guide_free(out);
    return ok;
}

void level_guide_free(level_guide_t* guide) {
    free(guide->runs);
    free(guide->landmarks);
    free(guide->vehicles);
    memset(guide, 0, sizeof *guide);
}

void level_guide_title(const char* hint_text, char* out, size_t cap) {
    size_t n = 0, end;
    if (!cap) return;
    while (hint_text && isspace((unsigned char)*hint_text)) hint_text++;
    if (!hint_text) hint_text = "";
    end = strlen(hint_text);
    for (size_t i = 0; hint_text[i]; i++)
        if (strchr(".!?", hint_text[i]) && (!hint_text[i + 1] || isspace((unsigned char)hint_text[i + 1]))) {
            end = i + 1;
            break;
        }
    n = end < cap - 1 ? end : cap - 1;
    while (n > 0 && end > n && ((unsigned char)hint_text[n] & 0xC0) == 0x80) n--;
    memcpy(out, hint_text, n);
    out[n] = 0;
    while (n > 0 && isspace((unsigned char)out[n - 1])) out[--n] = 0;
}

static void forget(void) {
    level_guide_free(&G.guide);
    memset(&G, 0, sizeof G);
}

static int read_guide(const char* scene_file) {
    const char* slash = strrchr(scene_file, '/');
    char path[JPT_PATH_MAX], name[JPT_CUSTOM_LEVEL_PATH_MAX];
    uint64_t size = 0;
    jpt_fs_file_t* f;
    char* text;
    size_t i;
    int ok = 0;
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : scene_file);
    for (i = 0; name[i]; i++) name[i] = (char)tolower((unsigned char)name[i]);
    if ((size_t)snprintf(path, sizeof path, "%s/%s/%s.txt", jpt_custom_level_records_dir(), LEVEL_GUIDE_FOLDER, name) >= sizeof path) return 0;
    f = jpt_fs_open_read(path, &size);
    if (!f) return 0;
    text = size < GUIDE_BYTES_MAX ? (char*)malloc((size_t)size + 1) : NULL;
    if (text && jpt_fs_read_at(f, 0, text, (size_t)size)) {
        text[size] = 0;
        ok = level_guide_parse(text, &G.guide);
    }
    free(text);
    jpt_fs_close(f);
    return ok;
}

/* The scene of the package the game names by this id; -1 for a level of the game the package leads to. */
static int package_scene(const char* level_id) {
    for (int i = 0; i < jpt_custom_level_scene_count(); i++)
        if (same_level(jpt_custom_level_scene_at(i), level_id)) return i;
    return -1;
}

static void choose(const char* level_id) {
    char world[DM_LEVEL_ID_MAX + 8], file[JPT_PATH_MAX];
    int scene;
    if (!strcmp(G.level, level_id)) return;
    forget();
    snprintf(G.level, sizeof G.level, "%s", level_id);
    scene = package_scene(level_id);
    if (scene < 0) {
        snprintf(world, sizeof world, "%s.grf", level_id);
        G.decided_map = G.decided_goals = jpt_custom_level_find(world, file, sizeof file);
        host_log("guide", "level %s: %s", level_id, G.decided_map ? "the package has its own world of that name and no guide: no information" : "a level of the game: the campaign's minimap and objectives");
        return;
    }
    G.parsed = read_guide(jpt_custom_level_scene_file_at(scene));
    G.decided_map = !(G.parsed && G.guide.in_game && minimap_retail_bounds(level_id));
    G.decided_goals = !(G.parsed && G.guide.in_game && ob_level(level_id));
    host_log("guide", "level %s: minimap %s, objectives %s", level_id,
             !G.decided_map ? "the campaign's (the game's own world)" : G.guide.has_map ? "from the package's guide" : "none: \"No map information\"",
             !G.decided_goals ? "the campaign's" : G.guide.objective_count ? "from the level's hints and exits" : "none: \"No objective information\"");
}

static void entitle(void) {
    char buffer[DM_LINE_MAX];
    for (int i = 0; i < G.guide.objective_count; i++) {
        const int hint = G.guide.hints[i];
        const char* text = hint >= 0 ? l10n_text_utf8(OB_HINT_STRING_BASE + (unsigned)hint) : NULL;
        if (!text && hint >= 0 && guest_load_string(OB_HINT_STRING_BASE + (unsigned)hint, buffer, (int)sizeof buffer)) text = buffer;
        level_guide_title(text, G.titles[i], TITLE_MAX);
        if (!G.titles[i][0] && hint < 0) snprintf(G.titles[i], TITLE_MAX, "%s", jpt_tr("Find the way out"));
        else if (!G.titles[i][0]) snprintf(G.titles[i], TITLE_MAX, jpt_tr("Objective %d"), i + 1);
        G.items[i].hint = hint;
        G.items[i].title = G.titles[i];
        for (int k = 0; k < OB_NAMES_MAX; k++) G.items[i].target[k] = G.guide.targets[i][k][0] ? G.guide.targets[i][k] : NULL;
        G.items[i].carried = NULL;
        G.items[i].route[0] = NULL;
    }
    G.table.level = G.level;
    G.table.items = G.items;
    G.table.count = G.guide.objective_count;
    G.titled = 1;
}

int level_guide_minimap(const char* level_id, const minimap_bounds_t** bounds, const minimap_level_map_t** map) {
    *bounds = NULL;
    *map = NULL;
    if (!jpt_custom_level_active() || !level_id || !level_id[0]) return 0;
    choose(level_id);
    if (!G.decided_map) return 0;
    if (G.guide.has_map) {
        G.guide.bounds.id = G.level;
        G.guide.map.id = G.level;
        *bounds = &G.guide.bounds;
        *map = &G.guide.map;
    }
    return 1;
}

int level_guide_objectives(const char* level_id, const ob_level_t** table) {
    *table = NULL;
    if (!jpt_custom_level_active() || !level_id || !level_id[0]) return 0;
    choose(level_id);
    if (!G.decided_goals) return 0;
    if (G.guide.objective_count) {
        if (!G.titled) entitle();
        *table = &G.table;
    }
    return 1;
}

static int minimap_source(const char* level_id, const minimap_bounds_t** bounds, const minimap_level_map_t** map) {
    return level_guide_minimap(level_id, bounds, map);
}

void level_guide_install(void) {
    if (!jpt_custom_level_active()) return;
    jpt_custom_level_on_end(forget);
    minimap_set_source(minimap_source);
}

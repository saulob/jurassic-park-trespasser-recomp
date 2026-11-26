#include "level_actions_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SPEED_LEAST 0.05f
#define SPEED_NORMAL_WITHIN 0.001f
#define SLOW_SAME 0.0001f
#define COMMANDS_ALL 0xFFFFFFFFu
#define COMMAND_JUMP 0x00000001u
#define COMMANDS_MOVE 0x1F000000u
#define STATE_MAGIC 0x3254434Cu
#define STATE_MAGIC_FIRST 0x3154434Cu /* ends with the map */
#define UNLIMITED_BELOW (-10.0f)
#define PROBABILITY_UNIT 0x028F5C28u
#define AI_TYPE_SEEN 5
#define AI_TYPE_LAST 7
#define PI 3.14159265358979323846

int la_creates(int type) { return type >= LA_FIRST && type <= LA_LAST && type != 107 && type != 108 && type != 119; }

uint32_t la_scene_bytes(int type, int scene_version) {
    if (scene_version < 25) return 0;
    switch (type) {
    case 100:
    case 101:
    case 103:
    case 104:
    case 105:
    case 106:
    case 113: return 4;
    case 99:
    case 102: return scene_version >= 26 ? 4 : 0;
    default: return 0;
    }
}

void la_speed_reset(la_speed_t* s) {
    memset(s, 0, sizeof *s);
    s->base = s->current = 1.0f;
}

float la_speed_clamp(float speed) {
    if (!(speed >= SPEED_LEAST)) return SPEED_LEAST;
    return fabsf(speed - 1.0f) < SPEED_NORMAL_WITHIN ? 1.0f : speed;
}

void la_speed_set(la_speed_t* s, float speed) { s->base = la_speed_clamp(speed); }

void la_speed_slow(la_speed_t* s, float speed, float duration_ms, float enter_ms, float leave_ms) {
    const float start = s->active ? s->current : 1.0f;
    const int again = s->active;
    if (!(duration_ms > 0.0f)) return;
    s->slow = la_speed_clamp(speed);
    s->enter = enter_ms > 0.0f ? enter_ms / 1000.0f : 0.0f;
    s->leave = leave_ms > 0.0f ? leave_ms / 1000.0f : 0.0f;
    if (again && s->slow < 1.0f) {
        const float left = fabsf(start - s->slow);
        if (left < SLOW_SAME) s->enter = 0.0f;
        else if (start < 1.0f) s->enter *= left / (1.0f - s->slow);
    }
    s->duration = duration_ms / 1000.0f;
    s->start = start;
    s->elapsed = 0.0f;
    s->active = 1;
    la_speed_step(s, 0.0f);
}

void la_speed_step(la_speed_t* s, float game_seconds) {
    const float hold_end = s->enter + s->duration;
    if (!s->active) return;
    s->elapsed += game_seconds / la_speed_factor(s) * s->base;
    if (s->elapsed < s->enter) {
        s->current = s->start + (s->slow - s->start) * (s->elapsed / s->enter);
    } else if (s->elapsed < hold_end) {
        s->current = s->slow;
    } else if (s->elapsed < hold_end + s->leave) {
        s->current = s->slow + (1.0f - s->slow) * ((s->elapsed - hold_end) / s->leave);
    } else {
        s->current = 1.0f;
        s->active = 0;
    }
}

float la_speed_factor(const la_speed_t* s) {
    const float factor = s->base * s->current;
    return factor > SPEED_LEAST ? factor : SPEED_LEAST;
}

void la_controls_reset(la_controls_t* c) {
    c->mask = COMMANDS_ALL;
    c->mouse_off = 0;
}

void la_controls_set(la_controls_t* c, int keys, int mouse) {
    if (keys == 0) c->mask = 0;
    if (keys == 1) c->mask = COMMANDS_ALL;
    if (keys == 2) c->mask = ~(COMMANDS_MOVE | COMMAND_JUMP);
    if (mouse == 0 || mouse == 1) c->mouse_off = !mouse;
}

int la_controls_active(const la_controls_t* c) { return c->mask != COMMANDS_ALL || c->mouse_off; }

void la_controls_apply(const la_controls_t* c, float rotate[2], float move[2], uint32_t* state, uint32_t* hit) {
    *state &= c->mask;
    *hit &= c->mask;
    if (!(c->mask & COMMANDS_MOVE)) move[0] = move[1] = 0.0f;
    if (c->mouse_off) rotate[0] = rotate[1] = 0.0f;
}

int la_hint_join(const char* sample, const char* frame, char* out, size_t cap) {
    const char *first = sample ? sample : "", *second = frame ? frame : "";
    if (!first[0] && !second[0]) return 0;
    snprintf(out, cap, "%s%s%s", first, first[0] && second[0] ? " " : "", second);
    return 1;
}

static const char* quoted(const char* p, const char* end, const char** from, size_t* length) {
    const char* close;
    if (p == end || *p != '"' || (close = (const char*)memchr(p + 1, '"', (size_t)(end - p - 1))) == NULL) return NULL;
    *from = p + 1;
    *length = (size_t)(close - p - 1);
    return close + 1;
}

static const char* skip_blanks(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static int hint_line(const char* p, const char* end, const char* key, char* out, size_t cap) {
    const char *name, *text, *after;
    size_t length, count;
    p = skip_blanks(p, end);
    if (p == end || *p == ';') return 0;
    after = quoted(p, end, &name, &length);
    if (!after) {
        after = (const char*)memchr(p, '=', (size_t)(end - p));
        if (!after) return 0;
        name = p;
        length = (size_t)(after - p);
        while (length && (p[length - 1] == ' ' || p[length - 1] == '\t')) length--;
    }
    after = skip_blanks(after, end);
    if (after == end || *after != '=' || length != strlen(key) || memcmp(name, key, length)) return 0;
    if (!quoted(skip_blanks(after + 1, end), end, &text, &count)) return 0;
    snprintf(out, cap, "%.*s", (int)count, text);
    return 1;
}

int la_hint_lookup(const char* strings, size_t size, const char* key, char* out, size_t cap) {
    const char *p = strings, *end = strings + size;
    while (p < end) {
        const char* line = (const char*)memchr(p, '\n', (size_t)(end - p));
        if (hint_line(p, line ? line : end, key, out, cap)) return 1;
        p = line ? line + 1 : end;
    }
    return 0;
}

int la_reload_rounds(int ammo, int alt_count, int most) { return ammo >= 0 ? ammo : most - alt_count; }

int la_pick_group(const int* weights, int count, uint32_t draw) {
    uint32_t total = 0, r;
    for (int i = 0; i < count; i++) total += weights[i] > 0 ? (uint32_t)weights[i] : 0;
    if (!total) return -1;
    r = draw % total;
    for (int i = 0; i < count; i++) {
        if (weights[i] <= 0) continue;
        if (r < (uint32_t)weights[i]) return i;
        r -= (uint32_t)weights[i];
    }
    return -1;
}

int la_shuffle(uint8_t* list, int left, uint32_t draw) {
    const uint32_t k = draw % (uint32_t)left;
    const int taken = list[k];
    list[k] = list[left - 1];
    return taken;
}

static void product(const float a[4], const float b[4], float out[4]) {
    out[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    out[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    out[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    out[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}

static void turn(int axis, float degrees, float out[4]) {
    const double half = (double)degrees * PI / 360.0;
    memset(out, 0, 4 * sizeof *out);
    out[0] = (float)cos(half);
    out[1 + axis] = (float)sin(half);
}

void la_quaternion(float x, float y, float z, float out[4]) {
    float qx[4], qy[4], qz[4], zy[4];
    turn(0, x, qx);
    turn(1, y, qy);
    turn(2, z, qz);
    product(qz, qy, zy);
    product(zy, qx, out);
}

uint32_t la_random(uint32_t* seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed >> 8;
}

void la_forward(const float q[4], float out[3]) {
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    float length;
    out[0] = 2.0f * (x * y - w * z);
    out[1] = 2.0f * (y * y + w * w - 0.5f);
    out[2] = 2.0f * (w * x + y * z);
    length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    for (int i = 0; i < 3; i++) out[i] = length > 0.0f ? out[i] / length : i == 1 ? 1.0f : 0.0f;
}

int la_map_name(const char* sample, char out[LA_MAP_NAME_MAX]) {
    const size_t n = strcspn(sample, "\"");
    if (!n || n >= LA_MAP_NAME_MAX) return 0;
    memcpy(out, sample, n);
    out[n] = 0;
    return 1;
}

void la_map_cells(const uint8_t* bgra, int width, int height, int side, int* cols, int* rows, uint32_t* cells) {
    *cols = width < side ? width : side;
    *rows = height < side ? height : side;
    for (int r = 0; r < *rows; r++) {
        for (int c = 0; c < *cols; c++) {
            const int x0 = (int)((int64_t)c * width / *cols), x1 = (int)((int64_t)(c + 1) * width / *cols);
            const int y0 = (int)((int64_t)r * height / *rows), y1 = (int)((int64_t)(r + 1) * height / *rows);
            uint64_t sum[4] = { 0 }, n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++, n++)
                    for (int k = 0; k < 4; k++) sum[k] += bgra[((size_t)y * (size_t)width + (size_t)x) * 4 + (size_t)k];
            cells[r * *cols + c] = (uint32_t)(sum[3] / n) << 24 | (uint32_t)(sum[2] / n) << 16 | (uint32_t)(sum[1] / n) << 8 | (uint32_t)(sum[0] / n);
        }
    }
}

float la_health(float now, float most, float lost) { return lost == 0.0f ? most : now - lost; }

static int separator(char c) { return c == '/' || c == '\\'; }

int la_video_name(const char* sample, char out[LA_VIDEO_MAX]) {
    const size_t n = strlen(sample);
    if (!n || n >= LA_VIDEO_MAX || separator(sample[0]) || strchr(sample, ':') || strstr(sample, "..")) return 0;
    for (size_t i = 0; i <= n; i++) out[i] = separator(sample[i]) ? '\\' : sample[i];
    return 1;
}

static uint32_t channel(int value) { return value < 0 ? 0u : value > 255 ? 255u : (uint32_t)value; }

uint32_t la_colour(int r, int g, int b) { return channel(r) << 16 | channel(g) << 8 | channel(b); }

float la_fixed_seconds(int interval_ms) { return interval_ms < 0 ? LA_UNLIMITED : (float)interval_ms / 1000.0f; }

int la_fixed_step(la_fixed_t* f, float seconds) {
    if (f->left <= UNLIMITED_BELOW) return 1;
    f->left -= seconds;
    return f->left >= 0.0f;
}

int la_fixed_set(la_state_t* s, uint32_t instance, const float velocity[3], float seconds, int replace) {
    uint32_t i = 0;
    while (i < s->fixed_count && s->fixed[i].instance != instance) i++;
    if (i < s->fixed_count ? !replace : i >= LA_FIXED_MAX) return 0;
    if (i == s->fixed_count) s->fixed_count++;
    s->fixed[i].instance = instance;
    memcpy(s->fixed[i].velocity, velocity, sizeof s->fixed[i].velocity);
    s->fixed[i].left = seconds;
    return 1;
}

void la_fixed_end(la_state_t* s, uint32_t instance) {
    for (uint32_t i = 0; i < s->fixed_count; i++) {
        if (s->fixed[i].instance != instance) continue;
        memmove(&s->fixed[i], &s->fixed[i + 1], (s->fixed_count - i - 1u) * sizeof s->fixed[0]);
        s->fixed_count--;
        return;
    }
}

int la_ai_valid(int type) { return type >= -1 && type <= AI_TYPE_LAST; }

int la_ai_type(int type, int now) { return type >= 0 ? type : now == AI_TYPE_SEEN ? 0 : AI_TYPE_SEEN; }

int la_rounds_left(int rounds, int no_click) {
    if (rounds <= 0) return -1;
    return rounds == 1 && no_click ? -1 : rounds - 1;
}

void la_pitched(const float quaternion[4], float degrees, float out[4]) {
    float pitch[4];
    turn(0, degrees, pitch);
    product(quaternion, pitch, out);
}

uint32_t la_probability(int percent) { return percent == 100 ? 0xFFFFFFFFu : (uint32_t)percent * PROBABILITY_UNIT; }

void la_swap_set(la_state_t* s, uint32_t instance, int material, int frame) {
    for (uint32_t i = 0; i < s->swap_count; i++) {
        if (s->swaps[i].instance != instance || s->swaps[i].material != material) continue;
        memmove(&s->swaps[i], &s->swaps[i + 1], (s->swap_count - i - 1u) * sizeof s->swaps[0]);
        s->swap_count--;
        break;
    }
    if (s->swap_count < LA_SWAPS_MAX) s->swaps[s->swap_count++] = (la_swap_t){ instance, material, frame };
}

void la_reset_set(la_state_t* s, uint32_t trigger, int count) {
    uint32_t i = 0;
    while (i < s->reset_count && s->resets[i].trigger != trigger) i++;
    if (i == LA_RESETS_MAX) return;
    if (i == s->reset_count) s->reset_count++;
    s->resets[i] = (la_reset_t){ trigger, count };
}

typedef struct {
    uint8_t* at;
    size_t left;
    int reading, ok;
} stream_t;

static void pass(stream_t* s, void* value, size_t size) {
    if (!s->ok || s->left < size) {
        s->ok = 0;
        return;
    }
    if (s->reading) memcpy(value, s->at, size);
    else memcpy(s->at, value, size);
    s->at += size;
    s->left -= size;
}

static void pass_list(stream_t* f, void* items, uint32_t* count, uint32_t most, size_t size) {
    pass(f, count, 4);
    if (*count > most) f->ok = 0;
    if (f->ok) pass(f, items, *count * size);
}

static size_t walk(la_state_t* s, stream_t* f) {
    const size_t size = f->left;
    uint32_t magic = STATE_MAGIC, length = (uint32_t)strlen(s->hint), active = (uint32_t)s->speed.active, mouse = (uint32_t)s->controls.mouse_off;
    float* speeds[] = { &s->speed.base, &s->speed.current, &s->speed.slow, &s->speed.start, &s->speed.duration, &s->speed.enter, &s->speed.leave, &s->speed.elapsed };
    pass(f, &magic, 4);
    if (magic != STATE_MAGIC && magic != STATE_MAGIC_FIRST) f->ok = 0;
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) pass(f, speeds[i], 4);
    pass(f, &active, 4);
    pass(f, &s->controls.mask, 4);
    pass(f, &mouse, 4);
    pass(f, &length, 4);
    if (length >= LA_TEXT_MAX) f->ok = 0;
    if (f->ok) pass(f, s->hint, length);
    pass(f, &s->run_count, 4);
    if (s->run_count > LA_RUNS_MAX) f->ok = 0;
    if (f->ok) pass(f, s->runs, s->run_count * sizeof s->runs[0]);
    s->speed.active = active != 0;
    s->controls.mouse_off = mouse != 0;
    if (f->reading && f->ok) s->hint[length] = 0;
    pass(f, s->map, sizeof s->map);
    s->map[sizeof s->map - 1] = 0;
    pass(f, s->map_corners, sizeof s->map_corners);
    if (magic == STATE_MAGIC_FIRST) {
        s->player_ai = LA_AI_NONE;
        return f->ok ? size - f->left : 0;
    }
    pass(f, &s->player_ai, 4);
    pass_list(f, s->fixed, &s->fixed_count, LA_FIXED_MAX, sizeof s->fixed[0]);
    pass_list(f, s->swaps, &s->swap_count, LA_SWAPS_MAX, sizeof s->swaps[0]);
    pass_list(f, s->resets, &s->reset_count, LA_RESETS_MAX, sizeof s->resets[0]);
    return f->ok ? size - f->left : 0;
}

size_t la_state_write(const la_state_t* s, uint8_t* out, size_t cap) {
    la_state_t copy = *s;
    stream_t f = { out, cap, 0, 1 };
    return walk(&copy, &f);
}

static int sane(float value) { return value >= 0.0f && value < 86400.0f; }

int la_state_read(la_state_t* s, const uint8_t* bytes, size_t size) {
    la_state_t read;
    stream_t f = { (uint8_t*)(uintptr_t)bytes, size, 1, 1 };
    memset(&read, 0, sizeof read);
    if (!walk(&read, &f)) return 0;
    if (!sane(read.speed.base) || !sane(read.speed.current) || !sane(read.speed.slow) || !sane(read.speed.start) || !sane(read.speed.duration) ||
        !sane(read.speed.enter) || !sane(read.speed.leave) || !sane(read.speed.elapsed))
        return 0;
    for (uint32_t i = 0; i < read.fixed_count; i++)
        for (int k = 0; k < 3; k++)
            if (!isfinite(read.fixed[i].velocity[k]) || !isfinite(read.fixed[i].left)) return 0;
    if (read.player_ai != LA_AI_NONE && !la_ai_valid(read.player_ai)) return 0;
    *s = read;
    return 1;
}

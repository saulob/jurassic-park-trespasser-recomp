#include "pad_glyphs.h"

#include <math.h>
#include <string.h>

#define DARK 0x24272Bu
#define RIM 0x6E737Au
#define WHITE 0xF4F4F4u
#define LIGHT 0xD8DBDFu
#define KEYCAP 0xEDEDEDu
#define KEY_RIM 0x8C8C8Cu
#define KEY_INK 0x1C1C1Cu
#define MOUSE_LIT 0x3C7FD8u

static const uint32_t k_xbox_letters[4] = { 0x6CC24Au, 0xE5483Bu, 0x3D8FE0u, 0xF2C230u };
static const uint32_t k_ps_symbols[4] = { 0x8FB8F0u, 0xF0646Eu, 0xE59AD0u, 0x4FD8B0u };

static void base(pad_glyph_t* g, int shape, float width, float text_size) {
    memset(g, 0, sizeof *g);
    g->shape = (uint8_t)shape;
    g->width = width;
    g->text_x = width * 0.5f;
    g->text_y = 0.5f;
    g->text_size = text_size;
    g->color[PAD_LAYER_BODY] = DARK;
    g->color[PAD_LAYER_RIM] = RIM;
    g->color[PAD_LAYER_MARK] = WHITE;
    g->color[PAD_LAYER_ACCENT] = LIGHT;
    g->text_color = WHITE;
}

static void set_text(pad_glyph_t* g, const char* text) {
    strncpy(g->text, text ? text : "", sizeof g->text - 1);
    g->text[sizeof g->text - 1] = 0;
}

static float pill_width(const char* text, float per_char, float lo, float hi) {
    float w = 0.55f + per_char * (float)strlen(text);
    return w < lo ? lo : w > hi ? hi : w;
}

static int face(jpt_pad_family_t family, int control, pad_glyph_t* g) {
    const int index = control - JPT_PAD_SOUTH;
    if (family == JPT_FAMILY_GENERIC) {
        base(g, PAD_GLYPH_DIAMOND, 1.0f, 0.0f);
        g->dir = (uint8_t)index;
        g->color[PAD_LAYER_ACCENT] = 0x9AA0A6u;
        return 1;
    }
    base(g, PAD_GLYPH_FACE, 1.0f, 0.52f);
    if (family == JPT_FAMILY_PLAYSTATION) {
        static const uint8_t symbols[4] = { PAD_SYMBOL_CROSS, PAD_SYMBOL_CIRCLE, PAD_SYMBOL_SQUARE, PAD_SYMBOL_TRIANGLE };
        g->symbol = symbols[index];
        g->color[PAD_LAYER_MARK] = k_ps_symbols[index];
        return 1;
    }
    set_text(g, jpt_pad_glyph_text(family, control));
    if (family == JPT_FAMILY_XBOX) g->text_color = k_xbox_letters[index];
    return 1;
}

int pad_glyph_control(jpt_pad_family_t family, int control, int dir, int tilt, pad_glyph_t* g) {
    const char* text = jpt_pad_glyph_text(family, control);
    switch (control) {
    case JPT_PAD_SOUTH:
    case JPT_PAD_EAST:
    case JPT_PAD_WEST:
    case JPT_PAD_NORTH: return face(family, control, g);
    case JPT_PAD_LEFT_SHOULDER:
    case JPT_PAD_RIGHT_SHOULDER:
        base(g, PAD_GLYPH_BUMPER, 1.5f, 0.42f);
        break;
    case JPT_PAD_LEFT_TRIGGER:
    case JPT_PAD_RIGHT_TRIGGER:
        base(g, PAD_GLYPH_TRIGGER, 1.3f, 0.42f);
        break;
    case JPT_PAD_BACK:
    case JPT_PAD_GUIDE:
    case JPT_PAD_START:
        base(g, PAD_GLYPH_PILL, pill_width(text, 0.34f, 1.2f, 3.4f), 0.36f);
        break;
    case JPT_PAD_DPAD_UP:
    case JPT_PAD_DPAD_DOWN:
    case JPT_PAD_DPAD_LEFT:
    case JPT_PAD_DPAD_RIGHT:
        base(g, PAD_GLYPH_DPAD, 1.0f, 0.0f);
        g->dir = (uint8_t)(JPT_DIR_UP + (control - JPT_PAD_DPAD_UP));
        return 1;
    case JPT_PAD_LEFT_STICK:
    case JPT_PAD_RIGHT_STICK:
        base(g, PAD_GLYPH_STICK_CLICK, 1.0f, 0.34f);
        break;
    case JPT_PAD_LEFT_STICK_MOVE:
    case JPT_PAD_RIGHT_STICK_MOVE: {
        static const float dx[5] = { 0, 0, 0, -1, 1 }, dy[5] = { 0, -1, 1, 0, 0 };
        float travel;
        base(g, PAD_GLYPH_STICK, 1.0f, 0.30f);
        if (dir < JPT_DIR_NONE || dir > JPT_DIR_RIGHT) dir = JPT_DIR_NONE;
        g->dir = (uint8_t)dir;
        g->tilt = (uint8_t)(dir == JPT_DIR_NONE ? 0 : tilt < 1 ? 1 : tilt > 2 ? 2 : tilt);
        travel = g->tilt == 2 ? 0.15f : g->tilt == 1 ? 0.075f : 0.0f;
        g->text_x = 0.5f + dx[g->dir] * travel;
        g->text_y = 0.5f + dy[g->dir] * travel;
        g->text_color = KEY_INK;
        break;
    }
    default: return 0;
    }
    set_text(g, text);
    return 1;
}

int pad_glyph_key(uint32_t vk, const char* label, pad_glyph_t* g) {
    if (vk == 0x01 || vk == 0x02 || vk == 0x04) {
        base(g, PAD_GLYPH_MOUSE, 0.8f, 0.0f);
        g->dir = (uint8_t)(vk == 0x01 ? 1 : vk == 0x02 ? 2 : 3);
        g->color[PAD_LAYER_BODY] = KEYCAP;
        g->color[PAD_LAYER_RIM] = KEY_RIM;
        g->color[PAD_LAYER_MARK] = KEY_RIM;
        g->color[PAD_LAYER_ACCENT] = MOUSE_LIT;
        return 1;
    }
    if (!label || !*label) return 0;
    base(g, PAD_GLYPH_KEY, pill_width(label, 0.40f, 1.0f, 4.2f), 0.44f);
    g->color[PAD_LAYER_BODY] = KEYCAP;
    g->color[PAD_LAYER_RIM] = KEY_RIM;
    g->text_color = KEY_INK;
    set_text(g, label);
    return 1;
}

int pad_glyph_token(const jpt_prompt_token_t* token, jpt_pad_family_t family, pad_glyph_t* out) {
    if (token->prompt.gamepad)
        return pad_glyph_control(family, token->prompt.control, token->prompt.dir, token->prompt.tilt, out);
    if (token->mouse_move) {
        pad_glyph_key(0x01, NULL, out);
        out->dir = 0;
        return 1;
    }
    return pad_glyph_key(token->prompt.vk, token->label, out);
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static float cover(float d, float aa) { return clamp01(0.5f - d / aa); }

static float sd_circle(float u, float v, float cx, float cy, float r) { return sqrtf((u - cx) * (u - cx) + (v - cy) * (v - cy)) - r; }

static float sd_box(float u, float v, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(u - cx) - (hw - r), qy = fabsf(v - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static float sd_segment(float u, float v, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = u - ax, wy = v - ay, t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
    t = clamp01(t);
    wx -= vx * t;
    wy -= vy * t;
    return sqrtf(wx * wx + wy * wy);
}

static float outline(const pad_glyph_t* g, float u, float v) {
    const float cx = g->width * 0.5f, cy = 0.5f;
    switch (g->shape) {
    case PAD_GLYPH_FACE:
    case PAD_GLYPH_DIAMOND:
    case PAD_GLYPH_STICK:
    case PAD_GLYPH_STICK_CLICK: return sd_circle(u, v, cx, cy, 0.46f);
    case PAD_GLYPH_BUMPER: return sd_box(u, v, cx, cy, g->width * 0.5f - 0.05f, 0.30f, 0.18f);
    case PAD_GLYPH_TRIGGER: return sd_box(u, v, cx, cy, g->width * 0.5f - 0.05f, 0.40f, v < cy ? 0.30f : 0.12f);
    case PAD_GLYPH_PILL: return sd_box(u, v, cx, cy, g->width * 0.5f - 0.04f, 0.30f, 0.30f);
    case PAD_GLYPH_DPAD: {
        float a = sd_box(u, v, cx, cy, 0.16f, 0.46f, 0.06f), b = sd_box(u, v, cx, cy, 0.46f, 0.16f, 0.06f);
        return a < b ? a : b;
    }
    case PAD_GLYPH_KEY: return sd_box(u, v, cx, cy, g->width * 0.5f - 0.03f, 0.40f, 0.14f);
    case PAD_GLYPH_MOUSE: return sd_box(u, v, cx, cy, 0.30f, 0.46f, 0.28f);
    default: return 1.0f;
    }
}

#define RIM_WIDTH 0.075f
#define STROKE 0.07f

static float symbol(const pad_glyph_t* g, float u, float v, float aa) {
    const float cx = g->width * 0.5f, cy = 0.5f;
    float d;
    switch (g->symbol) {
    case PAD_SYMBOL_CROSS: {
        const float s = 0.16f;
        float a = sd_segment(u, v, cx - s, cy - s, cx + s, cy + s), b = sd_segment(u, v, cx - s, cy + s, cx + s, cy - s);
        d = (a < b ? a : b) - STROKE * 0.5f;
        break;
    }
    case PAD_SYMBOL_CIRCLE: d = fabsf(sd_circle(u, v, cx, cy, 0.17f)) - STROKE * 0.5f; break;
    case PAD_SYMBOL_SQUARE: d = fabsf(sd_box(u, v, cx, cy, 0.155f, 0.155f, 0.02f)) - STROKE * 0.5f; break;
    case PAD_SYMBOL_TRIANGLE: {
        const float top = cy - 0.19f, low = cy + 0.13f, s = 0.19f;
        float a = sd_segment(u, v, cx, top, cx - s, low), b = sd_segment(u, v, cx - s, low, cx + s, low);
        float c = sd_segment(u, v, cx + s, low, cx, top);
        d = (a < b ? (a < c ? a : c) : (b < c ? b : c)) - STROKE * 0.5f;
        break;
    }
    default: return 0.0f;
    }
    return cover(d, aa);
}

static const float k_dot_x[4] = { 0.0f, 0.22f, -0.22f, 0.0f }, k_dot_y[4] = { 0.22f, 0.0f, 0.0f, -0.22f };

static float diamond(const pad_glyph_t* g, int layer, float u, float v, float aa) {
    const float cx = g->width * 0.5f, cy = 0.5f;
    float best = 0.0f;
    for (int i = 0; i < 4; i++) {
        float d = sd_circle(u, v, cx + k_dot_x[i], cy + k_dot_y[i], 0.095f), c;
        if (layer == PAD_LAYER_MARK && i == g->dir) c = cover(d, aa);
        else if (layer == PAD_LAYER_ACCENT && i != g->dir) c = cover(fabsf(d) - 0.02f, aa);
        else continue;
        if (c > best) best = c;
    }
    return best;
}

static float stick_arc(const pad_glyph_t* g, float u, float v, float aa) {
    static const float ax[5] = { 0, 0, 0, -1, 1 }, ay[5] = { 0, -1, 1, 0, 0 };
    const float cx = g->width * 0.5f, cy = 0.5f;
    float du = u - cx, dv = v - cy, len = sqrtf(du * du + dv * dv), along;
    if (g->dir == JPT_DIR_NONE || len <= 0.0f) return 0.0f;
    along = (du * ax[g->dir] + dv * ay[g->dir]) / len;
    return cover(fabsf(sd_circle(u, v, cx, cy, 0.46f - RIM_WIDTH * 0.5f)) - RIM_WIDTH * 0.5f, aa) * clamp01((along - 0.80f) / 0.08f);
}

static float stick_cap(const pad_glyph_t* g, float u, float v, float aa) {
    return cover(sd_circle(u, v, g->text_x, g->text_y, 0.24f), aa);
}

static float dpad_arm(const pad_glyph_t* g, float u, float v, float aa) {
    static const float ox[5] = { 0, 0, 0, -0.27f, 0.27f }, oy[5] = { 0, -0.27f, 0.27f, 0, 0 };
    const float cx = g->width * 0.5f + ox[g->dir], cy = 0.5f + oy[g->dir];
    int vertical = g->dir == JPT_DIR_UP || g->dir == JPT_DIR_DOWN;
    if (g->dir == JPT_DIR_NONE) return 0.0f;
    return cover(sd_box(u, v, cx, cy, vertical ? 0.10f : 0.15f, vertical ? 0.15f : 0.10f, 0.04f), aa);
}

static float mouse_parts(const pad_glyph_t* g, int layer, float u, float v, float aa) {
    const float cx = g->width * 0.5f, split = 0.44f;
    float inside = cover(outline(g, u, v) + RIM_WIDTH, aa);
    if (layer == PAD_LAYER_MARK) {
        float a = sd_segment(u, v, cx, 0.0f, cx, split), b = sd_segment(u, v, cx - 0.32f, split, cx + 0.32f, split);
        return cover((a < b ? a : b) - 0.022f, aa) * inside;
    }
    if (g->dir == 0) return 0.0f;
    if (g->dir == 3) return cover(sd_box(u, v, cx, 0.27f, 0.05f, 0.09f, 0.04f), aa);
    if (v >= split) return 0.0f;
    if (g->dir == 1) return inside * clamp01((cx - u) / aa);
    return inside * clamp01((u - cx) / aa);
}

float pad_glyph_coverage(const pad_glyph_t* g, int layer, float u, float v, float aa) {
    float d;
    if (aa <= 0.0f) aa = 0.01f;
    d = outline(g, u, v);
    switch (layer) {
    case PAD_LAYER_BODY: return cover(d, aa);
    case PAD_LAYER_RIM: return g->shape == PAD_GLYPH_DPAD ? cover(d, aa) - cover(d + RIM_WIDTH * 0.7f, aa)
                                                          : cover(d, aa) - cover(d + RIM_WIDTH, aa);
    case PAD_LAYER_MARK:
        switch (g->shape) {
        case PAD_GLYPH_FACE: return symbol(g, u, v, aa);
        case PAD_GLYPH_DIAMOND: return diamond(g, layer, u, v, aa);
        case PAD_GLYPH_STICK: return stick_arc(g, u, v, aa);
        case PAD_GLYPH_MOUSE: return mouse_parts(g, layer, u, v, aa);
        default: return 0.0f;
        }
    case PAD_LAYER_ACCENT:
        switch (g->shape) {
        case PAD_GLYPH_DIAMOND: return diamond(g, layer, u, v, aa);
        case PAD_GLYPH_DPAD: return dpad_arm(g, u, v, aa);
        case PAD_GLYPH_STICK: return stick_cap(g, u, v, aa);
        case PAD_GLYPH_STICK_CLICK: return cover(fabsf(sd_circle(u, v, g->width * 0.5f, 0.5f, 0.32f)) - 0.025f, aa);
        case PAD_GLYPH_MOUSE: return mouse_parts(g, layer, u, v, aa);
        default: return 0.0f;
        }
    default: return 0.0f;
    }
}

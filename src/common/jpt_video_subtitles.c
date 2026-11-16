#include "jpt_video_subtitles.h"

#include <string.h>

#define CUES(a) a, (int)(sizeof a / sizeof a[0])

static const jvs_cue_t k_intro[] = {
    { 0, 82, "Video::My name is John Parker Hammond. I was born on March 14, 1928." },
    { 89, 124, "Video::What follows is a record of certain events" },
    { 125, 194, "Video::in which I took part, between the years 1980 and 1997," },
    { 197, 245, "Video::on an island I will call Site B." },
    { 251, 314, "Video::Site B was not to be a theme park, but a research station." },
    { 315, 364, "Video::This was where we did the real work." },
    { 447, 535, "Video::By 1989, International Genetic Technologies had succeeded in their design," },
    { 536, 587, "Video::to genetically recreate the dinosaurs." },
    { 621, 707, "Video::It was an unprecedented accomplishment, the pinnacle of 20th century science," },
    { 708, 775, "Video::a work to rank with the achievements of Galileo, or Einstein." },
    { 848, 910, "Video::But it was not all so easy or so simple as it appeared." },
    { 919, 973, "Video::One seldom hears the true history of such events," },
    { 974, 1030, "Video::what happened at the place where the world changed." },
    { 1035, 1057, "Video::How it began..." },
    { 1061, 1090, "Video::...what were the reasons..." },
    { 1095, 1139, "Video::...what was the cost." },
};

static const jvs_cue_t k_new_game[] = {
    { 368, 399, "Video::The person you're trying to reach is unavailable." },
    { 400, 433, "Video::Please leave a message after the tone." },
    { 435, 460, "Video::Anne? Anne, are you there?" },
    { 461, 493, "Video::It's Jill, pick up the phone!" },
    { 500, 539, "Video::God, don't tell me your mom was serious? You're in Costa Rica?" },
    { 545, 586, "Video::Visiting the natives, huh? Sun, sand and adventure." },
    { 591, 634, "Video::My little Indiana Anne, world traveler extraordinaire." },
    { 639, 671, "Video::Well, give me a call when you get back, okay?" },
    { 678, 730, "Video::By the way, I thought you hated flying?" },
};

static const jvs_cue_t k_win[] = {
    { 278, 314, "Video::The person you're trying to reach is unavailable." },
    { 315, 344, "Video::Please leave a message after the tone." },
    { 354, 406, "Video::Anne! It's Jill. This is like the eighth message I'm leaving you." },
    { 413, 439, "Video::I'm sensing some sort of pattern here..." },
    { 443, 487, "Video::Come on, Anne. Quit being antisocial and pick up the phone sometime." },
    { 491, 524, "Video::Oh, and by the way, don't even bother calling me" },
    { 525, 550, "Video::unless you have a damn good excuse." },
};

const jvs_timeline_t jvs_timelines[] = {
    { "tpassintro.smk", 29473692u, "8df7c0d6c0e809cf17c644b81c4e1ba40fde487227d35dfccb1e22323c1cc915", 1585u, CUES(k_intro) },
    { "newgame.smk", 15502788u, "98a379a48847e603c54ea84ee95c872eafdadbbd49e9a4a8e7d75e6a218b7770", 1085u, CUES(k_new_game) },
    { "win.smk", 13227104u, "fbe95bb588dc4b639e4b7a3177b25623482dcbb1d6661db5780bf5346bc01ab9", 606u, CUES(k_win) },
};
const int jvs_timeline_count = (int)(sizeof jvs_timelines / sizeof jvs_timelines[0]);

static int same_name(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

const jvs_timeline_t* jvs_timeline_named(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < jvs_timeline_count; i++)
        if (same_name(name, jvs_timelines[i].name)) return &jvs_timelines[i];
    return NULL;
}

int jvs_fingerprint_matches(const jvs_timeline_t* t, uint64_t size, const char* sha256_hex) {
    return t && sha256_hex && size == t->size && same_name(sha256_hex, t->sha256);
}

jvs_choice_t jvs_select(int enabled, const char* name, uint64_t size, const char* sha256_hex, const jvs_timeline_t** out) {
    const jvs_timeline_t* t = jvs_timeline_named(name);
    *out = NULL;
    if (!enabled) return JVS_DISABLED;
    if (!t) return JVS_NO_DIALOGUE;
    if (!jvs_fingerprint_matches(t, size, sha256_hex)) return JVS_NOT_RETAIL;
    *out = t;
    return JVS_SELECTED;
}

int jvs_cue_at(const jvs_timeline_t* t, int32_t frame) {
    if (!t || frame < 0) return -1;
    for (int i = 0; i < t->cue_count; i++) {
        if (frame < t->cues[i].first) return -1;
        if (frame <= t->cues[i].last) return i;
    }
    return -1;
}

double jvs_frame_ms(int32_t frame, double ms_per_frame) { return (double)frame * ms_per_frame; }

void jvs_session_begin(jvs_session_t* s, const jvs_timeline_t* t) {
    memset(s, 0, sizeof *s);
    s->timeline = t;
    s->playing = t != NULL;
    s->last_frame = -1;
}

int jvs_session_cue(jvs_session_t* s, int32_t frame) {
    if (!s->playing || s->ended) return -1;
    if (frame < s->last_frame) {
        s->ended = 1;
        return -1;
    }
    s->last_frame = frame;
    return jvs_cue_at(s->timeline, frame);
}

void jvs_session_end(jvs_session_t* s) {
    s->playing = 0;
    s->timeline = NULL;
}

int jvs_font_px(const jvs_rect_t* picture) {
    int px = (picture->h * 52 + 500) / 1000;
    return px < 12 ? 12 : px;
}

int jvs_outline_px(int font_px) {
    int r = (font_px + 8) / 16;
    return r < 1 ? 1 : r;
}

static int safe_margin(int size) {
    int m = size * 4 / 100;
    return m < 2 ? 2 : m;
}

int jvs_max_line_width(const jvs_rect_t* picture, int target_w) {
    int w = picture->w * 88 / 100, safe = target_w - 2 * safe_margin(target_w);
    return w < safe ? w : safe;
}

jvs_rect_t jvs_place(int target_w, int target_h, const jvs_rect_t* picture, int block_w, int block_h) {
    jvs_rect_t r;
    int mx = safe_margin(target_w), my = safe_margin(target_h), bottom;
    r.w = block_w;
    r.h = block_h;
    r.x = picture->x + (picture->w - block_w) / 2;
    bottom = picture->y + picture->h - picture->h * 5 / 100;
    if (bottom > target_h - my) bottom = target_h - my;
    r.y = bottom - block_h;
    if (r.x + r.w > target_w - mx) r.x = target_w - mx - r.w;
    if (r.x < mx) r.x = mx;
    if (r.y < my) r.y = my;
    return r;
}

static int measure(const char* text, int start, int end, jvs_width_fn width, void* ctx) {
    while (start < end && text[start] == ' ') start++;
    while (end > start && text[end - 1] == ' ') end--;
    return end > start ? width(ctx, text + start, end - start) : 0;
}

static jvs_line_t line_of(const char* text, int start, int end, jvs_width_fn width, void* ctx) {
    jvs_line_t l;
    while (start < end && text[start] == ' ') start++;
    while (end > start && text[end - 1] == ' ') end--;
    l.start = start;
    l.length = end - start;
    l.width = l.length ? width(ctx, text + start, l.length) : 0;
    return l;
}

int jvs_wrap(const char* text, int max_width, jvs_width_fn width, void* ctx, jvs_line_t* lines) {
    int n = (int)strlen(text), best = -1, best_w = 0, count = 0, start = 0;
    if (!n) return 0;
    if (measure(text, 0, n, width, ctx) <= max_width) {
        lines[0] = line_of(text, 0, n, width, ctx);
        return 1;
    }
    for (int i = 1; i < n; i++) {
        int w1, w2, worst;
        if (text[i] != ' ') continue;
        w1 = measure(text, 0, i, width, ctx);
        w2 = measure(text, i, n, width, ctx);
        worst = w1 > w2 ? w1 : w2;
        if (worst <= max_width && (best < 0 || worst < best_w)) {
            best = i;
            best_w = worst;
        }
    }
    if (best >= 0) {
        lines[0] = line_of(text, 0, best, width, ctx);
        lines[1] = line_of(text, best, n, width, ctx);
        return 2;
    }
    while (start < n && count < JVS_MAX_LINES) {
        int end = n, last_space = -1;
        if (count < JVS_MAX_LINES - 1 && measure(text, start, n, width, ctx) > max_width) {
            for (int i = start + 1; i < n; i++)
                if (text[i] == ' ') {
                    if (measure(text, start, i, width, ctx) > max_width) break;
                    last_space = i;
                }
            if (last_space > start) end = last_space;
            else {
                for (end = start + 1; end < n && text[end] != ' '; end++) {
                }
            }
        }
        lines[count++] = line_of(text, start, end, width, ctx);
        start = end;
        while (start < n && text[start] == ' ') start++;
    }
    return count;
}

static uint32_t over(uint32_t d, uint32_t s) {
    uint32_t a = s >> 24, inv = 255 - a, out = 0;
    if (!a) return d;
    if (a == 255) return s;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t c = ((s >> shift) & 255) + (((d >> shift) & 255) * inv + 127) / 255;
        out |= (c > 255 ? 255 : c) << shift;
    }
    return out;
}

void jvs_blend(uint32_t* target, long target_pitch_pixels, int target_w, int target_h, const uint32_t* image, int image_w,
               int image_h, int x, int y) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + image_w > target_w ? target_w : x + image_w, y1 = y + image_h > target_h ? target_h : y + image_h;
    if (!target || !image) return;
    for (int ty = y0; ty < y1; ty++) {
        uint32_t* d = target + (size_t)ty * (size_t)target_pitch_pixels;
        const uint32_t* s = image + (size_t)(ty - y) * (size_t)image_w - x;
        for (int tx = x0; tx < x1; tx++) d[tx] = over(d[tx], s[tx]);
    }
}

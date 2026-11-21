#include "linux_video_subtitles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_config.h"
#include "jpt_game_version.h"
#include "jpt_hash.h"
#include "jpt_l10n.h"
#include "jpt_tr.h"
#include "jpt_video_subtitles.h"
#include "l10n_assets.h"
#include "linux_log.h"
#include "linux_text.h"

static struct {
    int enabled;
    uint32_t video;
    const jvs_timeline_t* timeline;
    jvs_session_t session;
    int cue;
    int target_w, target_h;
    jvs_rect_t picture;
    uint32_t* image;
    int image_w, image_h, image_x, image_y;
    size_t image_cap, distance_cap;
    uint16_t* distance;
    uint32_t* composed;
    size_t composed_cap;
} S = { .enabled = 1, .cue = -1 };

void lx_vsub_set_enabled(int enabled) { S.enabled = enabled; }

void lx_vsub_init(const char* settings_file, const char* version_id) {
    static jpt_config_t config;
    const jpt_l10n_language_t* language;
    const char* data;
    size_t size;
    int entries = 0;
    jpt_config_defaults(&config);
    if (settings_file) jpt_config_load(settings_file, &config);
    jpt_config_select_language(&config, version_id ? jpt_game_version_by_id(version_id) : NULL);
    S.enabled = config.video_subtitles;
    language = jpt_l10n_language(config.language);
    jpt_tr_reset();
    if (language && strcmp(language->code, JPT_L10N_ENGLISH) && l10n_asset(language->code, JPT_TR_ASSET, &data, &size))
        entries = jpt_tr_load(data, size, language->codepage);
    lx_log("Linux video-subtitles", "%s, language %s%s", S.enabled ? "enabled" : "disabled", language ? language->code : JPT_L10N_ENGLISH,
           entries > 0 ? "" : " (the English text)");
}

static void drop_image(void) {
    S.cue = -1;
    S.image_w = S.image_h = 0;
}

void lx_vsub_reset(void) {
    jvs_session_end(&S.session);
    S.timeline = NULL;
    S.video = 0;
    drop_image();
}

static int fingerprint(const smk_source_t* source, uint64_t* size, char sha[65]) {
    uint8_t* head = (uint8_t*)malloc(JVS_FINGERPRINT_BYTES);
    uint32_t got;
    if (!head) return 0;
    got = source->read_at(source->context, 0, head, JVS_FINGERPRINT_BYTES);
    if (got) jpt_sha256_buffer_hex(head, got, sha);
    free(head);
    *size = source->size;
    return got != 0;
}

void lx_vsub_opened(uint32_t video, const char* guest_name, const smk_source_t* source) {
    char name[64];
    const jvs_timeline_t* timeline;
    uint64_t size = 0;
    char sha[65] = "";
    const char* base = guest_name ? guest_name : "";
    lx_vsub_reset();
    if (!S.enabled || !base[0] || !source) return;
    for (const char* p = guest_name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    snprintf(name, sizeof name, "%s", base);
    if (!jvs_timeline_named(name)) return;
    if (!fingerprint(source, &size, sha)) {
        lx_log("Linux video-subtitles", "%s: the file could not be read, subtitles skipped", name);
        return;
    }
    if (jvs_select(S.enabled, name, size, sha, &timeline) != JVS_SELECTED) {
        lx_log("Linux video-subtitles", "%s: unknown replacement video, subtitles skipped", name);
        return;
    }
    lx_vsub_begin_timeline(video, timeline);
    lx_log("Linux video-subtitles", "%s timeline: %d cues", timeline->name, timeline->cue_count);
}

void lx_vsub_begin_timeline(uint32_t video, const jvs_timeline_t* timeline) {
    lx_vsub_reset();
    S.timeline = timeline;
    S.video = video;
    jvs_session_begin(&S.session, timeline);
}

void lx_vsub_closed(uint32_t video) {
    if (video == S.video) lx_vsub_reset();
}

const char* lx_vsub_text(int cue) {
    if (!S.timeline || cue < 0 || cue >= S.timeline->cue_count) return NULL;
    return jpt_tr(S.timeline->cues[cue].key);
}

static int next_codepoint(const char* s, int bytes, int* length) {
    unsigned char c = (unsigned char)s[0];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1, cp;
    if (n > bytes) n = 1;
    cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int i = 1; i < n; i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    *length = n;
    return n == 1 && c >= 0x80 ? '?' : cp;
}

static int text_width(void* ctx, const char* utf8, int bytes) {
    float scale = *(const float*)ctx;
    int width = 0;
    for (int i = 0; i < bytes;) {
        int n;
        int cp = next_codepoint(utf8 + i, bytes - i, &n);
        width += lx_text_unicode_advance(scale, cp);
        i += n;
    }
    return width;
}

typedef struct {
    uint32_t* pixels;
    int w, h;
} canvas_t;

static void put_coverage(void* ctx, int x, int y, int coverage) {
    canvas_t* c = (canvas_t*)ctx;
    uint32_t* p;
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    p = &c->pixels[(size_t)y * (size_t)c->w + (size_t)x];
    if ((int)(*p & 0xFF) < coverage) *p = (uint32_t)coverage;
}

static void* grow(void* p, size_t* capacity, size_t need) {
    void* q;
    if (need <= *capacity) return p;
    q = realloc(p, need);
    if (q) *capacity = need;
    return q;
}

static int render(const char* text, int target_w, int target_h, const jvs_rect_t* picture) {
    jvs_line_t lines[JVS_MAX_LINES];
    lx_text_metrics_t metrics;
    uint32_t* glyphs;
    canvas_t canvas;
    int px = jvs_font_px(picture), r = jvs_outline_px(px), count, line_h, block_w = 0, block_h, pad;
    float scale = lx_text_scale(-px);
    jvs_rect_t at;
    size_t pixels;
    lx_text_metrics(scale, &metrics);
    count = jvs_wrap(text, jvs_max_line_width(picture, target_w) - 2 * r, text_width, &scale, lines);
    line_h = metrics.height;
    pad = r + 2;
    for (int i = 0; i < count; i++)
        if (lines[i].width > block_w) block_w = lines[i].width;
    block_w += 2 * pad;
    block_h = count * line_h + 2 * pad;
    if (!count || block_w <= 2 * pad) return 0;
    pixels = (size_t)block_w * (size_t)block_h;
    S.image = (uint32_t*)grow(S.image, &S.image_cap, pixels * 4);
    S.distance = (uint16_t*)grow(S.distance, &S.distance_cap, pixels * 2);
    glyphs = (uint32_t*)calloc(pixels, 4);
    if (!glyphs || S.image_cap < pixels * 4 || S.distance_cap < pixels * 2) {
        free(glyphs);
        return 0;
    }
    canvas.pixels = glyphs, canvas.w = block_w, canvas.h = block_h;
    for (int i = 0; i < count; i++) {
        int x = (block_w - lines[i].width) / 2;
        for (int at_byte = lines[i].start; at_byte < lines[i].start + lines[i].length;) {
            int n;
            int cp = next_codepoint(text + at_byte, lines[i].start + lines[i].length - at_byte, &n);
            lx_text_unicode_glyph(scale, cp, x, pad + i * line_h + metrics.ascent, put_coverage, &canvas);
            x += lx_text_unicode_advance(scale, cp);
            at_byte += n;
        }
    }
    jvs_outline(glyphs, block_w, block_h, r, S.distance, S.image);
    free(glyphs);
    at = jvs_place(target_w, target_h, picture, block_w, block_h);
    S.image_x = at.x, S.image_y = at.y, S.image_w = block_w, S.image_h = block_h;
    return 1;
}

void lx_vsub_frame(uint32_t video, uint32_t frame, int x, int y, int w, int h) {
    jvs_rect_t picture = { x, y, w, h };
    int cue;
    if (!S.timeline || video != S.video) return;
    cue = jvs_session_cue(&S.session, (int32_t)frame);
    if (cue < 0) {
        drop_image();
        return;
    }
    if (cue != S.cue || memcmp(&picture, &S.picture, sizeof picture) != 0) {
        S.cue = cue;
        S.picture = picture;
        S.image_w = 0;
    }
}

int lx_vsub_cue(void) { return S.timeline ? S.cue : -1; }

static uint32_t from_565(uint16_t v) {
    uint32_t r = (uint32_t)(v >> 11) & 31u, g = (uint32_t)(v >> 5) & 63u, b = (uint32_t)v & 31u;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

int lx_vsub_compose(const lx_frame_t* in, lx_frame_t* out) {
    size_t pixels;
    if (!S.timeline || S.cue < 0 || !in->pixels || in->width <= 0 || in->height <= 0) return 0;
    if (S.image_w == 0 || S.target_w != in->width || S.target_h != in->height) {
        const char* text = lx_vsub_text(S.cue);
        S.target_w = in->width, S.target_h = in->height;
        if (!text || !render(text, in->width, in->height, &S.picture)) return 0;
    }
    pixels = (size_t)in->width * (size_t)in->height;
    S.composed = (uint32_t*)grow(S.composed, &S.composed_cap, pixels * 4);
    if (S.composed_cap < pixels * 4) return 0;
    for (int y = 0; y < in->height; y++) {
        const uint8_t* row = (const uint8_t*)in->pixels + (size_t)y * (size_t)in->pitch;
        uint32_t* dst = S.composed + (size_t)y * (size_t)in->width;
        if (in->format == LX_PF_RGB565)
            for (int x = 0; x < in->width; x++) dst[x] = from_565(((const uint16_t*)row)[x]);
        else
            memcpy(dst, row, (size_t)in->width * 4);
    }
    jvs_blend(S.composed, in->width, in->width, in->height, S.image, S.image_w, S.image_h, S.image_x, S.image_y);
    *out = *in;
    out->pixels = S.composed;
    out->pitch = in->width * 4;
    out->format = LX_PF_XRGB8888;
    return 1;
}

#include "video_layout.h"

#include <string.h>

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int vl_smk_parse(const uint8_t* header, uint32_t size, vl_smk_info_t* out) {
    uint32_t rate;
    memset(out, 0, sizeof *out);
    if (!header || size < VL_SMK_HEADER_BYTES || memcmp(header, "SMK", 3) || (header[3] != '2' && header[3] != '4'))
        return 0;
    out->width = le32(header + 0x04);
    out->height = le32(header + 0x08);
    out->frames = le32(header + 0x0C);
    out->frame_rate = (int32_t)le32(header + 0x10);
    out->flags = le32(header + 0x14);
    rate = le32(header + 0x48);
    if (rate & 0x40000000u) {
        out->audio_rate = rate & 0x00FFFFFFu;
        out->audio_bits = (rate & 0x20000000u) ? 16 : 8;
        out->audio_channels = (rate & 0x10000000u) ? 2 : 1;
        out->audio_compressed = (rate & 0x80000000u) != 0;
    }
    return out->width > 0 && out->height > 0 && out->frames > 0;
}

double vl_smk_ms_per_frame(int32_t frame_rate) {
    if (frame_rate > 0) return (double)frame_rate;
    if (frame_rate < 0) return -(double)frame_rate / 100.0;
    return 100.0;
}

uint32_t vl_smk_shown_rows(uint32_t stored_rows, uint32_t flags) {
    return (flags & (VL_SMK_Y_INTERLACED | VL_SMK_Y_DOUBLED)) ? stored_rows * 2 : stored_rows;
}

vl_policy_t vl_policy_for(uint32_t flags, uint32_t shown_rows) {
    vl_policy_t p;
    p.shown_rows = (int)shown_rows;
    p.parity = 0;
    if ((flags & (VL_SMK_Y_INTERLACED | VL_SMK_Y_DOUBLED)) && shown_rows >= 2) {
        p.rows = VL_ROWS_REBUILD;
        p.stored_rows = (int)(shown_rows / 2);
    } else {
        p.rows = VL_ROWS_PROGRESSIVE;
        p.stored_rows = (int)shown_rows;
    }
    return p;
}

const char* vl_policy_text(const vl_policy_t* p) {
    return p->rows == VL_ROWS_REBUILD ? "off (not interlaced: half-height rows, rebuilt edge-directed)"
                                      : "off (progressive)";
}

vl_layout_t vl_layout(int out_w, int out_h, int content_w, int content_h) {
    vl_layout_t l;
    memset(&l, 0, sizeof l);
    if (out_w <= 0 || out_h <= 0 || content_w <= 0 || content_h <= 0) return l;
    l.video.w = out_w;
    l.video.h = (int)(((long long)out_w * content_h + content_w / 2) / content_w);
    if (l.video.h > out_h) {
        l.video.h = out_h;
        l.video.w = (int)(((long long)out_h * content_w + content_h / 2) / content_h);
    }
    if (out_h - l.video.h < 2) l.video.h = out_h;
    if (out_w - l.video.w < 2) l.video.w = out_w;
    l.video.x = (out_w - l.video.w) / 2;
    l.video.y = (out_h - l.video.h) / 2;
    l.side = l.video.x;
    l.bars = l.video.y;
    return l;
}

void vl_output_size(int client_w, int client_h, int box_w, int box_h, int* w, int* h) {
    long long cw = client_w, ch = client_h, bw = box_w, bh = box_h;
    *w = *h = 0;
    if (client_w <= 0 || client_h <= 0) return;
    *w = client_w;
    *h = client_h;
    if (box_w <= 0 || box_h <= 0) return;
    bh = (bw * ch + cw / 2) / cw;
    if (bh > box_h) {
        bh = box_h;
        bw = (bh * cw + ch / 2) / ch;
    }
    if (bw < client_w && bh < client_h && bw > 0 && bh > 0) {
        *w = (int)bw;
        *h = (int)bh;
    }
}

void vl_display_name(const char* guest_name, char* out, int cap) {
    const char* base = guest_name;
    int n;
    if (cap <= 0) return;
    out[0] = 0;
    if (!guest_name) return;
    for (const char* p = guest_name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    n = (int)strlen(base);
    if (n > cap - 5) n = cap - 5;
    if (n < 0) n = 0;
    memcpy(out, base, (size_t)n);
    out[n] = 0;
    if (!strchr(out, '.')) strcat(out, ".smk");
}

uint16_t vl_rgb565(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }

int vl_palette_map_update(vl_palette_map_t* m, const uint8_t* palette) {
    if (m->valid && !memcmp(m->palette, palette, sizeof m->palette)) return 0;
    for (int i = 0; i < m->count; i++) m->map[m->codes[i]] = 0;
    m->count = 0;
    for (int i = 0; i < 256; i++) {
        const uint8_t* c = palette + i * 3;
        uint16_t code = vl_rgb565(c[0], c[1], c[2]);
        if (m->map[code]) continue;
        m->map[code] = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        m->codes[m->count++] = code;
    }
    memcpy(m->palette, palette, sizeof m->palette);
    m->valid = 1;
    return 1;
}

const char* vl_play_mode_name(vl_play_mode_t mode) {
    return mode == VL_PLAY_ORIGINAL ? "ORIGINAL" : mode == VL_PLAY_HD ? "HD" : "AUTO";
}

vl_play_mode_t vl_play_mode_take(vl_play_mode_t* forced) {
    vl_play_mode_t mode = *forced;
    *forced = VL_PLAY_AUTO;
    return mode;
}

int vl_play_uses_session(vl_play_mode_t mode) { return mode != VL_PLAY_ORIGINAL; }

void vl_session_begin(vl_session_t* s) {
    int overlay = s->overlay;
    memset(s, 0, sizeof *s);
    s->overlay = overlay;
    s->playing = 1;
    s->last_frame = -1;
}

vl_show_t vl_session_route(const vl_session_t* s, int hardware, int raster_ready, int first_pending, int buffer_path) {
    if (!s->playing || !hardware || s->failed || buffer_path || !raster_ready) return VL_SHOW_CLASSIC;
    return first_pending ? VL_SHOW_BLACK : VL_SHOW_FRAME;
}

void vl_session_shown(vl_session_t* s, vl_show_t what, int32_t frame_number, double ms) {
    if (what == VL_SHOW_CLASSIC) return;
    s->overlay = 1;
    if (what != VL_SHOW_FRAME) return;
    s->started = 1;
    s->frames++;
    if (s->last_frame >= 0 && frame_number > s->last_frame + 1) s->dropped += (unsigned)(frame_number - s->last_frame - 1);
    if (frame_number > s->last_frame) s->last_frame = frame_number;
    if (ms > s->max_ms) s->max_ms = ms;
}

void vl_session_failed(vl_session_t* s) { s->failed = 1; }

void vl_session_end(vl_session_t* s) { s->playing = 0; }

int vl_session_main_presented(vl_session_t* s) {
    if (!s->overlay || (s->playing && !s->failed)) return 0;
    s->overlay = 0;
    return 1;
}

int vl_lone_bottom_pixels(const uint8_t* rows, long stride, int width, int height) {
    const uint16_t *last, *above[3];
    int lone = 0;
    if (!rows || width <= 0 || height < 4) return 0;
    last = (const uint16_t*)(rows + (size_t)(height - 1) * stride);
    for (int k = 0; k < 3; k++) above[k] = (const uint16_t*)(rows + (size_t)(height - 2 - k) * stride);
    for (int x = 0; x < width; x++) lone += last[x] && !above[0][x] && !above[1][x] && !above[2][x];
    return lone;
}

int vl_session_stray_line(vl_session_t* s, int lone_pixels) {
    if (s->stray_line || lone_pixels < VL_STRAY_LINE_MIN) return 0;
    s->stray_line = 1;
    return 1;
}

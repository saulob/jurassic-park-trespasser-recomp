/*
 * Subtitles over the retail videos. The cue follows the Smacker frame the player shows, so it pauses, ends and skips
 * with the video. The text is drawn with the face of the game's own caption font at the target's resolution.
 */
#include "guest_va.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_hash.h"
#include "jpt_tr.h"
#include "jpt_video_subtitles.h"
#include "video_layout.h"

/* The face CreateFontA receives in the gameplay text font's constructor (0x0050E750). */
#define GAME_TEXT_FACE_VA GVA(0x0065F888)

static struct {
    int enabled;
    char game_dir[MAX_PATH];
    char name[64];
    jvs_session_t session;
    const jvs_timeline_t* timeline;
    int cue;
    int target_w, target_h;
    jvs_rect_t picture;
    video_subtitle_image_t image;
    uint32_t* pixels;
    size_t pixel_cap;
    uint16_t* distance;
    size_t distance_cap;
    HFONT font;
    int font_px;
    WCHAR face[LF_FACESIZE];
    HDC dc;
} S = { .cue = -1 };

void video_subtitles_configure(int enabled, const char* game_dir) {
    S.enabled = enabled;
    snprintf(S.game_dir, sizeof S.game_dir, "%s", game_dir ? game_dir : "");
    host_log("video-subtitles", enabled ? "enabled" : "disabled");
}

static int read_head(const char* path, uint64_t* size, char sha[65]) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER length;
    DWORD got = 0;
    uint8_t* head;
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    head = (uint8_t*)malloc(JVS_FINGERPRINT_BYTES);
    ok = head && GetFileSizeEx(h, &length) && ReadFile(h, head, JVS_FINGERPRINT_BYTES, &got, NULL);
    CloseHandle(h);
    if (ok) {
        *size = (uint64_t)length.QuadPart;
        jpt_sha256_buffer_hex(head, got, sha);
    }
    free(head);
    return ok;
}

/* The file the game opens: its name under the data folder, as the Data Drive is the game directory. */
static int fingerprint(const char* guest_name, uint64_t* size, char sha[65]) {
    const char* ext = strrchr(guest_name, '.');
    const char* suffix = ext && !strchr(ext, '\\') ? "" : ".smk";
    char path[MAX_PATH * 2];
    if (strchr(guest_name, ':') || guest_name[0] == '\\') {
        snprintf(path, sizeof path, "%s%s", guest_name, suffix);
        return read_head(path, size, sha);
    }
    snprintf(path, sizeof path, "%s\\data\\%s%s", S.game_dir, guest_name, suffix);
    if (read_head(path, size, sha)) return 1;
    snprintf(path, sizeof path, "%s\\%s%s", S.game_dir, guest_name, suffix);
    return read_head(path, size, sha);
}

void video_subtitles_begin(const char* guest_name) {
    const jvs_timeline_t* t;
    uint64_t size = 0;
    char sha[65] = "";
    video_subtitles_end();
    if (!S.enabled || !guest_name || !guest_name[0]) return;
    vl_display_name(guest_name, S.name, sizeof S.name);
    t = jvs_timeline_named(S.name);
    if (!t) return;
    if (!fingerprint(guest_name, &size, sha)) {
        host_log("video-subtitles", "%s: the file could not be read, subtitles skipped", S.name);
        return;
    }
    if (jvs_select(S.enabled, S.name, size, sha, &t) != JVS_SELECTED) {
        host_log("video-subtitles", "%s: unknown replacement video, subtitles skipped", S.name);
        return;
    }
    S.timeline = t;
    jvs_session_begin(&S.session, t);
    host_log("video-subtitles", "%s timeline: %d cues", t->name, t->cue_count);
}

void video_subtitles_end(void) {
    jvs_session_end(&S.session);
    S.timeline = NULL;
    S.cue = -1;
    S.image.pixels = NULL;
}

static int ensure_font(int px) {
    if (S.font && S.font_px == px) return 1;
    if (!S.face[0]) {
        char face[LF_FACESIZE] = "";
        if (!guest_read_string(GAME_TEXT_FACE_VA, face, sizeof face) || !face[0]) strcpy(face, "Arial");
        MultiByteToWideChar(CP_ACP, 0, face, -1, S.face, LF_FACESIZE);
    }
    if (!S.dc && !(S.dc = CreateCompatibleDC(NULL))) return 0;
    if (S.font) DeleteObject(S.font);
    S.font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, VARIABLE_PITCH, S.face);
    S.font_px = S.font ? px : 0;
    return S.font != NULL;
}

static int widen(const char* utf8, int bytes, WCHAR* out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, bytes, out, cap);
    return n > 0 ? n : 0;
}

static int text_width(void* ctx, const char* utf8, int bytes) {
    WCHAR w[512];
    SIZE size = { 0, 0 };
    int n = widen(utf8, bytes, w, 512);
    (void)ctx;
    GetTextExtentPoint32W(S.dc, w, n, &size);
    return (int)size.cx;
}

static void* grow(void* p, size_t* cap, size_t need) {
    void* q;
    if (need <= *cap) return p;
    q = realloc(p, need);
    if (q) *cap = need;
    return q;
}

static int render(const char* text, int target_w, int target_h, const jvs_rect_t* picture) {
    jvs_line_t lines[JVS_MAX_LINES];
    TEXTMETRICW tm;
    BITMAPINFO bi;
    HBITMAP bitmap;
    HGDIOBJ old_bitmap, old_font;
    uint32_t* glyphs = NULL;
    int px = jvs_font_px(picture), r = jvs_outline_px(px), count, line_h, block_w = 0, block_h, pad;
    jvs_rect_t at;
    if (!ensure_font(px)) return 0;
    old_font = SelectObject(S.dc, S.font);
    GetTextMetricsW(S.dc, &tm);
    count = jvs_wrap(text, jvs_max_line_width(picture, target_w) - 2 * r, text_width, NULL, lines);
    line_h = tm.tmHeight + tm.tmExternalLeading;
    pad = r + 2;
    for (int i = 0; i < count; i++)
        if (lines[i].width > block_w) block_w = lines[i].width;
    block_w += 2 * pad;
    block_h = count * line_h + 2 * pad;
    if (!count || block_w <= 2 * pad) {
        SelectObject(S.dc, old_font);
        return 0;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = block_w;
    bi.bmiHeader.biHeight = -block_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bitmap = CreateDIBSection(S.dc, &bi, DIB_RGB_COLORS, (void**)&glyphs, NULL, 0);
    S.pixels = (uint32_t*)grow(S.pixels, &S.pixel_cap, (size_t)block_w * block_h * 4);
    S.distance = (uint16_t*)grow(S.distance, &S.distance_cap, (size_t)block_w * block_h * 2);
    if (!bitmap || !glyphs || S.pixel_cap < (size_t)block_w * block_h * 4 ||
        S.distance_cap < (size_t)block_w * block_h * 2) {
        if (bitmap) DeleteObject(bitmap);
        SelectObject(S.dc, old_font);
        return 0;
    }
    old_bitmap = SelectObject(S.dc, bitmap);
    memset(glyphs, 0, (size_t)block_w * block_h * 4);
    SetBkMode(S.dc, TRANSPARENT);
    SetTextColor(S.dc, RGB(255, 255, 255));
    SetTextAlign(S.dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    for (int i = 0; i < count; i++) {
        WCHAR w[512];
        int n = widen(text + lines[i].start, lines[i].length, w, 512);
        TextOutW(S.dc, (block_w - lines[i].width) / 2, pad + i * line_h, w, n);
    }
    GdiFlush();
    jvs_outline(glyphs, block_w, block_h, r, S.distance, S.pixels);
    SelectObject(S.dc, old_bitmap);
    SelectObject(S.dc, old_font);
    DeleteObject(bitmap);
    at = jvs_place(target_w, target_h, picture, block_w, block_h);
    S.image.pixels = S.pixels;
    S.image.x = at.x;
    S.image.y = at.y;
    S.image.w = block_w;
    S.image.h = block_h;
    S.image.generation++;
    return 1;
}

static int same_rect(const jvs_rect_t* a, const jvs_rect_t* b) {
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

const video_subtitle_image_t* video_subtitles_image(int target_w, int target_h, int px, int py, int pw, int ph) {
    jvs_rect_t picture = { px, py, pw, ph };
    int32_t frame = 0;
    int x, y, w, h, cue;
    if (!S.timeline || target_w <= 0 || target_h <= 0 || pw <= 0 || ph <= 0 || !video_hd_picture(&frame, &x, &y, &w, &h))
        return NULL;
    cue = jvs_session_cue(&S.session, frame);
    if (cue < 0) {
        S.cue = -1;
        return NULL;
    }
    if (cue != S.cue || target_w != S.target_w || target_h != S.target_h || !same_rect(&picture, &S.picture) ||
        !S.image.pixels) {
        S.cue = cue;
        S.target_w = target_w;
        S.target_h = target_h;
        S.picture = picture;
        S.image.pixels = NULL;
        if (!render(jpt_tr(S.timeline->cues[cue].key), target_w, target_h, &picture)) return NULL;
    }
    return &S.image;
}

const video_subtitle_image_t* video_subtitles_for_screen(int screen_w, int screen_h, int target_w, int target_h) {
    int32_t frame;
    int x, y, w, h;
    if (!S.timeline || screen_w <= 0 || screen_h <= 0 || !video_hd_picture(&frame, &x, &y, &w, &h)) return NULL;
    return video_subtitles_image(target_w, target_h, (int)((long long)x * target_w / screen_w),
                                 (int)((long long)y * target_h / screen_h), (int)((long long)w * target_w / screen_w),
                                 (int)((long long)h * target_h / screen_h));
}

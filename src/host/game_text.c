/*
 * The gameplay text overlay (hints, subtitles, tutorial prompts): font class 0x0050E750 (HFONT +0, tallest glyph +0xC, advance widths of
 * 0x20..0xFE from +0x90), drawn through GetDC by 0x0050EAF0 and 0x0050E830.
 */
#include "guest_va.h"
#include "host.h"

#include <string.h>

#include "jpt_display.h"
#include "jpt_l10n.h"
#include "text_layout.h"
#include "tutorial_prompts.h"

#define FONT_CTOR_VA GVA(0x0050E750)
#define TEXT_DRAW_VA GVA(0x0050EAF0)
#define TEXT_DRAW_LINE_VA GVA(0x0050E830)
#define TEXT_LAYOUT_VA GVA(0x0050E8C0)
#define RASTER_PTR GVA(0x0073DC40)
#define RASTER_WIDTH 0x08u
#define RASTER_HEIGHT 0x0Cu
#define RASTER_LEFT 0x88u
#define RASTER_TOP 0x8Cu
#define TEXT_SCAN_MAX 0x10000u
#define FONT_HFONT 0x00u
#define FONT_TALLEST 0x0Cu
#define FONT_WIDTHS 0x90u
#define FONT_WIDTH_BASE 0x10u
#define FONT_FIRST_CHAR 0x20
#define FONT_END_CHAR 0xFF
/* Both draws call TextOutA through this import slot. */
#define TEXTOUT_IAT GVA(0x0064506C)

static recomp_func_t s_ctor_lifted, s_draw_lifted, s_draw_line_lifted, s_layout_lifted;
static int s_drawing, s_logged, s_font_hooked, s_boxes_known = -1, s_layouts_logged;
static struct {
    HFONT font;
    uint32_t width;
} s_last_glyph[4];

/* Aliased glyphs make the draw a pure write (see GETDC_CALLER_TEXT_OVERLAY); the widths are measured again because
 * the game lays out with them. */
static void make_aliased(uint32_t font) {
    HFONT old = (HFONT)(uintptr_t)MEM32(font + FONT_HFONT), aliased;
    LOGFONTA lf;
    HDC dc;
    HGDIOBJ previous;
    LONG tallest = 0;
    if (!old || GetObjectA(old, sizeof lf, &lf) != sizeof lf) return;
    lf.lfQuality = NONANTIALIASED_QUALITY;
    aliased = CreateFontIndirectA(&lf);
    if (!aliased) return;
    dc = GetDC(NULL);
    previous = SelectObject(dc, aliased);
    for (int c = FONT_FIRST_CHAR; c < FONT_END_CHAR; c++) {
        char ch = (char)c;
        SIZE size = { 0, 0 };
        GetTextExtentPointA(dc, &ch, 1, &size);
        MEM32(font + FONT_WIDTHS + (uint32_t)(c - FONT_FIRST_CHAR) * 4) = (uint32_t)size.cx;
        if (size.cy > tallest) tallest = size.cy;
    }
    SelectObject(dc, previous);
    ReleaseDC(NULL, dc);
    MEM32(font + FONT_TALLEST) = (uint32_t)tallest;
    MEM32(font + FONT_HFONT) = (uint32_t)(uintptr_t)aliased;
    DeleteObject(old);
}

/* The width table stops at 0xFE, but 0xFF is a letter in both code pages (y diaeresis, Cyrillic ya). */
static uint32_t last_glyph_width(HFONT font) {
    static int next;
    char ch = (char)0xFF;
    SIZE size = { 0, 0 };
    HDC dc;
    HGDIOBJ previous;
    for (int i = 0; i < 4; i++)
        if (s_last_glyph[i].font == font) return s_last_glyph[i].width;
    dc = GetDC(NULL);
    previous = SelectObject(dc, font);
    GetTextExtentPointA(dc, &ch, 1, &size);
    SelectObject(dc, previous);
    ReleaseDC(NULL, dc);
    s_last_glyph[next].font = font;
    s_last_glyph[next].width = (uint32_t)size.cx;
    next = (next + 1) % 4;
    return (uint32_t)size.cx;
}

static uint32_t glyph_width(void* ctx, unsigned char c) {
    uint32_t font = (uint32_t)(uintptr_t)ctx;
    if (c >= FONT_FIRST_CHAR && c < FONT_END_CHAR) return MEM32(font + FONT_WIDTHS + (uint32_t)(c - FONT_FIRST_CHAR) * 4);
    if (c == 0xFF) return last_glyph_width((HFONT)(uintptr_t)MEM32(font + FONT_HFONT));
    return MEM32(font + FONT_WIDTH_BASE + (uint32_t)c * 4);
}

static int has_high_bytes(const unsigned char* s) {
    for (uint32_t i = 0; i < TEXT_SCAN_MAX && s[i]; i++)
        if (s[i] >= 0x80) return 1;
    return 0;
}

/* thiscall (font in ecx): text, blob, flags; returns the blob's end. */
static void text_layout(void) {
    uint32_t font = g_ecx, text = GUEST_ARG(0), out = GUEST_ARG(1), flags = GUEST_ARG(2), raster;
    tl_view_t view;
    size_t size;
    if (g_alias_entry || !text || !out || !has_high_bytes((const unsigned char*)ADDR(text)) ||
        !(raster = MEM32(RASTER_PTR))) {
        s_layout_lifted();
        return;
    }
    view.width = (int32_t)MEM32(raster + RASTER_WIDTH);
    view.height = (int32_t)MEM32(raster + RASTER_HEIGHT);
    view.left = (int32_t)MEM32(raster + RASTER_LEFT);
    view.top = (int32_t)MEM32(raster + RASTER_TOP);
    view.line_height = (int32_t)MEM32(font + FONT_TALLEST);
    size = tl_layout((const char*)ADDR(text), flags & 0xFFu, &view, glyph_width, (void*)(uintptr_t)font,
                     (uint8_t*)ADDR(out), 8 + (strlen((const char*)ADDR(text)) + 1) * (TL_RECORD + 2));
    if (s_layouts_logged++ < 4)
        host_log("text", "laid out translated text with unsigned glyph widths (%u lines)", MEM32(out + 4));
    g_eax = out + (uint32_t)size;
    GUEST_RETURN_STDCALL(3);
}

static void font_ctor(void) {
    uint32_t font = g_ecx, slot = g_esp + 4;
    int height = (int)MEM32(slot), screen_w = 0, screen_h = 0, scaled;
    if (g_alias_entry) {
        s_ctor_lifted();
        return;
    }
    window_host_display_size(&screen_w, &screen_h);
    scaled = jpt_ui_text_height(height, screen_h);
    MEM32(slot) = (uint32_t)scaled;
    s_ctor_lifted();
    make_aliased(font);
    if (s_logged++ < 4)
        host_log("text", "gameplay text font %d px on a %dx%d screen (the game asked for %d px), aliased", scaled,
                 screen_w, screen_h, height);
}

static void text_draw(void) {
    s_drawing++;
    if (g_alias_entry || !tutorial_prompts_draw(s_draw_lifted)) s_draw_lifted();
    s_drawing--;
}

static void text_draw_line(void) {
    s_drawing++;
    s_draw_line_lifted();
    s_drawing--;
}

int game_text_drawing(void) { return s_font_hooked && s_drawing > 0; }

int game_text_boxes_known(void) {
    if (s_boxes_known < 0) s_boxes_known = recomp_lookup_import(MEM32(TEXTOUT_IAT)) == shim_TextOutA;
    return s_boxes_known;
}

/* The device pixels TextOutA can change: the string's extent from (x, y), widened by the widest glyph and its
 * overhang either side. 0 when the DC's alignment or font could put the text elsewhere. */
static int text_box(HDC dc, int x, int y, const char* text, int count, RECT* box) {
    SIZE size;
    TEXTMETRICA tm;
    LOGFONTA lf;
    POINT corner[2];
    HGDIOBJ font = GetCurrentObject(dc, OBJ_FONT);
    LONG wide;
    if ((GetTextAlign(dc) & (TA_UPDATECP | TA_CENTER | TA_BASELINE)) || !font ||
        GetObjectA(font, sizeof lf, &lf) != sizeof lf || lf.lfEscapement || lf.lfOrientation ||
        !GetTextExtentPoint32A(dc, text, count, &size) || !GetTextMetricsA(dc, &tm))
        return 0;
    wide = tm.tmMaxCharWidth + tm.tmOverhang;
    corner[0].x = x - wide;
    corner[0].y = y - 2;
    corner[1].x = x + size.cx + wide;
    corner[1].y = y + size.cy + 2;
    if (!LPtoDP(dc, corner, 2)) return 0;
    box->left = corner[0].x < corner[1].x ? corner[0].x : corner[1].x;
    box->right = corner[0].x < corner[1].x ? corner[1].x : corner[0].x;
    box->top = corner[0].y < corner[1].y ? corner[0].y : corner[1].y;
    box->bottom = corner[0].y < corner[1].y ? corner[1].y : corner[0].y;
    return 1;
}

void shim_TextOutA(void) {
    HDC dc = (HDC)GUEST_PTR(0);
    int x = (int)GUEST_ARG(1), y = (int)GUEST_ARG(2), count = (int)GUEST_ARG(4);
    const char* text = (const char*)GUEST_PTR(3);
    RECT box;
    if (s_drawing > 0 && count > 0 && tutorial_prompts_textout(dc, x, y, text, count)) {
        g_eax = TRUE;
        GUEST_RETURN_STDCALL(5);
        return;
    }
    if (s_drawing > 0 && count > 0) d3dr_text_box(dc, text_box(dc, x, y, text, count, &box) ? &box : NULL);
    g_eax = (uint32_t)TextOutA(dc, x, y, text, count);
    GUEST_RETURN_STDCALL(5);
}

void game_text_install(void) {
    if (jpt_l10n_display_codepage() && !guest_override_install(TEXT_LAYOUT_VA, text_layout, &s_layout_lifted))
        host_log("text", "the text layout override is missing: translated text keeps retail's signed glyph widths");
    const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
    } k[] = {
        { FONT_CTOR_VA, font_ctor, &s_ctor_lifted },
        { TEXT_DRAW_VA, text_draw, &s_draw_lifted },
        { TEXT_DRAW_LINE_VA, text_draw_line, &s_draw_line_lifted },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        int hooked = guest_override_install(k[i].va, k[i].replacement, k[i].lifted);
        if (i == 0) s_font_hooked = hooked;
    }
}

/*
 * The gameplay text overlay (hints, subtitles): font class 0x0050E750 (HFONT +0, tallest glyph +0xC, advance widths of
 * 0x20..0xFE from +0x90), drawn through GetDC by 0x0050EAF0 and 0x0050E830.
 */
#include "host.h"

#include "jpt_display.h"

#define FONT_CTOR_VA 0x0050E750u
#define TEXT_DRAW_VA 0x0050EAF0u
#define TEXT_DRAW_LINE_VA 0x0050E830u
#define FONT_HFONT 0x00u
#define FONT_TALLEST 0x0Cu
#define FONT_WIDTHS 0x90u
#define FONT_FIRST_CHAR 0x20
#define FONT_END_CHAR 0xFF

static recomp_func_t s_ctor_lifted, s_draw_lifted, s_draw_line_lifted;
static int s_drawing, s_logged, s_font_hooked;

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
    s_draw_lifted();
    s_drawing--;
}

static void text_draw_line(void) {
    s_drawing++;
    s_draw_line_lifted();
    s_drawing--;
}

int game_text_drawing(void) { return s_font_hooked && s_drawing > 0; }

void game_text_install(void) {
    static const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
        const char* name;
    } k[] = {
        { FONT_CTOR_VA, font_ctor, &s_ctor_lifted, "text font" },
        { TEXT_DRAW_VA, text_draw, &s_draw_lifted, "text draw" },
        { TEXT_DRAW_LINE_VA, text_draw_line, &s_draw_line_lifted, "text line draw" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        int hooked = guest_override_redirect(k[i].va, k[i].replacement, k[i].lifted);
        if (i == 0) s_font_hooked = hooked;
        if (!hooked)
            host_log("text", "sub_%08X (%s) has no incremental-link thunk: the game's own version stays", k[i].va,
                     k[i].name);
    }
}

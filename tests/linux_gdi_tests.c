/* The Linux gdi32 layer, called the way the guest calls it. Device contexts, bitmaps, brushes, pens, fonts and regions are tokens
 * (slot and generation, so a stale one fails); DIB sections live in guest memory; a window's device context draws into that
 * window's client surface, and the picture reaches the host window as an unconverted RGB565 frame (virtual backend). */
#include <stdlib.h>

#include "linux_gdi32.h"
#include "linux_text.h"
#include "linux_user32.h"
#include "linux_win32_harness.h"

#define SRCCOPY 0x00CC0020u
#define W 640
#define H 480

static uint32_t s_hwnd;

static void make_window(void) {
    uint32_t wc[10] = { 8, FAKE_WNDPROC, 0, 0, 0x00400000u, 0, 0, 0, 0, 0 };
    wc[9] = put_string(g_scratch, "GdiTest");
    gp_copy_out(g_scratch + 0x40, wc, sizeof wc);
    U32(RegisterClassA, 1, g_scratch + 0x40);
    s_hwnd = U32(CreateWindowExA, 12, 0u, g_scratch, put_string(g_scratch + 0x100, "gdi"), 0x90080000u, 0u, 0u, (uint32_t)W, (uint32_t)H, 0u, 0u,
                 0x00400000u, 0u);
}

/* BITMAPINFO at where; returns the DIB section token and the guest address of its bits in *bits. */
static uint32_t make_dib(uint32_t where, int width, int height, int bpp, int bitfields, uint32_t* bits) {
    uint32_t bih[10] = { 40, (uint32_t)width, (uint32_t)height, 1u | ((uint32_t)bpp << 16), bitfields ? 3u : 0u, 0, 0, 0, 0, 0 };
    uint32_t masks[3] = { 0xF800, 0x07E0, 0x001F }, out = where + 0x100, bmp;
    gp_copy_out(where, bih, sizeof bih);
    if (bitfields) gp_copy_out(where + 40, masks, sizeof masks);
    bmp = G32(CreateDIBSection, 6, 0u, where, 0u, out, 0u, 0u);
    *bits = rd(out);
    return bmp;
}

static uint16_t pattern(int x, int y) { return (uint16_t)((x * 31 + y * 17) & 0xFFFF); }

static void test_window_dc(void) {
    uint32_t hdc = U32(GetDC, 1, s_hwnd), hdc2;
    CHECK(hdc != 0 && hdc < 0x01000000u, "GetDC returns a token, not a host pointer (0x%08X)", hdc);
    CHECK(U32(GetDC, 1, s_hwnd + 0x100) == 0, "GetDC of a stale window token fails");
    CHECK(G32(GetDeviceCaps, 2, hdc, 12u) == 16 && G32(GetDeviceCaps, 2, hdc, 8u) == W && G32(GetDeviceCaps, 2, hdc, 10u) == H &&
              G32(GetDeviceCaps, 2, hdc, 14u) == 1 && G32(GetDeviceCaps, 2, hdc, 88u) == 96,
          "the display is 16 bpp, 640x480, one plane, 96 dpi");
    CHECK(U32(ReleaseDC, 2, s_hwnd, hdc) == 1, "ReleaseDC");
    CHECK(U32(ReleaseDC, 2, s_hwnd, hdc) == 0, "a released device context is stale");
    hdc2 = U32(GetDC, 1, s_hwnd);
    CHECK(hdc2 != hdc, "the slot is reused with a new generation (0x%X after 0x%X)", hdc2, hdc);
    U32(ReleaseDC, 2, s_hwnd, hdc2);
}

static void test_objects(void) {
    uint32_t hdc = G32(CreateCompatibleDC, 1, 0u), brush, stock = G32(GetStockObject, 1, 4u), old;
    CHECK(hdc != 0 && G32(GetStockObject, 1, 4u) == stock && stock != 0, "GetStockObject returns the same token every time");
    CHECK(G32(DeleteObject, 1, stock) == 1 && G32(GetStockObject, 1, 4u) == stock, "a stock object is not really deleted");
    brush = G32(CreateSolidBrush, 1, 0x0000FFu);
    CHECK(brush != 0 && brush != stock, "CreateSolidBrush");
    old = G32(SelectObject, 2, hdc, brush);
    CHECK(old == G32(GetStockObject, 1, 0u), "SelectObject returns the previous brush (the white stock brush)");
    CHECK(G32(SelectObject, 2, hdc, old) == brush, "and the brush selected before");
    CHECK(G32(DeleteObject, 1, brush) == 1 && G32(DeleteObject, 1, brush) == 0, "DeleteObject frees it once");
    CHECK(G32(SelectObject, 2, hdc, brush) == 0, "a deleted object is stale");
    CHECK(G32(SetBkMode, 2, hdc, 1u) == 2 && G32(SetBkMode, 2, hdc, 2u) == 1, "SetBkMode returns the previous mode (OPAQUE by default)");
    CHECK(G32(SetTextColor, 2, hdc, 0x123456u) == 0 && G32(GetTextColor, 1, hdc) == 0x123456u, "SetTextColor / GetTextColor");
    CHECK(G32(SetBkColor, 2, hdc, 0x654321u) == 0xFFFFFFu, "SetBkColor returns white by default");
    CHECK(G32(SetMapMode, 2, hdc, 1u) == 1 && G32(SetStretchBltMode, 2, hdc, 3u) == 1 && G32(SetTextAlign, 2, hdc, 6u) == 0, "map, stretch and text-align state");
    {
        uint32_t font = G32(CreateFontA, 14, 0xFFFFFFF4u, 0u, 0u, 0u, 400u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u), tm = g_scratch + 0x2000;
        CHECK(font != 0 && G32(SelectObject, 2, hdc, font) == G32(GetStockObject, 1, 13u), "a font selects over the system font");
        CHECK(G32(GetTextMetricsA, 2, hdc, tm) == 1 && rd(tm) >= 12 && rd(tm) <= 16 && rd(tm + 4) > 0 && rd(tm + 4) + rd(tm + 8) == rd(tm), "GetTextMetricsA reports a cell of about the em height (%u)", rd(tm));
        CHECK(G32(GetTextExtentPointA, 4, hdc, put_string(g_scratch + 0x2100, "abcd"), 4u, g_scratch + 0x2200) == 1 && rd(g_scratch + 0x2200) > 0, "GetTextExtentPointA");
    }
    CHECK(G32(DeleteDC, 1, hdc) == 1 && G32(DeleteDC, 1, hdc) == 0, "DeleteDC frees a memory context once");
}

static uint32_t expected_frame(int bottom_up) {
    static uint8_t buf[W * H * 2];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t v = pattern(x, bottom_up ? H - 1 - y : y);
            buf[(y * W + x) * 2] = (uint8_t)v, buf[(y * W + x) * 2 + 1] = (uint8_t)(v >> 8);
        }
    return fnv(buf, sizeof buf);
}

static void fill_pattern(uint32_t bits, int rows_reversed) {
    uint16_t* p = (uint16_t*)gm_ptr_checked(bits, W * H * 2, 1);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) p[y * W + x] = pattern(x, rows_reversed ? H - 1 - y : y);
}

static void test_dib_and_blit(void) {
    uint32_t bits = 0, bmp, mem, win, before;
    const lx_virtual_state_t* v = lx_virtual_state();
    bmp = make_dib(g_scratch + 0x4000, W, -H, 16, 1, &bits);
    CHECK(bmp != 0 && bits != 0 && gm_heap_ok(bits), "CreateDIBSection returns a token and guest memory for the bits (0x%08X)", bits);
    mem = G32(CreateCompatibleDC, 1, 0u);
    G32(SelectObject, 2, mem, bmp);
    fill_pattern(bits, 0);
    win = U32(GetDC, 1, s_hwnd);
    before = v->presents;
    CHECK(G32(BitBlt, 9, win, 0u, 0u, (uint32_t)W, (uint32_t)H, mem, 0u, 0u, SRCCOPY) == 1, "BitBlt from the DIB section to the window context");
    CHECK(v->presents == before + 1 && v->last.format == LX_PF_RGB565 && v->last.width == W && v->last.height == H,
          "the blit presented one RGB565 %dx%d frame", v->last.width, v->last.height);
    CHECK(v->last_checksum == expected_frame(0), "the presented pixels are the DIB's pixels, unconverted (0x%08X)", v->last_checksum);

    {
        uint32_t bits_up = 0, bmp_up = make_dib(g_scratch + 0x8000, W, H, 16, 1, &bits_up), mem_up = G32(CreateCompatibleDC, 1, 0u);
        G32(SelectObject, 2, mem_up, bmp_up);
        fill_pattern(bits_up, 1);
        before = v->presents;
        G32(BitBlt, 9, win, 0u, 0u, (uint32_t)W, (uint32_t)H, mem_up, 0u, 0u, SRCCOPY);
        CHECK(v->presents == before + 1 && v->last_checksum == expected_frame(0), "a bottom-up DIB shows with its rows the right way up");
        G32(DeleteDC, 1, mem_up);
        G32(DeleteObject, 1, bmp_up);
    }
    {
        uint32_t bits24 = 0, bmp24 = make_dib(g_scratch + 0xC000, W, -H, 24, 0, &bits24), mem24 = G32(CreateCompatibleDC, 1, 0u);
        static uint8_t red[W * H * 2];
        uint8_t* p = (uint8_t*)gm_ptr_checked(bits24, W * 3 * H, 1);
        G32(SelectObject, 2, mem24, bmp24);
        for (int i = 0; i < W * H; i++) p[i * 3] = 0, p[i * 3 + 1] = 0, p[i * 3 + 2] = 255;
        for (int i = 0; i < W * H; i++) red[i * 2] = 0x00, red[i * 2 + 1] = 0xF8;
        before = v->presents;
        G32(BitBlt, 9, win, 0u, 0u, (uint32_t)W, (uint32_t)H, mem24, 0u, 0u, SRCCOPY);
        CHECK(v->presents == before + 1 && v->last_checksum == fnv(red, sizeof red), "a 24 bpp DIB converts to RGB565 (red = 0xF800)");
        G32(DeleteDC, 1, mem24);
        G32(DeleteObject, 1, bmp24);
    }
    {
        uint32_t small_bits = 0, small = make_dib(g_scratch + 0x10000, 2, -2, 16, 1, &small_bits), small_dc = G32(CreateCompatibleDC, 1, 0u);
        static uint8_t expect[W * H * 2];
        uint16_t quad[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
        gp_copy_out(small_bits, quad, sizeof quad);
        G32(SelectObject, 2, small_dc, small);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint16_t v16 = quad[(y * 2 / H) * 2 + (x * 2 / W)];
                expect[(y * W + x) * 2] = (uint8_t)v16, expect[(y * W + x) * 2 + 1] = (uint8_t)(v16 >> 8);
            }
        before = v->presents;
        CHECK(G32(StretchBlt, 11, win, 0u, 0u, (uint32_t)W, (uint32_t)H, small_dc, 0u, 0u, 2u, 2u, SRCCOPY) == 1, "StretchBlt");
        CHECK(v->presents == before + 1 && v->last_checksum == fnv(expect, sizeof expect), "a 2x2 DIB stretches to four quadrants");
        G32(DeleteDC, 1, small_dc);
        G32(DeleteObject, 1, small);
    }
    G32(DeleteDC, 1, mem);
    CHECK(G32(BitBlt, 9, win, 0u, 0u, 8u, 8u, mem, 0u, 0u, SRCCOPY) == 0, "a deleted source context is stale");
    CHECK(G32(DeleteObject, 1, bmp) == 1, "the DIB section deletes");
    U32(ReleaseDC, 2, s_hwnd, win);
}

static void test_paint(void) {
    static uint8_t red[W * H * 2];
    uint32_t ps = g_scratch + 0x14000, rect = g_scratch + 0x14100, hdc, brush = G32(CreateSolidBrush, 1, 0x0000FFu);
    const lx_virtual_state_t* v = lx_virtual_state();
    unsigned before = v->presents;
    for (int i = 0; i < W * H; i++) red[i * 2] = 0x00, red[i * 2 + 1] = 0xF8;
    U32(SetRect, 5, rect, 0u, 0u, (uint32_t)W, (uint32_t)H);
    hdc = U32(BeginPaint, 2, s_hwnd, ps);
    CHECK(hdc != 0 && rd(ps) == hdc, "BeginPaint returns the context and stores it in PAINTSTRUCT");
    CHECK(U32(FillRect, 3, hdc, rect, brush) == 1, "FillRect with a solid brush");
    CHECK(U32(EndPaint, 2, s_hwnd, ps) == 1, "EndPaint");
    CHECK(v->presents == before + 1 && v->last_checksum == fnv(red, sizeof red), "EndPaint presents the painted client (red fill)");
    G32(DeleteObject, 1, brush);
}

enum { CW = 200, CH = 80, DT_C = 1, DT_R = 2, DT_VC = 4, DT_WB = 0x10, DT_SL = 0x20, DT_NOCLIP_ = 0x100, DT_CALC = 0x400, WHITE16 = 0xFFFF, RED16 = 0xF800 };

typedef struct {
    uint32_t dc, bmp, bits;
} canvas_t;

static void clear_canvas(uint32_t bits) {
    uint16_t* p = (uint16_t*)gm_ptr_checked(bits, CW * CH * 2, 1);
    for (int i = 0; i < CW * CH; i++) p[i] = WHITE16;
}

static canvas_t make_canvas(uint32_t where) {
    canvas_t c;
    c.bmp = make_dib(where, CW, -CH, 16, 1, &c.bits);
    c.dc = G32(CreateCompatibleDC, 1, 0u);
    G32(SelectObject, 2, c.dc, c.bmp);
    clear_canvas(c.bits);
    return c;
}

static void free_canvas(canvas_t c) {
    G32(DeleteDC, 1, c.dc);
    G32(DeleteObject, 1, c.bmp);
}

static uint16_t px(canvas_t c, int x, int y) { return ((const uint16_t*)gm_ptr_checked(c.bits, CW * CH * 2, 0))[y * CW + x]; }

typedef struct {
    int x0, y0, x1, y1, ink;
} ink_t;

static ink_t ink_in(canvas_t c, int l, int t, int r, int b) {
    ink_t k = { CW, CH, -1, -1, 0 };
    for (int y = t; y < b; y++)
        for (int x = l; x < r; x++)
            if (px(c, x, y) != WHITE16) {
                if (x < k.x0) k.x0 = x;
                if (y < k.y0) k.y0 = y;
                if (x > k.x1) k.x1 = x;
                if (y > k.y1) k.y1 = y;
                k.ink++;
            }
    return k;
}

static uint32_t region_hash(canvas_t c) {
    static uint8_t buf[CW * CH * 2];
    size_t n = 0;
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) buf[n++] = (uint8_t)px(c, x, y), buf[n++] = (uint8_t)(px(c, x, y) >> 8);
    return fnv(buf, n);
}

static uint32_t set_rect(uint32_t where, int l, int t, int r, int b) {
    U32(SetRect, 5, where, (uint32_t)l, (uint32_t)t, (uint32_t)r, (uint32_t)b);
    return where;
}

static uint32_t draw_text(canvas_t c, const char* text, int l, int t, int r, int b, uint32_t flags) {
    uint32_t rc = set_rect(g_scratch + 0x1F000, l, t, r, b);
    return U32(DrawTextExA, 6, c.dc, put_string(g_scratch + 0x1F100, text), 0xFFFFFFFFu, rc, flags, 0u);
}

static void test_text(void) {
    lx_text_metrics_t m;
    uint32_t font = G32(CreateFontA, 14, 0xFFFFFFF0u, 0u, 0u, 0u, 400u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, put_string(g_scratch + 0x1E000, "Arial"));
    canvas_t c = make_canvas(g_scratch + 0x18000);
    uint32_t rc = g_scratch + 0x1F000, ext = g_scratch + 0x1E100;
    int line_h, text_w, lines;
    ink_t k;
    lx_text_metrics(lx_text_scale(-16), &m);
    G32(SelectObject, 2, c.dc, font);
    G32(SetBkMode, 2, c.dc, 1u);
    G32(SetTextColor, 2, c.dc, 0x0000FFu);
    line_h = m.height;
    G32(GetTextExtentPointA, 4, c.dc, put_string(g_scratch + 0x1E200, "Hello World"), 11u, ext);
    text_w = (int)rd(ext);
    CHECK(text_w > 40 && text_w < 150 && (int)rd(ext + 4) == line_h, "GetTextExtentPointA measures with the rasteriser's advances (%d x %d)", text_w, (int)rd(ext + 4));

    CHECK(G32(TextOutA, 5, c.dc, 10u, 10u, put_string(g_scratch + 0x1E200, "Hello World"), 11u) == 1, "TextOutA");
    k = ink_in(c, 0, 0, CW, CH);
    CHECK(k.ink > 100 && k.x0 >= 10 && k.x1 < 10 + text_w + 2 && k.y0 >= 10 && k.y1 < 10 + line_h + 2,
          "TextOutA draws ink inside the string's cell (%d pixels, x %d..%d, y %d..%d)", k.ink, k.x0, k.x1, k.y0, k.y1);
    {
        int exact = 0, off_ramp = 0;
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++) {
                uint16_t v = px(c, x, y);
                exact += v == RED16;
                off_ramp += (v >> 11) != 31;
            }
        CHECK(exact > 10, "the text colour reaches the surface as RGB565 red where coverage is full (%d pixels)", exact);
        CHECK(off_ramp == 0, "every pixel keeps full red: a blend of white and red (%d are not)", off_ramp);
    }
    CHECK(px(c, 2, 2) == WHITE16 && px(c, CW - 1, CH - 1) == WHITE16, "transparent mode leaves the background alone");

    clear_canvas(c.bits);
    CHECK(draw_text(c, "Hello World", 10, 10, 190, 70, 0) == (uint32_t)line_h, "DrawTextExA returns the text height");
    {
        uint32_t via_drawtext = region_hash(c);
        clear_canvas(c.bits);
        G32(TextOutA, 5, c.dc, 10u, 10u, put_string(g_scratch + 0x1E200, "Hello World"), 11u);
        CHECK(via_drawtext == region_hash(c), "DrawTextExA (left, top) and TextOutA draw the same pixels: one rasteriser");
    }

    clear_canvas(c.bits);
    CHECK(draw_text(c, "Hello World", 20, 5, 120, 60, DT_CALC) == (uint32_t)line_h, "DT_CALCRECT returns the height");
    CHECK((int)rd(rc) == 20 && (int)rd(rc + 4) == 5 && (int)rd(rc + 8) == 20 + text_w && (int)rd(rc + 12) == 5 + line_h,
          "DT_CALCRECT rewrites the guest RECT to the text's extent (%d,%d,%d,%d)", (int)rd(rc), (int)rd(rc + 4), (int)rd(rc + 8), (int)rd(rc + 12));
    CHECK(ink_in(c, 0, 0, CW, CH).ink == 0, "DT_CALCRECT draws nothing");

    draw_text(c, "Hello World", 20, 5, 120, 60, DT_C);
    k = ink_in(c, 0, 0, CW, CH);
    CHECK(k.ink > 0 && abs((k.x0 + k.x1) / 2 - 70) <= 3, "DT_CENTER centres the line in the rectangle (ink %d..%d, centre 70)", k.x0, k.x1);
    clear_canvas(c.bits);
    draw_text(c, "Hello World", 20, 5, 120, 60, DT_R);
    k = ink_in(c, 0, 0, CW, CH);
    CHECK(k.ink > 0 && k.x1 <= 119 && k.x1 >= 115, "DT_RIGHT puts the line against the right edge (ink ends at %d)", k.x1);

    clear_canvas(c.bits);
    draw_text(c, "Hello World", 20, 10, 150, 70, DT_C | DT_VC | DT_SL);
    k = ink_in(c, 0, 0, CW, CH);
    CHECK(k.ink > 0 && abs((k.y0 + k.y1) / 2 - 40) <= line_h / 2, "DT_VCENTER | DT_SINGLELINE centres vertically (ink rows %d..%d around 40)", k.y0, k.y1);
    clear_canvas(c.bits);
    draw_text(c, "Hello World", 20, 10, 150, 70, DT_C | DT_VC);
    k = ink_in(c, 0, 0, CW, CH);
    CHECK(k.ink > 0 && k.y0 < 10 + line_h, "DT_VCENTER without DT_SINGLELINE is ignored, as Win32 does");

    clear_canvas(c.bits);
    draw_text(c, "one two three four five six seven", 10, 10, 70, 78, DT_WB | DT_CALC);
    lines = ((int)rd(rc + 12) - (int)rd(rc + 4)) / line_h;
    CHECK(lines >= 3 && (int)rd(rc + 8) - (int)rd(rc) <= 60, "DT_WORDBREAK | DT_CALCRECT wraps into several lines inside the width (%d lines, %d wide)", lines, (int)rd(rc + 8) - (int)rd(rc));
    draw_text(c, "one two three four five six seven", 10, 10, 70, 78, DT_WB);
    k = ink_in(c, 0, 0, CW, CH);
    CHECK(k.ink > 0 && k.x0 >= 10 && k.x1 < 70 && k.y1 - k.y0 > 2 * line_h, "DT_WORDBREAK draws the wrapped lines inside the rectangle (x %d..%d, y %d..%d)", k.x0, k.x1, k.y0, k.y1);
    clear_canvas(c.bits);
    draw_text(c, "first\nsecond", 10, 10, 190, 70, 0);
    k = ink_in(c, 0, 0, CW, CH);
    CHECK(k.y1 - k.y0 > line_h, "a line feed starts a new line without DT_WORDBREAK (rows %d..%d)", k.y0, k.y1);

    clear_canvas(c.bits);
    draw_text(c, "Hello World Hello World", 20, 20, 50, 30, 0);
    CHECK(ink_in(c, 0, 0, CW, 20).ink == 0 && ink_in(c, 0, 20, 20, CH).ink == 0 && ink_in(c, 50, 0, CW, CH).ink == 0 && ink_in(c, 0, 30, CW, CH).ink == 0 &&
              ink_in(c, 20, 20, 50, 30).ink > 0,
          "the rectangle clips the text");
    clear_canvas(c.bits);
    draw_text(c, "Hello World Hello World", 20, 20, 50, 30, DT_NOCLIP_);
    CHECK(ink_in(c, 50, 0, CW, CH).ink > 0, "DT_NOCLIP lets it run past the rectangle");

    clear_canvas(c.bits);
    draw_text(c, "A&B && C", 10, 10, 190, 70, 0);
    {
        uint32_t with_prefix = region_hash(c);
        clear_canvas(c.bits);
        draw_text(c, "AB & C", 10, 10, 190, 70, 0x800);
        CHECK(with_prefix == region_hash(c), "the prefix character is consumed unless DT_NOPREFIX");
    }

    G32(SetBkMode, 2, c.dc, 2u);
    G32(SetBkColor, 2, c.dc, 0x00FF00u);
    clear_canvas(c.bits);
    draw_text(c, "Hi", 10, 10, 190, 70, 0);
    CHECK(px(c, 10, 10) == 0x07E0 || px(c, 11, 10) == 0x07E0, "opaque mode paints the background colour behind the text (%04X)", px(c, 10, 10));
    CHECK(px(c, 150, 60) == WHITE16, "and only the text's cell");

    {
        uint32_t edge = set_rect(g_scratch + 0x1F200, 100, 40, 140, 70);
        clear_canvas(c.bits);
        CHECK(U32(DrawEdge, 4, c.dc, edge, 0xAu, 0x200Fu) == 1, "DrawEdge");
        CHECK((int)rd(edge) == 102 && (int)rd(edge + 4) == 42 && (int)rd(edge + 8) == 138 && (int)rd(edge + 12) == 68,
              "BF_ADJUST shrinks the rectangle by the sunken edge's two pixels");
        CHECK(px(c, 100, 40) == 0x8410 && px(c, 139, 69) == WHITE16 && px(c, 120, 55) == WHITE16,
              "the sunken edge is dark on the top left, light on the bottom right, and the middle is left alone without BF_MIDDLE (%04X %04X %04X)", px(c, 100, 40), px(c, 139, 69), px(c, 120, 55));
    }

    {
        uint32_t edge = set_rect(g_scratch + 0x1F200, 100, 40, 140, 70);
        U32(DrawEdge, 4, c.dc, edge, 0x5u, 0x080Fu);
        CHECK(px(c, 120, 55) == 0xC618 && (int)rd(edge) == 100, "BF_MIDDLE fills with the button face and, without BF_ADJUST, the rectangle is unchanged");
    }

    free_canvas(c);
    G32(DeleteObject, 1, font);
}

static void test_text_codepages(void) {
    float scale = lx_text_scale(-16);
    CHECK(lx_text_advance(scale, 0xE9, 0) > 0 && lx_text_advance(scale, 0xF1, 0) > 0, "the face has the Latin-1 letters Spanish and Portuguese need");
    lx_text_set_codepage(1251);
    CHECK(lx_text_advance(scale, 0xC6, 0) > lx_text_advance(scale, 'i', 0), "code page 1251 maps 0xC6 to the Cyrillic ZHE, a wide glyph");
    lx_text_set_codepage(1252);
    CHECK(lx_text_scale(0) > 0 && lx_text_scale(12) > 0 && lx_text_scale(-12) > 0, "GDI heights resolve to a scale");
}

int main(void) {
    if (!harness_start()) return 2;
    make_window();
    test_window_dc();
    test_objects();
    test_dib_and_blit();
    test_paint();
    test_text();
    test_text_codepages();
    harness_stop();
    printf("linux_gdi_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

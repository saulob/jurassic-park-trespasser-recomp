/*
 * The game's menus and dialogs are drawn for a 640x480 screen. On a larger game screen (Direct3D 11 plays at the
 * render size) their windows draw into an offscreen 640x480 raster of the game's own raster class instead, and the
 * renderer scales that canvas over the frame (d3dr_set_ui_layer). The frozen gameplay picture, the HUD and the
 * gameplay text stay on the game's screen, and so do the loading windows, which draw themselves straight to it.
 */
#include "guest_va.h"
#include "host.h"

#include <stdlib.h>

#include "ddraw/target_sync.h"
#include "frontend_hd.h"
#include "gamepad.h"
#include "jpt_display.h"
#include "loading_hd.h"

#define SCREEN_RASTER GVA(0x0073DC40)
#define RASTER_WIDTH 0x08u
#define RASTER_HEIGHT 0x0Cu
#define RASTER_PITCH 0x14u
#define RASTER_BITS 0x18u
#define RASTER_SURFACE 0x4Cu             /* CRasterVid's IDirectDrawSurface */
#define GAME_PAUSED 0x30u                /* CGameWnd::m_bPaused: DrawBackground shows the frozen picture */
#define FROZEN_PICTURE GVA(0x0066E338)       /* the DIB raster CaptureBackground fills */
#define RASTER_DISPLAY_W 0x7Cu
#define RASTER_DISPLAY_H 0x80u
#define RASTER_OBJECT_SIZE 0x74u
#define RASTER_DC_CTOR GVA(0x0040FD80)      /* (hdc, width, height, bpp), thiscall: the DIB raster class of the frozen picture */
#define UI_FRAME_VA GVA(0x00417300)         /* CUIWnd frame: draws every window and the cursor, then presents */
#define UI_DRAW_WINDOWS_VA GVA(0x00417380)  /* (raster, rect) */
#define UI_DRAW_CURSOR_VA GVA(0x00417410)   /* (raster, rect) */
#define GAME_BACKGROUND_VA GVA(0x0040C780)  /* CGameWnd::DrawBackground(raster, rect): the frozen picture */
#define CENTRE_WINDOW_VA GVA(0x00411EA0)    /* centres a window and its controls in the screen's display size */
#define RASTER_BLT_VA GVA(0x004128A0)       /* RasterBlt(source, rect, destination, ...): Locks both rasters */
#define BUTTON_HIT_TEST_VA GVA(0x00403520)  /* CUIButton hit test (x, y), thiscall: samples its picture under a masked rect */
#define CTRL_RECT 0x10u
#define BUTTON_STYLE 0x28u
#define BUTTON_MASKED 1u
#define BUTTON_PICTURE 0x2Cu
#define MGR_CURSOR_SHOWN 0x20u          /* CUIManager: the pointer is drawn while set */
#define PIXEL_BITS 16
#define LOG_LIMIT 6

static recomp_func_t s_frame_lifted, s_windows_lifted, s_cursor_lifted, s_background_lifted, s_centre_lifted,
    s_blt_lifted, s_button_hit_lifted;
static int s_enabled, s_in_frame, s_logged, s_screen_blt_write_only;
static uint32_t s_canvas;

static void call_guest(uint32_t va, uint32_t self, const uint32_t* args, int count) {
    recomp_func_t f = recomp_lookup(va);
    uint32_t ecx = g_ecx;
    for (int i = count - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    f();
    g_ecx = ecx;
}

static int make_canvas(void) {
    uint32_t object, args[4];
    HDC dc;
    if (s_canvas) return 1;
    object = (uint32_t)(uintptr_t)calloc(1, RASTER_OBJECT_SIZE);
    dc = GetDC(NULL);
    if (!object || !dc) {
        if (dc) ReleaseDC(NULL, dc);
        free((void*)(uintptr_t)object);
        return 0;
    }
    args[0] = (uint32_t)(uintptr_t)dc;
    args[1] = JPT_UI_CANVAS_W;
    args[2] = JPT_UI_CANVAS_H;
    args[3] = PIXEL_BITS;
    call_guest(RASTER_DC_CTOR, object, args, 4);
    ReleaseDC(NULL, dc);
    if (!MEM32(object + RASTER_BITS) || MEM32(object + RASTER_WIDTH) != JPT_UI_CANVAS_W ||
        MEM32(object + RASTER_HEIGHT) != JPT_UI_CANVAS_H) {
        host_log("ui", "the 640x480 UI canvas raster could not be created: menus stay at the game's screen size");
        s_enabled = 0;
        return 0;
    }
    s_canvas = object;
    return 1;
}

static int scaling_wanted_for(uint32_t* screen_w, uint32_t* screen_h, guest_ui_view_t* view) {
    uint32_t raster;
    view->kind = GUEST_WND_NONE;
    if (!s_enabled || !d3dr_hardware() || !guest_display_built()) return 0;
    raster = MEM32(SCREEN_RASTER);
    *screen_w = MEM32(raster + RASTER_DISPLAY_W);
    *screen_h = MEM32(raster + RASTER_DISPLAY_H);
    guest_ui_probe(view);
    if (view->kind == GUEST_WND_VIDEO && *screen_w <= JPT_UI_CANVAS_W && *screen_h <= JPT_UI_CANVAS_H) return 0;
    return (view->ui == GUEST_UI_MENU || view->ui == GUEST_UI_DIALOG) && view->kind != GUEST_WND_LOADER;
}

static int scaling_wanted(uint32_t* screen_w, uint32_t* screen_h) {
    guest_ui_view_t view;
    return scaling_wanted_for(screen_w, screen_h, &view);
}

int ui_canvas_present_held(void) {
    uint32_t screen_w = 0, screen_h = 0;
    guest_ui_view_t view;
    int menu;
    if (s_in_frame) return 0;
    menu = scaling_wanted_for(&screen_w, &screen_h, &view);
    return ui_present_held(s_in_frame, menu, view.kind);
}

static void clear_canvas(void) {
    uint16_t* bits = (uint16_t*)ADDR(MEM32(s_canvas + RASTER_BITS));
    long pitch = (long)MEM32(s_canvas + RASTER_PITCH);
    for (int y = 0; y < JPT_UI_CANVAS_H; y++)
        for (int x = 0; x < JPT_UI_CANVAS_W; x++) bits[y * pitch + x] = TSYNC_UNWRITTEN;
}

static void log_canvas(uint32_t screen_w, uint32_t screen_h) {
    jpt_ui_canvas_t c = jpt_ui_canvas((int)screen_w, (int)screen_h);
    if (s_logged >= LOG_LIMIT) return;
    host_log("ui", "UI canvas 640x480 over the %ux%u game screen: rectangle %d,%d-%d,%d, scale %.3f", screen_w, screen_h,
             c.rect.left, c.rect.top, c.rect.right, c.rect.bottom, c.scale);
    if (++s_logged == LOG_LIMIT) host_log("ui", "UI canvas log limit reached");
}

/* The frame presents inside the lifted call, so the HD layer is ready before it; the canvas, drawn by the game as
 * always, is what the renderer shows when the HD layer is not. */
static void ui_frame(void) {
    static uint32_t last_w, last_h;
    uint32_t screen_w = 0, screen_h = 0;
    int hd;
    if (g_alias_entry) {
        s_frame_lifted();
        return;
    }
    video_test_ui_frame();
    loading_hd_ui_frame();
    if (!scaling_wanted(&screen_w, &screen_h) || !make_canvas()) {
        window_host_set_ui_canvas(0);
        d3dr_set_ui_layer(NULL, 0, 0, 0);
        frontend_hd_idle();
        s_frame_lifted();
        return;
    }
    if (screen_w != last_w || screen_h != last_h) {
        last_w = screen_w;
        last_h = screen_h;
        log_canvas(screen_w, screen_h);
    }
    clear_canvas();
    window_host_set_ui_canvas(1);
    hd = frontend_hd_frame((int)screen_w, (int)screen_h);
    d3dr_set_ui_layer((const uint16_t*)ADDR(MEM32(s_canvas + RASTER_BITS)), (long)MEM32(s_canvas + RASTER_PITCH),
                      (int)screen_w, (int)screen_h);
    s_in_frame = 1;
    s_frame_lifted();
    s_in_frame = 0;
    d3dr_set_ui_layer(NULL, 0, 0, 0);
    if (hd) frontend_hd_frame_done();
}

/* Replaces the raster argument (the first stack argument) of a lifted function for the call. */
static void with_raster(recomp_func_t lifted, uint32_t from, uint32_t to) {
    uint32_t slot = g_esp + 4, raster = MEM32(slot);
    if (g_alias_entry || !s_in_frame || raster != from) {
        lifted();
        return;
    }
    MEM32(slot) = to;
    lifted();
    MEM32(slot) = raster;
}

static void ui_draw_windows(void) { with_raster(s_windows_lifted, MEM32(SCREEN_RASTER), s_canvas); }

/* thiscall on the UI manager, which draws its pointer only while [this + 0x20] is set. */
static void ui_draw_cursor(void) {
    uint32_t manager = g_ecx, shown = 0;
    if (g_alias_entry || !gamepad_menu_focus() || !guest_read32(manager + MGR_CURSOR_SHOWN, &shown) || !shown) {
        with_raster(s_cursor_lifted, MEM32(SCREEN_RASTER), s_canvas);
        return;
    }
    MEM32(manager + MGR_CURSOR_SHOWN) = 0;
    with_raster(s_cursor_lifted, MEM32(SCREEN_RASTER), s_canvas);
    MEM32(manager + MGR_CURSOR_SHOWN) = shown;
}

static int covers_raster(uint32_t rect, uint32_t raster) {
    return rect && (int32_t)MEM32(rect) <= 0 && (int32_t)MEM32(rect + 4) <= 0 &&
           MEM32(rect + 8) >= MEM32(raster + RASTER_WIDTH) && MEM32(rect + 12) >= MEM32(raster + RASTER_HEIGHT);
}

/* The paused branch of DrawBackground(raster, rect) is one plain RasterBlt of the frozen picture over the screen; the
 * renderer draws it from a cached texture instead. */
static int frozen_picture_on_gpu(void) {
    uint32_t screen = MEM32(SCREEN_RASTER), still = MEM32(FROZEN_PICTURE), rect = MEM32(g_esp + 8);
    if (!s_in_frame || MEM32(g_esp + 4) != s_canvas || !MEM32(g_ecx + GAME_PAUSED) || !screen || !still) return 0;
    if (MEM32(still + RASTER_WIDTH) != MEM32(screen + RASTER_WIDTH) ||
        MEM32(still + RASTER_HEIGHT) != MEM32(screen + RASTER_HEIGHT) || !MEM32(still + RASTER_BITS))
        return 0;
    return covers_raster(rect, screen) &&
           d3dr_draw_still_picture((void*)(uintptr_t)MEM32(screen + RASTER_SURFACE),
                                   (const uint16_t*)ADDR(MEM32(still + RASTER_BITS)), (long)MEM32(still + RASTER_PITCH),
                                   (int)MEM32(still + RASTER_WIDTH), (int)MEM32(still + RASTER_HEIGHT));
}

int ui_frozen_picture(const uint16_t** bits, int* w, int* h, long* pitch) {
    uint32_t still = 0, sw = 0, sh = 0, bpp = 0, sp = 0, sb = 0;
    if (!guest_read32(FROZEN_PICTURE, &still) || !still || !guest_read32(still + RASTER_WIDTH, &sw) ||
        !guest_read32(still + RASTER_HEIGHT, &sh) || !guest_read32(still + 0x10u, &bpp) ||
        !guest_read32(still + RASTER_PITCH, &sp) || !guest_read32(still + RASTER_BITS, &sb))
        return 0;
    if (bpp != PIXEL_BITS || !sb || !sw || !sh || sw > 16384 || sh > 16384 || sp < sw ||
        !guest_readable(sb, (sp * (sh - 1) + sw) * 2u))
        return 0;
    *bits = (const uint16_t*)(uintptr_t)sb;
    *w = (int)sw;
    *h = (int)sh;
    *pitch = (long)sp;
    return 1;
}

static void game_background(void) {
    if (!g_alias_entry && frozen_picture_on_gpu()) {
        g_esp += 4 + 2 * 4;
        return;
    }
    with_raster(s_background_lifted, s_canvas, MEM32(SCREEN_RASTER));
}

static void centre_window(void) {
    uint32_t screen_w = 0, screen_h = 0, raster, window = MEM32(g_esp + 4);
    if (g_alias_entry) {
        s_centre_lifted();
        return;
    }
    if (!scaling_wanted(&screen_w, &screen_h) || guest_ui_window_kind(window) == GUEST_WND_LOADER) {
        s_centre_lifted();
        frontend_hd_window_centred(window);
        return;
    }
    raster = MEM32(SCREEN_RASTER);
    MEM32(raster + RASTER_DISPLAY_W) = JPT_UI_CANVAS_W;
    MEM32(raster + RASTER_DISPLAY_H) = JPT_UI_CANVAS_H;
    s_centre_lifted();
    MEM32(raster + RASTER_DISPLAY_W) = screen_w;
    MEM32(raster + RASTER_DISPLAY_H) = screen_h;
    frontend_hd_window_centred(window);
}

/* A blit onto the screen from another raster overwrites the screen's pixels and never reads them. */
/* Reads past a picture are cut off here: a control's rect can be larger than its picture (fe_controls_rect), and the
 * game sizes the source rect from the control's rect. */
static void raster_blt(void) {
    uint32_t source = MEM32(g_esp + 4), rect_slot = g_esp + 8, rect = MEM32(g_esp + 8);
    uint32_t destination = MEM32(g_esp + 12), screen = MEM32(SCREEN_RASTER);
    int32_t clipped[4];
    int outer = s_screen_blt_write_only;
    if (g_alias_entry) {
        s_blt_lifted();
        return;
    }
    if (source && rect) {
        for (int i = 0; i < 4; i++) clipped[i] = (int32_t)MEM32(rect + 4u * (uint32_t)i);
        if (!raster_source_clip(clipped, (int32_t)MEM32(source + RASTER_WIDTH), (int32_t)MEM32(source + RASTER_HEIGHT))) {
            g_eax = 0;
            g_esp += 4;
            return;
        }
        MEM32(rect_slot) = (uint32_t)(uintptr_t)clipped;
    }
    s_screen_blt_write_only = screen && destination == screen && source != screen;
    s_blt_lifted();
    s_screen_blt_write_only = outer;
    MEM32(rect_slot) = rect;
}

int ui_screen_blt_writes_only(void) { return s_screen_blt_write_only; }

/* The game indexes the picture from the rect's corner without bounds, assuming the rect is the picture's size. */
static void button_hit_test(void) {
    uint32_t button = g_ecx, picture = MEM32(button + BUTTON_PICTURE);
    if ((MEM8(button + BUTTON_STYLE) & BUTTON_MASKED) && picture) {
        int32_t rect[4], point[2] = { (int32_t)MEM32(g_esp + 4), (int32_t)MEM32(g_esp + 8) };
        for (int i = 0; i < 4; i++) rect[i] = (int32_t)MEM32(button + CTRL_RECT + 4u * (uint32_t)i);
        if (button_picture_point(rect, (int32_t)MEM32(picture + RASTER_WIDTH), (int32_t)MEM32(picture + RASTER_HEIGHT),
                                 point)) {
            MEM32(g_esp + 4) = (uint32_t)point[0];
            MEM32(g_esp + 8) = (uint32_t)point[1];
        }
    }
    s_button_hit_lifted();
}

void ui_canvas_install(void) {
    const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
    } k[] = {
        { UI_FRAME_VA, ui_frame, &s_frame_lifted },
        { UI_DRAW_WINDOWS_VA, ui_draw_windows, &s_windows_lifted },
        { UI_DRAW_CURSOR_VA, ui_draw_cursor, &s_cursor_lifted },
        { GAME_BACKGROUND_VA, game_background, &s_background_lifted },
        { CENTRE_WINDOW_VA, centre_window, &s_centre_lifted },
    };
    guest_override_install(RASTER_BLT_VA, raster_blt, &s_blt_lifted);
    guest_override_install(BUTTON_HIT_TEST_VA, button_hit_test, &s_button_hit_lifted);
    s_enabled = 1;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!guest_override_install(k[i].va, k[i].replacement, k[i].lifted)) s_enabled = 0;
}

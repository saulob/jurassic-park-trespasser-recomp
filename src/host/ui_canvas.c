/*
 * The game's menus and dialogs are drawn for a 640x480 screen. On a larger game screen (Direct3D 11 plays at the
 * render size) their windows draw into an offscreen 640x480 raster of the game's own raster class instead, and the
 * renderer scales that canvas over the frame (d3dr_set_ui_layer). The frozen gameplay picture, the HUD and the
 * gameplay text stay on the game's screen, and so do the loading windows, which draw themselves straight to it.
 */
#include "host.h"

#include <stdlib.h>

#include "ddraw/target_sync.h"
#include "jpt_display.h"

#define SCREEN_RASTER 0x0073DC40u
#define RASTER_WIDTH 0x08u
#define RASTER_HEIGHT 0x0Cu
#define RASTER_PITCH 0x14u
#define RASTER_BITS 0x18u
#define RASTER_SURFACE 0x4Cu             /* CRasterVid's IDirectDrawSurface */
#define GAME_PAUSED 0x30u                /* CGameWnd::m_bPaused: DrawBackground shows the frozen picture */
#define FROZEN_PICTURE 0x0066E338u       /* the DIB raster CaptureBackground fills */
#define RASTER_DISPLAY_W 0x7Cu
#define RASTER_DISPLAY_H 0x80u
#define RASTER_OBJECT_SIZE 0x74u
#define RASTER_DC_CTOR 0x0040FD80u      /* (hdc, width, height, bpp), thiscall: the DIB raster class of the frozen picture */
#define UI_FRAME_VA 0x00417300u         /* CUIWnd frame: draws every window and the cursor, then presents */
#define UI_DRAW_WINDOWS_VA 0x00417380u  /* (raster, rect) */
#define UI_DRAW_CURSOR_VA 0x00417410u   /* (raster, rect) */
#define GAME_BACKGROUND_VA 0x0040C780u  /* CGameWnd::DrawBackground(raster, rect): the frozen picture */
#define CENTRE_WINDOW_VA 0x00411EA0u    /* centres a window and its controls in the screen's display size */
#define RASTER_BLT_VA 0x004128A0u       /* RasterBlt(source, rect, destination, ...): Locks both rasters */
#define PIXEL_BITS 16
#define LOG_LIMIT 6

static recomp_func_t s_frame_lifted, s_windows_lifted, s_cursor_lifted, s_background_lifted, s_centre_lifted,
    s_blt_lifted;
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

static int scaling_wanted(uint32_t* screen_w, uint32_t* screen_h) {
    guest_ui_view_t view;
    uint32_t raster;
    if (!s_enabled || !d3dr_hardware() || !guest_display_built()) return 0;
    raster = MEM32(SCREEN_RASTER);
    *screen_w = MEM32(raster + RASTER_DISPLAY_W);
    *screen_h = MEM32(raster + RASTER_DISPLAY_H);
    if (*screen_w <= JPT_UI_CANVAS_W && *screen_h <= JPT_UI_CANVAS_H) return 0;
    guest_ui_probe(&view);
    return (view.ui == GUEST_UI_MENU || view.ui == GUEST_UI_DIALOG) && view.kind != GUEST_WND_LOADER;
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

static void ui_frame(void) {
    static uint32_t last_w, last_h;
    uint32_t screen_w = 0, screen_h = 0;
    if (g_alias_entry) {
        s_frame_lifted();
        return;
    }
    if (!scaling_wanted(&screen_w, &screen_h) || !make_canvas()) {
        window_host_set_ui_canvas(0);
        d3dr_set_ui_layer(NULL, 0, 0, 0);
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
    d3dr_set_ui_layer((const uint16_t*)ADDR(MEM32(s_canvas + RASTER_BITS)), (long)MEM32(s_canvas + RASTER_PITCH),
                      (int)screen_w, (int)screen_h);
    s_in_frame = 1;
    s_frame_lifted();
    s_in_frame = 0;
    d3dr_set_ui_layer(NULL, 0, 0, 0);
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

static void ui_draw_cursor(void) { with_raster(s_cursor_lifted, MEM32(SCREEN_RASTER), s_canvas); }

static int covers_raster(uint32_t rect, uint32_t raster) {
    return rect && (int32_t)MEM32(rect) <= 0 && (int32_t)MEM32(rect + 4) <= 0 &&
           MEM32(rect + 8) >= MEM32(raster + RASTER_WIDTH) && MEM32(rect + 12) >= MEM32(raster + RASTER_HEIGHT);
}

/* DEBUG_SAULO: the first time each condition keeps the frozen picture off the GPU (logs 025-027 never drew it). */
static void frozen_picture_refused(unsigned reason, uint32_t screen, uint32_t still, uint32_t rect) {
    static unsigned logged;
    static const char* const why[] = { "not inside the UI frame", "raster argument is not the UI canvas",
                                       "game not paused", "no screen or frozen picture raster",
                                       "frozen picture size differs from the screen", "rectangle does not cover the screen",
                                       "the renderer declined" };
    if (logged & (1u << reason)) return;
    logged |= 1u << reason;
    host_log("ui", "frozen gameplay picture drawn by the game, not the GPU: %s (screen %ux%u, picture %ux%u, rectangle "
             "%d,%d-%d,%d)", why[reason], screen ? MEM32(screen + RASTER_WIDTH) : 0, screen ? MEM32(screen + RASTER_HEIGHT) : 0,
             still ? MEM32(still + RASTER_WIDTH) : 0, still ? MEM32(still + RASTER_HEIGHT) : 0,
             rect ? (int32_t)MEM32(rect) : 0, rect ? (int32_t)MEM32(rect + 4) : 0, rect ? (int32_t)MEM32(rect + 8) : 0,
             rect ? (int32_t)MEM32(rect + 12) : 0);
}

/* The paused branch of DrawBackground(raster, rect) is one plain RasterBlt of the frozen picture over the screen; the
 * renderer draws it from a cached texture instead. */
static int frozen_picture_on_gpu(void) {
    uint32_t screen = MEM32(SCREEN_RASTER), still = MEM32(FROZEN_PICTURE), rect = MEM32(g_esp + 8);
    unsigned reason = !s_in_frame ? 0u : MEM32(g_esp + 4) != s_canvas ? 1u : !MEM32(g_ecx + GAME_PAUSED) ? 2u
                    : !screen || !still ? 3u : 7u;
    if (reason == 7u && (MEM32(still + RASTER_WIDTH) != MEM32(screen + RASTER_WIDTH) ||
                         MEM32(still + RASTER_HEIGHT) != MEM32(screen + RASTER_HEIGHT) || !MEM32(still + RASTER_BITS)))
        reason = 4u;
    if (reason == 7u && !covers_raster(rect, screen)) reason = 5u;
    if (reason == 7u && !d3dr_draw_still_picture((void*)(uintptr_t)MEM32(screen + RASTER_SURFACE),
                                                 (const uint16_t*)ADDR(MEM32(still + RASTER_BITS)),
                                                 (long)MEM32(still + RASTER_PITCH), (int)MEM32(still + RASTER_WIDTH),
                                                 (int)MEM32(still + RASTER_HEIGHT)))
        reason = 6u;
    if (reason == 7u) return 1;
    if (reason) frozen_picture_refused(reason, screen, still, rect);
    return 0;
}

static void game_background(void) {
    if (!g_alias_entry && frozen_picture_on_gpu()) {
        g_esp += 4 + 2 * 4;
        return;
    }
    with_raster(s_background_lifted, s_canvas, MEM32(SCREEN_RASTER));
}

static void centre_window(void) {
    uint32_t screen_w = 0, screen_h = 0, raster;
    if (g_alias_entry || !scaling_wanted(&screen_w, &screen_h) ||
        guest_ui_window_kind(MEM32(g_esp + 4)) == GUEST_WND_LOADER) {
        s_centre_lifted();
        return;
    }
    raster = MEM32(SCREEN_RASTER);
    MEM32(raster + RASTER_DISPLAY_W) = JPT_UI_CANVAS_W;
    MEM32(raster + RASTER_DISPLAY_H) = JPT_UI_CANVAS_H;
    s_centre_lifted();
    MEM32(raster + RASTER_DISPLAY_W) = screen_w;
    MEM32(raster + RASTER_DISPLAY_H) = screen_h;
}

/* A blit onto the screen from another raster overwrites the screen's pixels and never reads them. */
static void raster_blt(void) {
    uint32_t source = MEM32(g_esp + 4), destination = MEM32(g_esp + 12), screen = MEM32(SCREEN_RASTER);
    int outer = s_screen_blt_write_only;
    if (g_alias_entry) {
        s_blt_lifted();
        return;
    }
    s_screen_blt_write_only = screen && destination == screen && source != screen;
    s_blt_lifted();
    s_screen_blt_write_only = outer;
}

int ui_screen_blt_writes_only(void) { return s_screen_blt_write_only; }

void ui_canvas_install(void) {
    static const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
        const char* name;
    } k[] = {
        { UI_FRAME_VA, ui_frame, &s_frame_lifted, "UI frame" },
        { UI_DRAW_WINDOWS_VA, ui_draw_windows, &s_windows_lifted, "UI windows" },
        { UI_DRAW_CURSOR_VA, ui_draw_cursor, &s_cursor_lifted, "UI cursor" },
        { GAME_BACKGROUND_VA, game_background, &s_background_lifted, "gameplay picture" },
        { CENTRE_WINDOW_VA, centre_window, &s_centre_lifted, "window centring" },
    };
    if (!guest_override_redirect(RASTER_BLT_VA, raster_blt, &s_blt_lifted))
        host_log("ui", "sub_%08X (RasterBlt) has no incremental-link thunk: blits onto the screen read it back first",
                 RASTER_BLT_VA);
    s_enabled = 1;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (guest_override_redirect(k[i].va, k[i].replacement, k[i].lifted)) continue;
        host_log("ui", "sub_%08X (%s) has no incremental-link thunk: the UI canvas is off", k[i].va, k[i].name);
        s_enabled = 0;
    }
}

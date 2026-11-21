#include "linux_panel.h"

#include <stddef.h>
#include <string.h>

#include "debug_menu.h"
#include "debug_menu_panel.h"

extern const unsigned char lx_font_data[];
extern const unsigned long lx_font_data_size;

#define BASE_HEIGHT 480.0f
#define SCALE_MAX 4.0f

static int s_font_set;

int lx_panel_wanted(int frame_counter_on) { return debug_menu_overlay_wanted(frame_counter_on); }

static float dpi_scale(int height) {
    float scale = (float)height / BASE_HEIGHT * 1.2f;
    return scale < 1.0f ? 1.0f : scale > SCALE_MAX ? SCALE_MAX : scale;
}

int lx_panel_draw(uint32_t* pixels, int width, int height, int pitch, const char* counter) {
    dmp_frame_t f;
    memset(&f, 0, sizeof f);
    f.view_w = (float)width;
    f.view_h = (float)height;
    f.dpi_scale = dpi_scale(height);
    debug_menu_fill_frame(&f);
    if (counter && counter[0]) f.counter = counter;
    if (!f.menu && !f.hud && !f.counter) return 0;
    if (!s_font_set) dmp_set_fallback_font_memory(lx_font_data, (size_t)lx_font_data_size), s_font_set = 1;
    dmp_draw_software(pixels, width, height, pitch, &f);
    return 1;
}

const dml_hits_t* lx_panel_hits(void) { return dmp_hits(); }

void lx_panel_release(void) {
    dmp_release();
    s_font_set = 0;
}

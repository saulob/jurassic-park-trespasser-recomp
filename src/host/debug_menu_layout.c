#include "debug_menu_layout.h"

#define MIN_HEADER 30.0f
#define MIN_CLOSE 24.0f
#define EPS 0.01f

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

int dml_contains(const dml_rect_t* r, float x, float y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

int dml_inside(const dml_rect_t* inner, const dml_rect_t* outer) {
    return inner->x >= outer->x - EPS && inner->y >= outer->y - EPS &&
           inner->x + inner->w <= outer->x + outer->w + EPS && inner->y + inner->h <= outer->y + outer->h + EPS;
}

float dml_margin(float view_w, float view_h, float em) { return maxf(em * 0.5f, 0.02f * minf(view_w, view_h)); }

static float header_height(float em) { return maxf(em * 2.2f, MIN_HEADER); }

static float close_size(float header_h) { return minf(header_h - 4.0f, maxf(header_h * 0.8f, MIN_CLOSE)); }

float dml_panel_width(float view_w, float view_h, float em, float pad, float min_w, float content_w, float title_w) {
    float w = maxf(min_w, maxf(content_w + 2.0f * pad, title_w + 2.0f * pad + header_height(em)));
    const float widest = view_w - 2.0f * dml_margin(view_w, view_h, em);
    return w > widest ? widest : w;
}

float dml_footer_width(float panel_w, float pad) { return panel_w - 2.0f * pad; }

void dml_layout(const dml_params_t* p, dml_layout_t* out) {
    const float margin = dml_margin(p->view_w, p->view_h, p->em);
    const float top_gap = p->dock_right ? p->top_inset : 0.0f, max_h = p->view_h - 2.0f * margin - top_gap;
    const float header_h = header_height(p->em), size = close_size(header_h), inset = (header_h - size) * 0.5f;
    const float rule = p->em * 0.5f;
    float body_h = p->body_h, panel_h = header_h + body_h + rule + p->footer_h + p->pad;
    out->scrolls = 0;
    if (panel_h > max_h) {
        body_h = maxf(0.0f, body_h - (panel_h - max_h));
        panel_h = header_h + body_h + rule + p->footer_h + p->pad;
        out->scrolls = 1;
    }
    out->panel.w = p->panel_w;
    out->panel.h = panel_h;
    if (p->dock_right) {
        out->panel.x = p->view_w - margin - out->panel.w;
        out->panel.y = margin + top_gap;
    } else {
        out->panel.x = (p->view_w - out->panel.w) * 0.5f;
        out->panel.y = maxf(margin, (p->view_h - panel_h) * 0.5f);
    }
    out->header = (dml_rect_t){ out->panel.x, out->panel.y, out->panel.w, header_h };
    out->close = (dml_rect_t){ out->panel.x + out->panel.w - inset - size, out->panel.y + inset, size, size };
    out->title = (dml_rect_t){ out->panel.x + p->pad, out->panel.y, out->close.x - out->panel.x - 2.0f * p->pad, header_h };
    out->body = (dml_rect_t){ out->panel.x, out->panel.y + header_h, out->panel.w, body_h };
    out->footer = (dml_rect_t){ out->panel.x + p->pad, out->body.y + body_h + rule, dml_footer_width(out->panel.w, p->pad),
                                p->footer_h };
}

void dml_counter_layout(float view_w, float em, float text_w, float text_h, dml_counter_t* out) {
    const float pad_x = em * 0.6f, pad_y = em * 0.3f, w = text_w + 2.0f * pad_x, h = text_h + 2.0f * pad_y;
    out->pad_x = pad_x;
    out->pad_y = pad_y;
    out->rounding = em * 0.5f;
    out->box = (dml_rect_t){ view_w - em - w, em, w, h };
}

float dml_counter_inset(const dml_counter_t* counter, float view_w, float view_h, float em) {
    if (!counter || counter->box.h <= 0.0f) return 0.0f;
    return maxf(0.0f, counter->box.y + counter->box.h + em * 0.5f - dml_margin(view_w, view_h, em));
}

float dml_objectives_width(float view_w, float view_h, float em) {
    const float margin = dml_margin(view_w, view_h, em), widest = minf(view_w * 0.6f, view_w - 2.0f * margin);
    const float w = em * 38.0f;
    return w > widest ? widest : w;
}

int dml_columns(const float* widths, int n, float avail, float gap, float min_gap, float* x) {
    float total = 0.0f, used = gap;
    if (n < 1 || n > DML_COLUMNS_MAX) return 0;
    for (int i = 0; i < n; i++) total += widths[i];
    if (n > 1 && total + (float)(n - 1) * gap > avail) used = maxf(min_gap, (avail - total) / (float)(n - 1));
    x[0] = 0.0f;
    for (int i = 1; i < n; i++) x[i] = x[i - 1] + widths[i - 1] + used;
    return x[n - 1] + widths[n - 1] <= avail + EPS && (n == 1 || used >= min_gap - EPS);
}

int dml_hit_find(const dml_hits_t* h, float x, float y) {
    for (int i = 0; i < h->count; i++)
        if (dml_contains(&h->hit[i].rect, x, y)) return i;
    return -1;
}

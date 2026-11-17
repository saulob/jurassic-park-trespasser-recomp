/* The geometry of the F10 / F3 panels without windows or fonts: where the header, its close button, the rows and the
 * footer go for a given view and text size, the columns of a table, and which row a pointer is over. */
#ifndef DEBUG_MENU_LAYOUT_H
#define DEBUG_MENU_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, w, h;
} dml_rect_t;

int dml_contains(const dml_rect_t* r, float x, float y);
/* 1 when inner lies wholly inside outer. */
int dml_inside(const dml_rect_t* inner, const dml_rect_t* outer);

/* The space kept free around the panel on every side. */
float dml_margin(float view_w, float view_h, float em);

/* The panel's width: the widest row, the title and the close button, at least min_w, at most the view less its margins. */
float dml_panel_width(float view_w, float view_h, float em, float pad, float min_w, float content_w, float title_w);
/* The width the footer text wraps at. */
float dml_footer_width(float panel_w, float pad);

typedef struct {
    float view_w, view_h, em, pad;
    float panel_w;
    float body_h;   /* every row */
    float footer_h; /* the footer text wrapped at dml_footer_width */
} dml_params_t;

typedef struct {
    dml_rect_t panel, header, title, close, body, footer;
    int scrolls; /* the rows do not all fit: the body scrolls and the footer stays */
} dml_layout_t;

void dml_layout(const dml_params_t* p, dml_layout_t* out);

/* Left edges of n columns of the given widths: gap apart, closing up towards min_gap when avail is short. 1 when the
 * columns then fit within avail without touching. */
#define DML_COLUMNS_MAX 4
int dml_columns(const float* widths, int n, float avail, float gap, float min_gap, float* x);

/* What a pointer can land on, filled by the panel each frame it draws. */
#define DML_HITS_MAX 48
typedef struct {
    dml_rect_t rect;
    int item;
} dml_hit_t;

typedef struct {
    int valid, menu, page, count;
    int has_close;
    dml_rect_t panel, close, help; /* help: the reserved help area */
    dml_hit_t hit[DML_HITS_MAX];
} dml_hits_t;

/* The index of the row at the point, or -1. */
int dml_hit_find(const dml_hits_t* h, float x, float y);

#ifdef __cplusplus
}
#endif

#endif

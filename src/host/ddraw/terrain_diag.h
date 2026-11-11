#ifndef TRESPASSER_TERRAIN_DIAG_H
#define TRESPASSER_TERRAIN_DIAG_H

/* DEBUG_SAULO: terrain draws seen by the D3D11 renderer, and a census that tells for the black parts
 * of a gameplay frame whether terrain covered them and what colour it should have produced there. */

#include "d3d_legacy.h"

/* CScreenRenderAuxD3D batches: terrain (RasterizeTerrainBatch) and textured opaque polygons under full
 * hardware texturing (RasterizeOpaque, render type 6). */
#define TDIAG_TERRAIN_SUBMITTER 0x0055D8B0u
#define TDIAG_OBJECT_SUBMITTER 0x0055B560u

enum { TDIAG_KIND_OTHER, TDIAG_KIND_TERRAIN, TDIAG_KIND_OBJECTS };
enum { TDIAG_WIREFRAME = 1, TDIAG_SOLID = 2, TDIAG_TEXTURE = 4, TDIAG_NOCULL = 8, TDIAG_NODEPTH = 16 };

/* Same layout as the renderer's batch vertex (D3DTLVERTEX). */
typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse, specular;
    float u, v;
} tdiag_vertex_t;

int tdiag_parse_option(const char* arg);
int tdiag_draw_kind(uint32_t caller);
/* Debug output for a batch of that kind; skip drops its draws (diagnostic only). */
int tdiag_debug_for(int kind);
int tdiag_skip(int kind);
void tdiag_log_debug_flags(void);

/* One DrawPrimitive after translation: its vertices are v[first..first+nverts), idx its triangle-list
 * indices into v. */
void tdiag_draw(d3d_device_t* dev, int kind, const tdiag_vertex_t* v, unsigned first, unsigned nverts,
                const uint32_t* idx, unsigned nidx, unsigned legacy_w, unsigned legacy_h);
void tdiag_clear(int target, int zbuffer, float z);
void tdiag_upload(const dd_surface_t* s, const uint32_t* bgra, unsigned w, unsigned h, unsigned left, unsigned top);
void tdiag_texture_written(const dd_surface_t* s, const RECT* r);
/* The water node a CEntityWater texture update (0x0060E590) runs for: tags its texture's D3D surface. */
void tdiag_note_water_node(uint32_t node);
void tdiag_texture_created(void);
void tdiag_texture_freed(void);
void tdiag_surface_freed(const dd_surface_t* s);

/* The frame being presented carries the cell grid; census it with its RGB565 pixels. */
int tdiag_census_frame(void);
void tdiag_census(const void* pixels, size_t pitch, unsigned w, unsigned h);
/* After each presented frame; arms the grid for the next one when a census is due. */
void tdiag_frame_end(int gameplay, double now);
void tdiag_report(double frames);

#endif

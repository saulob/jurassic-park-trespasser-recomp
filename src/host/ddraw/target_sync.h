#ifndef TRESPASSER_TARGET_SYNC_H
#define TRESPASSER_TARGET_SYNC_H

/*
 * Where the picture of a Direct3D render target is: on the GPU colour target, in the CPU copy the
 * game Locks, or both. The CPU copy is otherwise a capture buffer: pixels that differ from the
 * reference taken when the capture began are what the CPU wrote, and only those reach the GPU.
 * Pure logic, no device, so tests run it headless; the renderer performs the returned actions.
 */

#include <stdint.h>

#include "d3d_translate.h"

enum { TSYNC_CPU = 1u, TSYNC_GPU = 2u };

/* Performed by the renderer in this bit order. */
enum {
    TSYNC_COMPOSITE = 1u << 0,   /* changed CPU pixels drawn over the GPU target; reference := CPU copy */
    TSYNC_UPLOAD = 1u << 1,      /* the CPU copy is the whole picture: drawn opaque; reference := CPU copy */
    TSYNC_READBACK = 1u << 2,    /* GPU picture -> CPU copy and reference */
    TSYNC_COPY_FRONT = 1u << 3,  /* the front buffer holds the GPU picture: copied to CPU copy and reference */
    TSYNC_SYNTHESIZE = 1u << 4,  /* CPU copy and reference := the clear colour the GPU target holds */
    TSYNC_CAPTURE = 1u << 5,     /* CPU copy and reference := TSYNC_UNWRITTEN in the accessed region */
    TSYNC_CAPTURE_ALL = 1u << 6  /* the same over the whole surface */
};

/* Marks "not written by the CPU" in a capture; a write of exactly this value is not seen. */
#define TSYNC_UNWRITTEN 0xF81Fu

typedef enum { TSYNC_ACCESS_READ, TSYNC_ACCESS_NO_READ } tsync_access_t;

typedef struct {
    unsigned valid;      /* TSYNC_CPU / TSYNC_GPU: where the picture is complete, pending CPU writes aside */
    int pending;         /* the CPU copy holds writes the GPU target lacks */
    int uniform;         /* the GPU target holds one clear colour */
    uint16_t color;
    int stale_reference; /* the reference does not match the CPU copy (new device, Flip, clear) */
    int exact;           /* the reference equals the GPU picture, so what CPU writes cover is known */
    int front_mirror;    /* the front buffer's pixels are the GPU picture (last frame came from the CPU copy) */
} tsync_t;

/* A device on an existing surface: its pixels are the picture. */
void tsync_attach(tsync_t* t);
/* Before Lock, GetDC or Blt: what makes the CPU copy usable without losing pending writes. */
unsigned tsync_cpu_access(tsync_t* t, tsync_access_t access);
/* The part of tsync_cpu_access that cannot wait: pending CPU writes reach the GPU target. */
unsigned tsync_cpu_flush(tsync_t* t);

/* A Lock whose pixels are made usable only when the game first touches them. A Lock the game never
 * touches (a wait for the GPU) then costs nothing and changes nothing. */
typedef struct {
    unsigned acts;     /* waiting for the first touch */
    tsync_t untouched; /* the state if no touch comes */
} tsync_deferred_t;

/* Returns what to perform now; d->acts is what the first touch performs, 0 when nothing waits. */
unsigned tsync_lock_deferred(tsync_t* t, tsync_access_t access, tsync_deferred_t* d);
unsigned tsync_lock_touched(tsync_deferred_t* d);
/* Unlock with no touch: the access did not happen, so there is no CPU write to record. */
void tsync_lock_untouched(tsync_t* t, tsync_deferred_t* d);
/* Unlock, ReleaseDC, a Blt into the surface. */
void tsync_cpu_wrote(tsync_t* t);
/* Before a draw or a partial clear of the GPU target. */
unsigned tsync_gpu_draw(tsync_t* t);
/* A clear of the whole GPU target: writes the CPU had not composited are covered by it. */
void tsync_gpu_clear(tsync_t* t, uint16_t color);
/* Before Flip; then tsync_presents_gpu says which picture to show. */
unsigned tsync_present(tsync_t* t);
int tsync_presents_gpu(const tsync_t* t);
/* After a GPU-presented Flip exchanged the CPU buffers. */
void tsync_flipped(tsync_t* t);
/* The front buffer changed since the Flip: it no longer mirrors the GPU picture. */
void tsync_front_changed(tsync_t* t);
/* Before the GPU target is released while its picture can still be read (resize, device release). */
unsigned tsync_gpu_release(tsync_t* t);
/* The GPU target was lost without a chance to read it. */
void tsync_gpu_gone(tsync_t* t);
const char* tsync_location_name(unsigned valid);

/* black: changed pixels the CPU made black (the reference had another value there). */
typedef struct {
    unsigned long changed, black;
} tsync_counts_t;

/* CPU copy rows of `width` RGB565 pixels at `pitch` bytes; the reference is packed (`width` per row). */
void tsync_fill(uint8_t* copy, long pitch, uint16_t* reference, unsigned width, const d3dt_rect_t* r, uint16_t value);
/* Bounding box of the pixels that differ inside region; 0 when none. */
int tsync_changed_box(const uint8_t* copy, long pitch, const uint16_t* reference, unsigned width,
                      const d3dt_rect_t* region, d3dt_rect_t* box);
/* Overlay texels for box (changed pixels opaque, the rest transparent), counted; then reference := copy. */
void tsync_overlay_box(const uint8_t* copy, long pitch, uint16_t* reference, unsigned width, const d3dt_rect_t* box,
                       uint32_t* out, tsync_counts_t* counts);

#endif

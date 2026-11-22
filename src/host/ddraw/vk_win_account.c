#include "vk_win_account.h"

#include <string.h>

const char* vka_name(vka_cat_t cat) {
    static const char* const names[VKA_COUNT] = {
        "draw translation", "texture preparation", "target sync", "CPU access to targets", "front buffer readback",
        "layers and overlays", "frame begin", "flip and present", "video presentation", "other backend calls", "host overlays", "video subtitles",
    };
    return cat >= 0 && cat < VKA_COUNT ? names[cat] : "?";
}

void vka_reset(vka_t* a) { memset(a, 0, sizeof *a); }

void vka_enter(vka_t* a, vka_cat_t cat, double now_ms) {
    if (a->depth >= VKA_MAX_DEPTH) {
        a->overflow++;
        a->depth++;
        return;
    }
    a->stack[a->depth].cat = (int)cat;
    a->stack[a->depth].start = now_ms;
    a->stack[a->depth].child = 0.0;
    a->depth++;
}

void vka_leave(vka_t* a, double now_ms) {
    double elapsed, own;
    int cat;
    if (a->depth <= 0) {
        a->unbalanced++;
        return;
    }
    a->depth--;
    if (a->depth >= VKA_MAX_DEPTH) return;
    elapsed = now_ms - a->stack[a->depth].start;
    if (elapsed < 0.0) elapsed = 0.0;
    own = elapsed - a->stack[a->depth].child;
    if (own < 0.0) own = 0.0;
    cat = a->stack[a->depth].cat;
    a->ms[cat] += own;
    a->calls[cat]++;
    if (own > a->max_ms[cat]) a->max_ms[cat] = own;
    if (a->depth > 0 && a->depth - 1 < VKA_MAX_DEPTH) a->stack[a->depth - 1].child += elapsed;
}

double vka_total_ms(const vka_t* a) {
    double sum = 0.0;
    for (int i = 0; i < VKA_COUNT; i++) sum += a->ms[i];
    return sum;
}

void vka_delta(const vka_t* now, const vka_t* base, vka_t* out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < VKA_COUNT; i++) {
        out->ms[i] = now->ms[i] - base->ms[i];
        out->calls[i] = now->calls[i] - base->calls[i];
        out->max_ms[i] = now->max_ms[i];
    }
}

void vka_split(double wall_ms, double cpu_ms, double backend_ms, double waits_ms, unsigned long long frames,
               vka_split_t* out) {
    double f = frames ? (double)frames : 1.0, blocked, unaccounted;
    memset(out, 0, sizeof *out);
    if (backend_ms > wall_ms) backend_ms = wall_ms;
    if (cpu_ms > wall_ms) cpu_ms = wall_ms;
    blocked = wall_ms - cpu_ms;
    unaccounted = blocked - waits_ms;
    if (unaccounted < 0.0) unaccounted = 0.0;
    out->frame_ms = wall_ms / f;
    out->backend_ms = backend_ms / f;
    out->guest_ms = (wall_ms - backend_ms) / f;
    out->cpu_ms = cpu_ms / f;
    out->blocked_ms = blocked / f;
    out->waits_ms = (waits_ms < blocked ? waits_ms : blocked) / f;
    out->unaccounted_ms = unaccounted / f;
    out->unaccounted_pct = wall_ms > 0.0 ? 100.0 * unaccounted / wall_ms : 0.0;
}

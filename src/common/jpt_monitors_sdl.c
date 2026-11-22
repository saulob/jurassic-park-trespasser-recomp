#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "jpt_display.h"
#include "jpt_monitors.h"

int jpt_list_monitors(jpt_monitor_t* out, int cap) {
    int count = 0, found = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&found);
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; ids && i < found && count < cap; i++) {
        SDL_Rect bounds, usable;
        const char* name = SDL_GetDisplayName(ids[i]);
        jpt_monitor_t* m = &out[count];
        memset(m, 0, sizeof *m);
        if (!SDL_GetDisplayBounds(ids[i], &bounds)) continue;
        if (!SDL_GetDisplayUsableBounds(ids[i], &usable)) usable = bounds;
        m->handle = (HMONITOR)(size_t)ids[i];
        m->bounds.left = bounds.x, m->bounds.top = bounds.y;
        m->bounds.right = bounds.x + bounds.w, m->bounds.bottom = bounds.y + bounds.h;
        m->work.left = usable.x, m->work.top = usable.y;
        m->work.right = usable.x + usable.w, m->work.bottom = usable.y + usable.h;
        m->primary = ids[i] == primary;
        snprintf(m->device, sizeof m->device, "DISPLAY%d", count + 1);
        snprintf(m->name, sizeof m->name, "%s", name ? name : "");
        count++;
    }
    SDL_free(ids);
    for (int i = 1; i < count; i++) {
        jpt_monitor_t chosen;
        if (!out[i].primary) continue;
        chosen = out[i];
        memmove(&out[1], &out[0], (size_t)i * sizeof *out);
        out[0] = chosen;
        break;
    }
    return count;
}

int jpt_find_monitor(int setting, jpt_monitor_t* out) {
    jpt_monitor_t all[JPT_MAX_MONITORS];
    int count = jpt_list_monitors(all, JPT_MAX_MONITORS);
    if (count <= 0) return 0;
    *out = all[jpt_monitor_index(setting, count)];
    return 1;
}

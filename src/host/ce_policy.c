#include "ce_policy.h"

#include <stdio.h>
#include <string.h>

qs_decision_t qs_decide_save(const qs_world_t* w) {
    if (w->place != QS_PLACE_LEVEL) return QS_REFUSE_PLACE;
    if (w->host_panel || w->load_pending) return QS_REFUSE_BUSY;
    if (!w->player_alive) return QS_REFUSE_DEAD;
    return QS_DO_SAVE;
}

qs_decision_t qs_decide_load(const qs_world_t* w) {
    if (w->place == QS_PLACE_NONE) return QS_REFUSE_PLACE;
    if (w->host_panel || w->load_pending) return QS_REFUSE_BUSY;
    if (!w->file_exists) return QS_REFUSE_MISSING;
    return w->place == QS_PLACE_LEVEL ? QS_DO_LOAD_LEVEL : QS_DO_LOAD_MAIN_MENU;
}

int qs_path(const char* dir, char* out, size_t cap) {
    int n;
    if (!dir || !*dir || !out || !cap) return 0;
    n = snprintf(out, cap, "%s%s%s", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\", QS_FILE_NAME);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

void qs_display_name(const char* name, char* out, size_t cap) {
    size_t n = name ? strlen(name) : 0;
    if (!cap) return;
    if (n > QS_NAME_MAX) n = QS_NAME_MAX;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, name ? name : "", n);
    out[n] = 0;
}

int qs_is_quick_file(const char* path) {
    const char *a, *b, *name;
    if (!path) return 0;
    a = strrchr(path, '\\');
    b = strrchr(path, '/');
    name = a > b ? a + 1 : b ? b + 1 : path;
    return !_stricmp(name, QS_FILE_NAME);
}

int qs_filter_entries(uint8_t* entries, int count, size_t stride, size_t name_offset) {
    int kept = 0;
    for (int i = 0; i < count; i++) {
        uint8_t* e = entries + (size_t)i * stride;
        char name[32];
        size_t room = stride - name_offset < sizeof name ? stride - name_offset : sizeof name;
        memcpy(name, e + name_offset, room);
        name[room - 1] = 0;
        if (qs_is_quick_file(name)) continue;
        if (kept != i) memmove(entries + (size_t)kept * stride, e, stride);
        kept++;
    }
    return kept;
}

int ds_needs_chooser(const ds_press_t* p) { return p->enabled && p->stow_pressed && p->phase == 0 && p->modifier; }

ds_plan_t ds_plan(const ds_press_t* p) {
    ds_plan_t plan = { 0, 0 };
    if (!ds_needs_chooser(p)) return plan;
    if (p->chooser >= 0) plan.stow_only = 1;
    else if (p->belt && p->back) plan.hide_belt = 1;
    return plan;
}

int ds_stop_after_stow(int stow_only, int phase_before, int phase_after, int hand_empty) {
    return stow_only && phase_before == 3 && phase_after == 4 && hand_empty;
}

int ds_pose_preload(int phase_before, int active, int other_filled, int hand_empty) {
    if (active < 0 || active > 1) return -1;
    if (phase_before == 3) return active;
    if (phase_before == 5 && other_filled && hand_empty) return 1 - active;
    return -1;
}

int sound_rearm(float* last_time, float min_delay, float now) {
    if (!(*last_time > now)) return 0;
    *last_time = now - (min_delay > 0.0f ? min_delay : 0.0f);
    return 1;
}

int ds_pose_swapped(int phase_before, int phase_after) { return phase_before == 3 && (phase_after == 4 || phase_after == 0); }

uint8_t ce_detail_reduce(int max_object_detail, uint8_t game_value) { return max_object_detail ? 0 : game_value; }

uint32_t legacy_free_bytes(uint32_t sectors_per_cluster, uint32_t bytes_per_sector, uint32_t free_clusters, uint32_t swap_bytes) {
    return sectors_per_cluster * bytes_per_sector * free_clusters + swap_bytes;
}

int legacy_disk_check_passes(uint32_t free_bytes, uint32_t megabytes) { return free_bytes >= megabytes << 20; }

int startup_disk_check_passes(void) { return 1; }

void legacy_disk_geometry(uint64_t free_bytes, uint64_t total_bytes, uint32_t out[4]) {
    const uint64_t cluster = 8u * 512u, free_max = 0x7FFFFu, total_max = 0xFFFFFu;
    uint64_t free_clusters = free_bytes / cluster, total_clusters = total_bytes / cluster;
    if (free_clusters > free_max) free_clusters = free_max;
    if (total_clusters > total_max) total_clusters = total_max;
    if (total_clusters < free_clusters) total_clusters = free_clusters;
    out[0] = 8u;
    out[1] = 512u;
    out[2] = (uint32_t)free_clusters;
    out[3] = (uint32_t)total_clusters;
}

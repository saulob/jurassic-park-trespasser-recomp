/* Quick Save / Quick Load and Dual stow decisions, without guest memory (tested headlessly). */
#ifndef CE_POLICY_H
#define CE_POLICY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QS_FILE_NAME "SAVEGAME.QCK"
#define QS_NAME_MAX 20 /* the game's save name field holds 20 characters and its terminator */

typedef enum { QS_PLACE_NONE, QS_PLACE_LEVEL, QS_PLACE_MAIN_MENU } qs_place_t;

typedef struct {
    qs_place_t place;
    int player_alive;
    int load_pending; /* a level change of the game's triggers is under way */
    int host_panel;   /* a host overlay is open */
    int file_exists;
} qs_world_t;

typedef enum {
    QS_DO_SAVE,
    QS_DO_LOAD_LEVEL,
    QS_DO_LOAD_MAIN_MENU,
    QS_REFUSE_PLACE,   /* not in a level (or, to load, the main menu) */
    QS_REFUSE_DEAD,
    QS_REFUSE_BUSY,
    QS_REFUSE_MISSING,
} qs_decision_t;

qs_decision_t qs_decide_save(const qs_world_t* w);
qs_decision_t qs_decide_load(const qs_world_t* w);

/* "<dir>SAVEGAME.QCK"; dir as the game gives it, with its trailing backslash. 0 when it does not fit. */
int qs_path(const char* dir, char* out, size_t cap);
/* The save's display name, cut to what the game's field holds. */
void qs_display_name(const char* name, char* out, size_t cap);
/* SAVEGAME.QCK (any case), with or without a folder. */
int qs_is_quick_file(const char* path);
/* Drops the Quick Save from the game's list of save entries (stride bytes each, file name at name_offset); returns the
 * new count. The order of the others is kept. */
int qs_filter_entries(uint8_t* entries, int count, size_t stride, size_t name_offset);

/* Dual stow, over the retail stow handler: CPlayer phase 0 idle, 1..6 the stow animation, the swap at 4. */
typedef struct {
    int enabled;
    int stow_pressed;
    int modifier;     /* Rotate Arm or Rotate Wrist held */
    int phase;
    int chooser;      /* the slot the held object fits, -1 when it cannot be stowed */
    int belt, back;   /* the slots hold an object */
} ds_press_t;

typedef struct {
    int stow_only; /* stow and keep the hand empty instead of fetching the other slot */
    int hide_belt; /* retrieve the back: the retail rule picks the back while the belt reads empty */
} ds_plan_t;

/* Whether the press needs the slot chooser's answer (only then is chooser read). */
int ds_needs_chooser(const ds_press_t* p);
ds_plan_t ds_plan(const ds_press_t* p);
/* After the call: the stow just reached the swap with the hand left empty. */
int ds_stop_after_stow(int stow_only, int phase_before, int phase_after, int hand_empty);

/* The slot whose own retrieve pose goes into the game's single one before the call, or -1. */
int ds_pose_preload(int phase_before, int active, int other_filled, int hand_empty);
/* The swap stored the pose: 1 when the call took the stow phase from 3 to 4 (or to 0, the retail stop). */
int ds_pose_swapped(int phase_before, int phase_after);

/* A collision sound (footsteps included) plays only once fMinDelay + fLastTime < the game time, and a load sets the
 * game time back while fLastTime stays: a time in the future is re-armed so the pair can play again. 1 when changed. */
int sound_rearm(float* last_time, float min_delay, float now);

/* The renderer's bDetailReduce byte with Maximum object detail: On clears it (every object keeps its top mesh), Off
 * leaves whatever the game's own LOD set. */
uint8_t ce_detail_reduce(int max_object_detail, uint8_t game_value);

/* The retail startup disk check (0x0040DA00) multiplies sectors per cluster, bytes per sector and free clusters in 32
 * bits and adds the swap files' sizes: a volume with more than 4 GB free wraps to a small number. */
uint32_t legacy_free_bytes(uint32_t sectors_per_cluster, uint32_t bytes_per_sector, uint32_t free_clusters, uint32_t swap_bytes);
/* Its comparison (unsigned, megabytes << 20): 1 when the game would start. */
int legacy_disk_check_passes(uint32_t free_bytes, uint32_t megabytes);
/* What the port does instead: the check is not made, the game always starts. */
int startup_disk_check_passes(void);
/* What GetDiskFreeSpaceA tells the game: the retail loader multiplies sectors per cluster, bytes per sector and free
 * clusters in 32 bits, so the free space is reported capped at 2 GB. out: sectors per cluster, bytes per sector, free
 * clusters, total clusters. */
void legacy_disk_geometry(uint64_t free_bytes, uint64_t total_bytes, uint32_t out[4]);

#ifdef __cplusplus
}
#endif

#endif

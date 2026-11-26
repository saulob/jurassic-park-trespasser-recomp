/* A custom level's minimap and objectives, from the guide the launcher made of its world (launcher/level_guide.cpp). The
 * campaign's own are used only for a level whose world is the installed game's; the campaign never reads a guide. */
#ifndef CUSTOM_LEVEL_GUIDE_H
#define CUSTOM_LEVEL_GUIDE_H

#include "minimap.h"
#include "objective_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LEVEL_GUIDE_VERSION 1
#define LEVEL_GUIDE_FOLDER ".runtime/guide"

/* In a custom session: the minimap reads its level's data through the guide. */
void level_guide_install(void);
/* 1 when the custom session decides the level's minimap (both NULL: there is no information); 0: the campaign's holds. */
int level_guide_minimap(const char* level_id, const minimap_bounds_t** bounds, const minimap_level_map_t** map);
/* The same for its objectives; *table NULL: there is no information. */
int level_guide_objectives(const char* level_id, const ob_level_t** table);

/* Parsing, apart for the tests: 0 when the text is no guide of this version. */
typedef struct {
    int in_game;
    int has_map;
    minimap_bounds_t bounds;
    minimap_level_map_t map;
    minimap_run_t* runs;
    minimap_landmark_t* landmarks;
    minimap_vehicle_t* vehicles;
    int objective_count;
    int hints[DM_GOALS_MAX];
    char targets[DM_GOALS_MAX][OB_NAMES_MAX][64];
} level_guide_t;
int level_guide_parse(const char* text, level_guide_t* out);
void level_guide_free(level_guide_t* guide);
/* An objective's title from its hint's text: its first sentence, at most cap - 1 bytes. */
void level_guide_title(const char* hint_text, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif

/* What custom levels can ask for beyond the 1998 game, and what of it this runtime does. A level is playable when everything it
 * requires is supported; no package and no level is ever told by its name. */
#ifndef JPT_LEVEL_FEATURES_H
#define JPT_LEVEL_FEATURES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPT_FEATURE_FULLY_SUPPORTED,
    JPT_FEATURE_PARTIALLY_SUPPORTED,
    JPT_FEATURE_NOT_SUPPORTED,
    JPT_FEATURE_NOT_REQUIRED, /* no level's flow or play needs it here */
    JPT_FEATURE_UNKNOWN
} jpt_feature_status_t;

typedef struct {
    const char* name; /* "ATX_ACTION_106" */
    const char* title;
    jpt_feature_status_t status;
    const char* evidence; /* where it takes effect */
    const char* note;
} jpt_level_feature_t;

const jpt_level_feature_t* jpt_level_feature_at(int index); /* NULL past the last one */
const jpt_level_feature_t* jpt_level_feature(const char* name);
const char* jpt_feature_status_name(jpt_feature_status_t status);
/* A level that requires it may be played. */
int jpt_level_feature_playable(const char* name);

/* The feature an ActionType past the game's own is; NULL for one that neither ATX nor Trespasser CE gives an effect, which does
 * nothing in any of them. */
const jpt_level_feature_t* jpt_level_action_feature(int type);

#ifdef __cplusplus
}
#endif

#endif

/* The one classification of a custom level's problems, for the scanner, the importer, the launcher and the runtime alike.
 * Each problem has a stable code; its kind says what it is and the kind alone decides the level's state. A code that is not
 * classified here blocks the level. */
#ifndef JPT_LEVEL_COMPAT_H
#define JPT_LEVEL_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* In order of restriction: a level takes the most restrictive state of its problems. */
typedef enum { JPT_COMPAT_READY, JPT_COMPAT_LIMITED, JPT_COMPAT_INVALID } jpt_compat_t;

typedef enum {
    JPT_ISSUE_NOTE,         /* READY: only informs */
    JPT_ISSUE_FEATURE,      /* LIMITED: a feature the runtime leaves out without risk */
    JPT_ISSUE_UNCONFIRMED,  /* LIMITED: an optional file or setting whose effect cannot be confirmed */
    JPT_ISSUE_MISSING,      /* INVALID: a file the level cannot be loaded without */
    JPT_ISSUE_DAMAGED,      /* INVALID: a file that cannot be read as what it should be */
    JPT_ISSUE_INCOMPATIBLE, /* INVALID: an essential format or feature the engine cannot load */
    JPT_ISSUE_CAPACITY,     /* INVALID: past what the engine can hold */
    JPT_ISSUE_SECURITY,     /* INVALID: a path or file that is never followed */
    JPT_ISSUE_UNKNOWN,      /* INVALID: not classified */
    JPT_ISSUE_KINDS
} jpt_issue_kind_t;

/* note: the scanner found it informative only. subject: a feature name, "ActionType N" or a configuration code, by code. */
jpt_issue_kind_t jpt_level_issue_kind(const char* code, const char* subject, int note);
jpt_compat_t jpt_issue_compat(jpt_issue_kind_t kind);
jpt_compat_t jpt_compat_worst(jpt_compat_t a, jpt_compat_t b);

/* "ready", "limited", "invalid": what logs and records spell. */
const char* jpt_compat_name(jpt_compat_t compat);
int jpt_compat_parse(const char* name, jpt_compat_t* out);
/* "READY", "LIMITED COMPATIBILITY", "INVALID / UNSAFE". */
const char* jpt_compat_title(jpt_compat_t compat);
/* "note", "missing feature", ...; for logs and reports. */
const char* jpt_issue_kind_name(jpt_issue_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif

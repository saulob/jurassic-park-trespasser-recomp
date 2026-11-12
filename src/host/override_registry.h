#ifndef TRESPASSER_OVERRIDE_REGISTRY_H
#define TRESPASSER_OVERRIDE_REGISTRY_H

#include <stdint.h>

typedef void (*override_fn_t)(void);

typedef enum { OVERRIDE_REQUIRED, OVERRIDE_OPTIONAL } override_need_t;
typedef enum { OVERRIDE_UNUSED, OVERRIDE_INSTALLED, OVERRIDE_FAILED } override_state_t;
typedef enum {
    OVERRIDE_REFUSED_UNLISTED,
    OVERRIDE_REFUSED_UNVERIFIED,
    OVERRIDE_REFUSED_NO_REPLACEMENT,
    OVERRIDE_REFUSED_TWICE
} override_refusal_t;

/* One lifted function the host may replace. Its stable entry (sub_<va>) always jumps through *entry, which holds the
 * lifted body until an override is installed; nothing depends on how the linker reaches sub_<va>. */
typedef struct {
    uint32_t va;
    override_fn_t* entry;
    override_fn_t lifted;
    override_need_t need;
    override_state_t state;
} override_entry_t;

#define OVERRIDE_MAX_REFUSALS 16

typedef struct {
    override_entry_t* entries;
    unsigned count;
    int target_verified;
    unsigned refusal_count;
    struct {
        uint32_t va;
        override_refusal_t why;
        int required; /* an unlisted address counts as required */
    } refusals[OVERRIDE_MAX_REFUSALS];
} override_registry_t;

typedef struct {
    unsigned listed, installed, required_installed, optional_installed, unused, required_failed, optional_failed;
    unsigned refused, required_refused;
} override_counts_t;

/* Expansions of a GUEST_OVERRIDE(hex address, REQUIRED|OPTIONAL) list: the lifted body's declaration, the stable entry
 * that replaces the lifted function's own symbol, and the registry row. */
#define OVERRIDE_LIFTED_DECL(va, need) void lifted_sub_##va(void);
#define OVERRIDE_STABLE_ENTRY(va, need)                    \
    static override_fn_t s_entry_##va = lifted_sub_##va; \
    void sub_##va(void) { s_entry_##va(); }
#define OVERRIDE_ROW(va, need) { 0x##va##u, &s_entry_##va, lifted_sub_##va, OVERRIDE_##need, OVERRIDE_UNUSED },

/* Sorted by va without duplicates, every entry still pointing at its lifted body. */
int override_registry_valid(const override_registry_t* r);
override_entry_t* override_registry_find(override_registry_t* r, uint32_t va);
/* Points va's entry at replacement and gives its lifted body; 0 (recorded as a refusal) for an unlisted va, an
 * install before the target executable was verified, no usable replacement or a second install. */
int override_registry_install(override_registry_t* r, uint32_t va, override_fn_t replacement, override_fn_t* lifted);
override_counts_t override_registry_counts(const override_registry_t* r);
const char* override_refusal_text(override_refusal_t why);

#endif

#include "override_registry.h"

#include <stddef.h>

int override_registry_valid(const override_registry_t* r) {
    for (unsigned i = 0; i < r->count; i++) {
        const override_entry_t* e = &r->entries[i];
        if (!e->entry || !e->lifted || *e->entry != e->lifted || e->state != OVERRIDE_UNUSED) return 0;
        if (i && r->entries[i - 1].va >= e->va) return 0;
    }
    return 1;
}

override_entry_t* override_registry_find(override_registry_t* r, uint32_t va) {
    unsigned lo = 0, hi = r->count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (r->entries[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    return lo < r->count && r->entries[lo].va == va ? &r->entries[lo] : NULL;
}

static int refuse(override_registry_t* r, uint32_t va, override_refusal_t why, int required) {
    if (r->refusal_count < OVERRIDE_MAX_REFUSALS) {
        r->refusals[r->refusal_count].va = va;
        r->refusals[r->refusal_count].why = why;
        r->refusals[r->refusal_count].required = required;
    }
    r->refusal_count++;
    return 0;
}

int override_registry_install(override_registry_t* r, uint32_t va, override_fn_t replacement, override_fn_t* lifted) {
    override_entry_t* e = override_registry_find(r, va);
    override_refusal_t why;
    if (!e) return refuse(r, va, OVERRIDE_REFUSED_UNLISTED, 1);
    if (!r->target_verified) why = OVERRIDE_REFUSED_UNVERIFIED;
    else if (!replacement || replacement == e->lifted || !lifted) why = OVERRIDE_REFUSED_NO_REPLACEMENT;
    else if (e->state == OVERRIDE_INSTALLED || *e->entry != e->lifted) why = OVERRIDE_REFUSED_TWICE;
    else {
        *lifted = e->lifted;
        *e->entry = replacement;
        e->state = OVERRIDE_INSTALLED;
        return 1;
    }
    if (e->state != OVERRIDE_INSTALLED) e->state = OVERRIDE_FAILED;
    return refuse(r, va, why, e->need == OVERRIDE_REQUIRED);
}

void override_registry_reset(override_registry_t* r) {
    for (unsigned i = 0; i < r->count; i++) {
        *r->entries[i].entry = r->entries[i].lifted;
        r->entries[i].state = OVERRIDE_UNUSED;
    }
    r->target_verified = 0;
    r->refusal_count = 0;
}

override_counts_t override_registry_counts(const override_registry_t* r) {
    override_counts_t c = { 0 };
    c.listed = r->count;
    c.refused = r->refusal_count;
    for (unsigned i = 0; i < r->refusal_count && i < OVERRIDE_MAX_REFUSALS; i++) c.required_refused += r->refusals[i].required;
    if (r->refusal_count > OVERRIDE_MAX_REFUSALS) c.required_refused += r->refusal_count - OVERRIDE_MAX_REFUSALS;
    for (unsigned i = 0; i < r->count; i++) {
        const override_entry_t* e = &r->entries[i];
        int required = e->need == OVERRIDE_REQUIRED;
        switch (e->state) {
        case OVERRIDE_INSTALLED:
            c.installed++;
            if (required) c.required_installed++;
            else c.optional_installed++;
            break;
        case OVERRIDE_FAILED:
            if (required) c.required_failed++;
            else c.optional_failed++;
            break;
        default: c.unused++; break;
        }
    }
    return c;
}

const char* override_refusal_text(override_refusal_t why) {
    switch (why) {
    case OVERRIDE_REFUSED_UNLISTED: return "no stable entry (src/host/guest_override_list.h)";
    case OVERRIDE_REFUSED_UNVERIFIED: return "the target executable was not verified";
    case OVERRIDE_REFUSED_NO_REPLACEMENT: return "no replacement";
    default: return "already replaced";
    }
}

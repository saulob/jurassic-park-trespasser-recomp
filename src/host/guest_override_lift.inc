/* One lift's stable entries and registry. The includer defines OVERRIDE_LIFT (the lift's symbol prefix, e.g. lift116_)
 * and OVERRIDE_LIST (its list of GUEST_OVERRIDE rows). A listed function whose lifted body is missing or still has its
 * plain name fails the link, never the run. */
#define GUEST_OVERRIDE OVERRIDE_LIFTED_DECL
#include OVERRIDE_LIST
#undef GUEST_OVERRIDE
#define GUEST_OVERRIDE OVERRIDE_STABLE_ENTRY
#include OVERRIDE_LIST
#undef GUEST_OVERRIDE

static override_entry_t OVERRIDE_JOIN(OVERRIDE_LIFT, entries)[] = {
#define GUEST_OVERRIDE OVERRIDE_ROW
#include OVERRIDE_LIST
#undef GUEST_OVERRIDE
};

static override_registry_t OVERRIDE_JOIN(OVERRIDE_LIFT, registry) = {
    .entries = OVERRIDE_JOIN(OVERRIDE_LIFT, entries),
    .count = sizeof OVERRIDE_JOIN(OVERRIDE_LIFT, entries) / sizeof OVERRIDE_JOIN(OVERRIDE_LIFT, entries)[0]
};

#include "lift_registry.h"

#include <stdio.h>
#include <string.h>

extern const jpt_lift_meta_t lift116_meta, lift_es117_meta, lift_ptbr117_meta;
extern const recomp_dispatch_entry_t lift116_dispatch_table[], lift_es117_dispatch_table[],
    lift_ptbr117_dispatch_table[];
extern void (*const jpt_gsub_by_lift[JPT_LIFT_COUNT][JPT_GSUB_COUNT])(void);

static const lift_image_t s_images[JPT_LIFT_COUNT] = {
    [JPT_LIFT_116] = { &lift116_meta, lift116_dispatch_table },
    [JPT_LIFT_ES117] = { &lift_es117_meta, lift_es117_dispatch_table },
    [JPT_LIFT_PTBR117] = { &lift_ptbr117_meta, lift_ptbr117_dispatch_table },
};

static const lift_image_t* s_active;

uint32_t lift_entry_va, lift_text_start, lift_text_end, lift_selfmod_start, lift_selfmod_end;

const lift_image_t* lift_image(jpt_lift_t lift) { return lift >= 0 && lift < JPT_LIFT_COUNT ? &s_images[lift] : NULL; }

const lift_image_t* lift_active_image(void) { return s_active; }

int lift_activate(const jpt_game_version_t* version, char* error, size_t cap) {
    jpt_lift_t lift;
    const lift_image_t* image;
    jpt_lift_meta_result_t result;
    if (!version || !jpt_lift_find(version->lift, &lift)) {
        if (error && cap) snprintf(error, cap, "%s needs the lift %s, which this program does not carry",
                                   version ? version->id : "(no version)", version ? version->lift : "?");
        return 0;
    }
    image = &s_images[lift];
    result = jpt_lift_meta_check(&jpt_lift_data[lift].expected, image->meta);
    if (result != JPT_LIFT_META_OK) {
        if (error && cap)
            snprintf(error, cap, "stale lift: %s for %s is %s (generated as %s from %s, SHA-256 %s; expected %s from %s)",
                     version->lift, version->id, jpt_lift_meta_result_text(result), image->meta->lift_id,
                     image->meta->source_profile, image->meta->source_sha256, jpt_lift_data[lift].expected.lift_id,
                     jpt_lift_data[lift].expected.source_profile);
        return 0;
    }
    if (!jpt_profile_activate(version, error, cap)) return 0;
    memcpy(jpt_gsub, jpt_gsub_by_lift[lift], sizeof jpt_gsub);
    native32_set_dispatch(image->dispatch, image->meta->function_count);
    lift_entry_va = image->meta->entry_va;
    lift_text_start = image->meta->text_start;
    lift_text_end = image->meta->text_end;
    lift_selfmod_start = image->meta->selfmod_start;
    lift_selfmod_end = image->meta->selfmod_end;
    s_active = image;
    return 1;
}

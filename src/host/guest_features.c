#include "guest_features.h"

#include "ce_gameplay.h"
#include "guest_abi.h"
#include "level_actions.h"
#include "custom_level_guide.h"
#include "scene_options.h"

void guest_features_configure(const jpt_config_t* cfg) {
    ce_gameplay_configure(cfg->dual_stow, cfg->max_object_detail);
    active_ai_configure(cfg->active_ai_enhanced);
    patch117_configure(cfg->official_patch_117);
    patch117_player_configure(cfg->official_patch_117);
    patch117_physics_configure(cfg->official_patch_117);
    widescreen_set_fov(cfg->fov);
    skip_intro_configure(cfg->skip_intro);
    scene_options_configure(cfg->fog_enabled, cfg->shadows_enabled);
    script_engine_configure(cfg->script_user_vk);
}

void guest_features_install(void) {
    physics_fixes_install();
    ai_vocals_install();
    scene_scripts_install();
    level_actions_install();
    script_engine_install();
    level_guide_install();
    ce_gameplay_install();
    active_ai_install();
    patch117_install();
    patch117_player_install();
    patch117_physics_install();
}

void guest_features_stop(void) {
    patch117_configure(0);
    patch117_player_configure(0);
    patch117_physics_configure(0);
    skip_intro_configure(0);
}

void guest_features_step(void) {
    ce_gameplay_step();
    level_actions_step();
    widescreen_apply_fov();
    scene_options_step();
}

#ifndef JPT_VK_WIN_TEST_H
#define JPT_VK_WIN_TEST_H

#include <stddef.h>
#include <stdint.h>

#include "vk_renderer.h"
#include "vk_win_account.h"

typedef struct {
    unsigned long long guest_draws;
    unsigned long long bridge_skipped;
    unsigned long long texture_skips;
    unsigned long long untracked_writes;
    unsigned long long palette_refreshes;
    unsigned long long water_requested;
    unsigned long long sky_requested;
    unsigned long long composites;
    unsigned long long uploads;
    unsigned long long readbacks;
    unsigned long long tiles_read;
    unsigned long long tiles_kept;
    unsigned long long readbacks_avoided;
    unsigned long long readback_rects;
    unsigned long long readback_failures;
    unsigned long long overlay_failures;
    unsigned long long cpu_polygons;
    unsigned long long last_frame_reads;
    unsigned long long still_draws;
    unsigned long long mipped_textures;
    unsigned long long dynamic_textures;
    unsigned long long still_uploads;
    long validation_errors;
    int validation_enabled;
} vk_win_test_bridge_stats_t;

/* Headless harness for the Windows backend: the same translation as in the game, drawn into an offscreen target. */
int vk_win_test_enable_headless(uint32_t width, uint32_t height);
void vk_win_test_set_stats(int on);
void vk_win_test_set_effects(int water_refraction, int enhanced_sky, int ao_level);
void vk_win_test_shutdown(void);
int vk_win_test_readback(uint8_t* rgba, size_t bytes, uint32_t* width, uint32_t* height);
void vk_win_test_stats(jpt_vk_renderer_stats_t* out);
void vk_win_test_bridge_stats(vk_win_test_bridge_stats_t* out);
void vk_win_test_set_ui_state(int ao_scene);
void vk_win_test_account(vka_t* out);
void vk_win_test_set_filter(int jpt_filter);

#endif

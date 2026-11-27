// A script session over the model world of script_world.h, for the suites that exercise one engine function at a time
// (script_draw_tests, script_input_tests, script_player_tests). Each suite brings its own minimal scripts.
#pragma once

#include "script_fixtures.h"
#include "script_world.h"

static World g_world;
static jpt_script_engine g_engine;

static inline jpt_script_session* ApiSession(const Files& files, long instructions = 200000, size_t memory = 0) {
    jpt_script_session_destroy(Session(files));
    g_world = World{};
    g_world.player = g_world.Add("Player", "CPlayer");
    g_world.pictures["textures/white.tga"] = {8, 4, 0xFFFFFFFFu};
    g_engine = EngineOf(g_world);
    jpt_script_session_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.read = Read;
    config.instructions = instructions;
    config.memory_limit = memory;
    config.allocator = &kFake;
    config.engine = &g_engine;
    jpt_script_session* s = jpt_script_session_create(&config);
    for (const auto& [path, text] : files) jpt_script_session_add_file(s, path.c_str());
    g_lines.clear();
    return s;
}

static inline void Ready(jpt_script_session* s, const char* scene, jpt_script_begin why = JPT_SCRIPT_BEGIN_NEW_GAME) {
    Begin(s, scene, why);
    jpt_script_level_ready(s, 1);
}

static inline jpt_script_input_result Input(jpt_script_session* s, uint32_t held, uint32_t hit, float move_y = 0.0f) {
    jpt_script_input in{held, hit, 0.0f, move_y};
    jpt_script_input_result out{};
    jpt_script_level_input(s, &in, &out);
    return out;
}

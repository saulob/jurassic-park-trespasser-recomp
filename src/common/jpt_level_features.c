#include "jpt_level_features.h"

#include <stdio.h>
#include <string.h>

#define FULL JPT_FEATURE_FULLY_SUPPORTED
#define PART JPT_FEATURE_PARTIALLY_SUPPORTED
#define NONE JPT_FEATURE_NOT_SUPPORTED
#define FREE JPT_FEATURE_NOT_REQUIRED
#define UNKNOWN JPT_FEATURE_UNKNOWN
#define ACTIONS "src/host/level_actions.c"

static const jpt_level_feature_t k_features[] = {
    { "ATX_ACTION_99", "Scripted action", NONE, "", "an ActionType script (ScriptAT) needs CE's scripting API, which the port does not have yet" },
    { "ATX_ACTION_100", "Slow motion for a while", FULL, ACTIONS, "the interval is the time between its transitions; kept in saves" },
    { "ATX_ACTION_101", "Set game speed", FULL, ACTIONS, "kept in saves, back to normal at each load" },
    { "ATX_ACTION_102", "Set map picture", FULL, ACTIONS, "Show Objective shows the picture below the title with Anne's place, instead of ATX's MAP cheat; kept in saves" },
    { "ATX_ACTION_103", "Reload carried guns", FULL, ACTIONS, "the runs left are kept in saves" },
    { "ATX_ACTION_104", "Change health", FULL, ACTIONS, "the once-only run is kept in saves" },
    { "ATX_ACTION_105", "Play a video", FULL, ACTIONS, "in the game's own video window, the game paused; the run is kept in saves" },
    { "ATX_ACTION_106", "Show hint text", FULL, ACTIONS, "menu/hint_strings.txt, kept in saves, ended by the game's own hints" },
    { "ATX_ACTION_107", "Reserved by ATX", FREE, "", "ATX makes no action of it" },
    { "ATX_ACTION_108", "Reserved by ATX", FREE, "", "ATX makes no action of it" },
    { "ATX_ACTION_109", "Particle colour", FULL, ACTIONS, "at each load of the level, as the particles had it when the level ends" },
    { "ATX_ACTION_110", "Fixed speed of an object", FULL, ACTIONS, "kept in saves" },
    { "ATX_ACTION_111", "End a fixed speed", FULL, ACTIONS, "" },
    { "ATX_ACTION_112", "Keyboard and mouse on or off", FULL, ACTIONS, "kept in saves, all on at each load" },
    { "ATX_ACTION_113", "AI type of the player", FULL, ACTIONS, "of the player's parts alone, kept in saves, as it was at each load" },
    { "ATX_ACTION_114", "Randomized teleport", FULL, ACTIONS, "through the game's own teleport" },
    { "ATX_ACTION_115", "Use an object, fire a gun", FULL, ACTIONS, "through the game's own use" },
    { "ATX_ACTION_116", "Make an animal jump", FULL, ACTIONS, "through the game's own impulse" },
    { "ATX_ACTION_117", "Particle effect", FULL, ACTIONS, "through the game's own particles" },
    { "ATX_ACTION_118", "Swap a texture", FULL, ACTIONS, "kept in saves, as it was at each load" },
    { "ATX_ACTION_119", "Rain", FREE, "", "an option of ATX's, off unless its settings turn it on; Trespasser CE has none" },
    { "ATX_ACTION_120", "Fire count and probability of a trigger", FULL, ACTIONS, "the game's saves keep both, the mark of a save the count it starts again with" },
    { "ATX_ACTION_121", "Reload guns", FULL, ACTIONS, "carried or not" },
    { "ATX_MAP", "Map picture files (MAPS)", FULL, "src/common/jpt_custom_level.c", "" },
    { "ATX_LOADING_PICTURE", "Loading pictures (MAPS/LBG)", FULL, "launcher/level_analysis.cpp", "a LoadingImage of fm.ini holds over them" },
    { "ATX_LEVEL_SOUNDS", "Sound files of a level (MAPS/LVL)", FULL, "launcher/level_analysis.cpp", "added as Trespasser CE adds its own" },
    { "ATX_DINOSAUR_SOUNDS", "Sounds of more than 7 kinds of animals", FULL, "src/host/ai_vocals.c", "30 kinds, as ATX's default" },
    { "ATX_HUD", "Health and ammunition pictures (MAPS/HUD)", FREE, "", "an option of ATX's, off by default" },
    { "ATX_RANDOMIZER", "Places drawn by lot (MAPS/RAN)", FREE, "", "an option of ATX's, off by default" },
    { "ATX_NIGHT_MODE", "Night mode (MAPS/NMD)", FREE, "", "an option of ATX's, off by default" },
    { "ATX_RAIN", "Rain or snow of particles (MAPS/RNE)", FREE, "", "an option of ATX's, off by default; Trespasser CE has none, and a level whose package turns it on is played without it" },
    { "ATX_MENU_TEXTS", "Texts of ATX's menus", FREE, "", "the port has its own menus" },
    { "ATX_JUMPS", "Animals that jump as they bite (MAPS/JMP)", NONE, "", "Trespasser CE does not have them" },
    { "ATX_PROJECTILES", "Animals that throw objects (MAPS/SPT)", NONE, "", "Trespasser CE does not have them" },
    { "ATX_PLUGIN", "ATX plugins (MAPS/PLG)", NONE, "", "programs of a package are never run" },
    { "ATX_SETTINGS", "Rules changed in ATXconfig.ini", NONE, "", "ATX's own settings of the animals, the gravity and the fog" },
    { "CE_START_LEVEL", "StartLevel", FULL, "launcher/level_analysis.cpp", "" },
    { "CE_MOD_PATHS", "ModPaths, UberModPaths", FULL, "launcher/level_analysis.cpp", "" },
    { "CE_TPA_ADDED", "Sound files added (+=)", FULL, "src/host/stream_vfile.c", "" },
    { "CE_TPA_REPLACED", "Sound files replaced (=)", FULL, "src/common/jpt_custom_level.c", "" },
    { "CE_MENU_TPA", "Menu sounds", FREE, "", "frontend sound only" },
    { "CE_LOADING_IMAGE", "LoadingImage", FULL, "src/common/jpt_custom_level.c", "" },
    { "CE_WIN_MOVIE", "WinMovie", FULL, "src/host/skip_intro.c", "" },
    { "CE_MENU_FILES", "Menu replacements", FREE, "", "the launcher and the game's own Level Select start every level of a package" },
    { "CE_VIDEOS", "Replacement videos", FREE, "", "opening and ending videos of the frontend; a session starts in its level" },
    { "CE_END_GAME_DDF", "EndGameDDF", NONE, "", "the end screen is the original game's" },
    { "CE_ANIM_SCRIPTS", "Animation scripts", FULL, "src/common/jpt_custom_level.c", "" },
    { "CE_SCN_23_25", "Scenes of version 23 to 25", FULL, "src/common/jpt_scene.c", "" },
    { "CE_SCN_26", "Scenes of version 26", FULL, "src/host/scene_scripts.c", "marks of object scripts are read past" },
    { "CE_SCRIPTS", "Object scripts (Squirrel)", FULL, "src/common/jpt_script_session.cpp", "a VM for the levels whose scripts act in them: lifecycle, frame and nvram" },
    { "CE_SCRIPT_API", "Trespasser CE script functions", NONE, "", "the engine functions, classes and handlers of CE's scripting API are not there yet" },
    { "CE_SCRIPT_FILES", "Object scripts left out", NONE, "", "an earlier version left the package's scripts out when it installed it: import it again" },
    { "CE_FULL_RESTART", "DisableSimpleLevelRestart", FULL, ACTIONS, "Restart Level and loads purge the world first" },
    { "CE_DUAL_STOW", "EnableDualStow", FULL, "src/host/ce_gameplay.c", "" },
    { "CE_ENHANCED_SKY", "EnhancedSky", FULL, "src/host/ce_graphics.c", "" },
    { "CE_WATER_REFRACTION", "WaterRefraction", FULL, "src/host/ce_graphics.c", "" },
    { "CE_MAX_ACTIVE_AI", "MaxActiveAi", FULL, "src/host/active_ai.c", "6 animals, Trespasser CE's documented most" },
    { "CE_EXTENDED_AI", "EnableAiActivityEx", NONE, "", "14 activity classes that the 1998 engine does not have" },
    { "CE_AI_ACTIVATE_DISTANCE", "MaxAiActivateOnRenderDist", NONE, "", "" },
    { "CE_WOOD_SAP", "EnableWoodSapParticles", NONE, "", "" },
    { "CE_MESH_HEAP", "MeshHeapSize", FULL, "src/host/engine_heaps.c", "by the size of the content" },
    { "CE_MESH_FACES", "Meshes past 2048 polygons", FULL, "src/host/engine_heaps.c", "by the size of the content, up to 8192" },
    { "CE_TERRAIN_NODES", "ExtraTerrainNodes", FULL, "src/host/engine_heaps.c", "" },
    { "CE_TEXTURE_POOL", "Texture sets past 128 MB", FULL, "src/host/engine_heaps.c", "by the size of the content, up to 256 MB" },
    { "CE_TEXHW_BMP", "texhw colour textures, BMP", FULL, "src/common/jpt_texhw.c", "8, 24 and 32 bits; hardware renderers" },
    { "CE_TEXHW_TGA", "texhw colour textures, TGA", FULL, "src/common/jpt_texhw.c", "hardware renderers" },
    { "CE_TEXHW_DDS", "texhw colour textures, DDS", FULL, "src/common/jpt_texhw.c", "DXT1 to DXT5 and uncompressed, with their mips; hardware renderers" },
    { "CE_TEXHW_ODD_SIZE", "texhw over a texture whose sides are no powers of two", NONE, "", "the engine's surface is larger than the texture" },
    { "CE_TEXHW_BUMPED", "texhw textures of meshes the original game bump-maps", NONE, "", "the engine draws those meshes on the CPU" },
    { "CE_NORMAL_MAPS", "texhw normal maps of other meshes", FULL, "src/common/jpt_lit.c", "hardware renderers; a vertex's normal is told from its own light and the faces around it" },
    { "CE_SKY", "Custom sky: DOME, BODY, STRIP", FULL, "src/common/jpt_sky.c", "hardware renderers" },
    { "CE_SKY_2X", "Custom sky: 2X blend modes", FULL, "src/common/jpt_sky.c", "hardware renderers" },
    { "CE_SKY_STARS", "Custom sky: STARS, METEORS", NONE, "", "no level of the corpus has them" },
    { "CE_TERRAIN_CULL_BIAS", "TerrainBackfaceCullBias", FULL, "src/host/engine_heaps.c", "of the hardware renderers' terrain" },
};

const jpt_level_feature_t* jpt_level_feature_at(int index) {
    return index >= 0 && index < (int)(sizeof k_features / sizeof k_features[0]) ? &k_features[index] : NULL;
}

const jpt_level_feature_t* jpt_level_feature(const char* name) {
    const jpt_level_feature_t* f;
    for (int i = 0; (f = jpt_level_feature_at(i)) != NULL; i++)
        if (!strcmp(f->name, name)) return f;
    return NULL;
}

const char* jpt_feature_status_name(jpt_feature_status_t status) {
    switch (status) {
    case FULL: return "FULLY_SUPPORTED";
    case PART: return "PARTIALLY_SUPPORTED";
    case NONE: return "NOT_SUPPORTED";
    case FREE: return "NOT_REQUIRED";
    case UNKNOWN: break;
    }
    return "UNKNOWN";
}

int jpt_level_feature_playable(const char* name) {
    const jpt_level_feature_t* f = jpt_level_feature(name);
    return f && (f->status == FULL || f->status == FREE);
}

const jpt_level_feature_t* jpt_level_action_feature(int type) {
    char name[32];
    snprintf(name, sizeof name, "ATX_ACTION_%d", type);
    return jpt_level_feature(name);
}

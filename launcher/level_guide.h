// A custom level's guide, made from its world as the port's tools make the campaign's: the minimap (tools/minimap_bounds.py and
// tools/minimap_map.py, the same rules) and the objectives its own hint and exit triggers lead through. The game reads it from
// <package>/.runtime/guide/<scene>.txt (src/host/level_guide.c).
#pragma once

#include <string>
#include <vector>

#include "jpt_groff.h"

namespace jpt {

inline constexpr int kLevelGuideVersion = 1;
inline constexpr char kLevelGuideFolder[] = ".runtime/guide";

struct GuideRun {
    int start = 0, length = 0, row = 0, kind = 0;
};
struct GuideLandmark {
    int kind = 0;
    double x[4] = {}, y[4] = {};
};
struct GuideVehicle {
    double x = 0, y = 0, ax = 0, ay = 0;
    int size = 0;
};
struct GuideObjective {
    int hint = -1; // -1: the level sets no hint, its exit is the one objective
    std::vector<std::string> targets;
};

struct LevelGuide {
    bool map = false;
    std::string no_map; // why there is none
    double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    int cols = 0, rows = 0;
    std::vector<GuideRun> runs;
    std::vector<GuideLandmark> landmarks;
    std::vector<GuideVehicle> vehicles;
    std::vector<GuideObjective> objectives;
    unsigned objects = 0, playable = 0;
};

LevelGuide BuildLevelGuide(const jpt_groff_t* world);
// world: the world file's name; in_game: it is the installed game's own, so the campaign's guide of that level holds.
std::string LevelGuideText(const LevelGuide& guide, const std::string& world, bool in_game);
// The guide's file name for a scene: its file name in lower case, with ".txt".
std::string LevelGuideName(const std::string& scene_file);
// One line for the launcher's log.
std::string LevelGuideSummary(const LevelGuide& guide);

} // namespace jpt

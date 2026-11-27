// What each setting of a fan mission's INI files means to the port. Nothing here is ever saved: a setting is either a session
// override of the package, or a note in its manifest.
#pragma once

#include <string>

#include "level_ini.h"

namespace jpt {

// Trespasser CE's own mesh heap, the one every level made for it can count on.
inline constexpr long kMeshHeapMegabytes = 40, kMeshHeapMegabytesMax = 128, kTerrainNodesMax = 300, kTerrainCullMax = 10000;

enum class SettingFile { kFmIni, kTpModIni, kTrespasserIni, kTpassIni };

enum class SettingUse {
    kApplied,     // in effect while the package plays
    kMapped,      // in effect through the port's own setting
    kLimit,       // in effect as an engine limit of the session
    kBuiltIn,     // the port already behaves so
    kRecommended, // the author's recommendation; the player's setting stays
    kUserChoice,  // the launcher's setting stays
    kObsolete,    // belongs to the engine the file was written for
    kNotActive,   // recognised, no equivalent yet
    kRequired,    // a feature the level needs
    kIneffective, // not read where it stands, by Trespasser CE either
    kInvalid,     // a value the key cannot have
    kUnknown,
};

const char* SettingFileName(SettingFile file);
const char* SettingUseName(SettingUse use);

struct SettingMeaning {
    SettingUse use = SettingUse::kUnknown;
    std::string key, value; // the port's setting or limit, for kMapped and kLimit; the feature, for kRequired
    std::string note;
};

// Entries about levels, sounds and folders (StartLevel, *TPA, LoadingImage, FMPath...) are read by the analysis itself.
bool IsStructuralSetting(SettingFile file, const IniEntry& entry);
SettingMeaning MeaningOf(SettingFile file, const std::string& path, const IniEntry& entry);

} // namespace jpt

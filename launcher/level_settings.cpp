#include "level_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

#include "jpt_config.h"

namespace jpt {
namespace {

enum class Map { kNone, kBool, kActiveAi, kMegabytes, kThousands, kThousandths, kEngineFile };

struct Row {
    const char* key;
    SettingUse use;
    Map map;
    const char* setting;
    const char* note;
};

constexpr const char* kLimits = "engine limits are the port's own";
constexpr const char* kDistances = "draw distances and detail are the port's own";
constexpr const char* kDisplay = "renderer, GPU, resolution and display are chosen in the launcher";
constexpr const char* kSound = "the audio device and format are the port's own";
constexpr const char* kDebug = "engine debugging, not part of the level";
constexpr const char* kControls = "the player's own controls stay";

constexpr Row kRows[] = {
    {"EnableDualStow", SettingUse::kMapped, Map::kBool, JPT_KEY_DUAL_STOW, ""},
    {"EnhancedSky", SettingUse::kMapped, Map::kBool, JPT_KEY_ENHANCED_SKY, "with a hardware renderer"},
    {"WaterRefraction", SettingUse::kMapped, Map::kBool, JPT_KEY_WATER_REFRACTION, "with a hardware renderer"},
    {"MaxActiveAi", SettingUse::kMapped, Map::kActiveAi, JPT_KEY_ACTIVE_AI, ""},
    {"NewGameMovie", SettingUse::kBuiltIn, Map::kNone, "", "a custom level starts without the opening videos"},
    {"StartMovie", SettingUse::kBuiltIn, Map::kNone, "", "a custom level starts without the opening videos"},
    {"EnableQuickSaveKey", SettingUse::kBuiltIn, Map::kNone, "", "Quick Save is always available"},
    {"TryIncompleteInstall", SettingUse::kBuiltIn, Map::kNone, "", "the port has no installation check"},
    {"SavesShotsDirs", SettingUse::kBuiltIn, Map::kNone, "", "each package keeps its own saves"},
    {"PixelShaderBump", SettingUse::kBuiltIn, Map::kNone, "", "the hardware renderers bump-map objects"},
    {"MipMapMode", SettingUse::kBuiltIn, Map::kNone, "", "the port filters its textures"},
    {"AutoMipMap", SettingUse::kBuiltIn, Map::kNone, "", "the port filters its textures"},
    {"ForceMaxObjectDetail", SettingUse::kRecommended, Map::kNone, "", "Max Object Detail is the player's setting"},
    {"FOV", SettingUse::kUserChoice, Map::kNone, "", "the player's field of view stays"},
    {"LockHorizFOV", SettingUse::kUserChoice, Map::kNone, "", "the player's field of view stays"},
    {"DisableSimpleLevelRestart", SettingUse::kRequired, Map::kEngineFile, "CE_FULL_RESTART", "General"},
    {"EnableAiActivityEx", SettingUse::kRequired, Map::kEngineFile, "CE_EXTENDED_AI", "Game"},
    {"MaxAiActivateOnRenderDist", SettingUse::kRequired, Map::kEngineFile, "CE_AI_ACTIVATE_DISTANCE", "Game"},
    {"EnableWoodSapParticles", SettingUse::kRequired, Map::kEngineFile, "CE_WOOD_SAP", "Game"},
    {"EndGameDDF", SettingUse::kRequired, Map::kNone, "CE_END_GAME_DDF", ""},
    {"HWTextureOverrides", SettingUse::kBuiltIn, Map::kNone, "", "texhw pictures are used where the level has them"},
    {"UseDummyForMissingTextures", SettingUse::kBuiltIn, Map::kNone, "", "a grey picture keeps a missing texture from ending the game"},
    {"TerrainBackfaceCullBias", SettingUse::kLimit, Map::kThousandths, "terrain_cull_mm", "for this session"},
    {"MeshHeapSize", SettingUse::kLimit, Map::kMegabytes, "mesh_heap_mb", "for this session"},
    {"ExtraTerrainNodes", SettingUse::kLimit, Map::kThousands, "terrain_nodes", "for this session"},
    {"UseRecommendedQuality", SettingUse::kUserChoice, Map::kNone, "", "quality is chosen in the launcher"},
    {"Language", SettingUse::kUserChoice, Map::kNone, "", "the language is chosen in the launcher"},
    {"TimestampScreenshots", SettingUse::kObsolete, Map::kNone, "", ""},
    {"UseDX9", SettingUse::kUserChoice, Map::kNone, "", kDisplay},
    {"MultiSamplingLevel", SettingUse::kUserChoice, Map::kNone, "", kDisplay},
    {"EnableVSync", SettingUse::kUserChoice, Map::kNone, "", kDisplay},
    {"MaxFramerate", SettingUse::kUserChoice, Map::kNone, "", kDisplay},
    {"TexFilterD3D", SettingUse::kUserChoice, Map::kNone, "", kDisplay},
    {"FullscreenRefreshRate", SettingUse::kUserChoice, Map::kNone, "", kDisplay},
    {"32bitHardwareTextures", SettingUse::kObsolete, Map::kNone, "", "the hardware renderers draw in 32 bits"},
    {"ForceNoBlendTrans", SettingUse::kObsolete, Map::kNone, "", ""},
    {"RenderHardwareOnly", SettingUse::kObsolete, Map::kNone, "", ""},
    {"UseAL", SettingUse::kObsolete, Map::kNone, "", kSound},
};

struct SectionRow {
    const char* section;
    SettingUse use;
    const char* note;
};

constexpr SectionRow kSections[] = {
    {"Debug", SettingUse::kObsolete, kDebug},
    {"Controls", SettingUse::kUserChoice, kControls},
    {"Display", SettingUse::kObsolete, kDisplay},
    {"DisplayDX9", SettingUse::kObsolete, kDisplay},
    {"Sound", SettingUse::kObsolete, kSound},
    {"SoundOpenAL", SettingUse::kObsolete, kSound},
    {"Memory", SettingUse::kObsolete, kLimits},
    {"Render", SettingUse::kObsolete, kDistances},
    {"MaxQualityParams", SettingUse::kObsolete, kDistances},
    {"RecommendedQuality", SettingUse::kObsolete, kDistances},
};

// The registry settings of the original game, which tpass.ini stands in for.
constexpr const char* kRegistry[] = {
    "Safemode", "Auto Settings", "RenderQuality", "Width", "Height", "AutoLoad Temp", "XView", "YView", "Registry Init",
    "Max Recommended Tex Dim", "Z Buffer Bitdepth", "Hardware Water", "Page Managed", "Triple Buffer", "Dither", "PlaySFX", "PlayAmbient",
    "PlayVoiceOver", "PlayMusic", "UseSubTitles", "AudioEnable3D", "DX9 Adapter", "Gamma", "MasterVolume", "Gore", "Use D3D", "InvertMouse",
    "KeyMap", "KeyMapEx", "Full Screen", "Bitdepth", "System Memory", "DetailLevel", "D3D Device", "Driver", "Installed Directory",
    "Data Drive", "Sound Options", "MigrateFromRegistryNextRun"};

bool IsSceneSection(const std::string& section) {
    return section.size() > 4 && SameText(std::string_view(section).substr(section.size() - 4), ".scn");
}

// A length in metres, in thousandths of one; Trespasser CE takes none that is not above 0.
SettingMeaning Length(const Row& row, const IniEntry& entry) {
    char* end = nullptr;
    const double metres = std::strtod(entry.value.c_str(), &end);
    if (end == entry.value.c_str()) return {SettingUse::kInvalid, "", "", "not a value of this key"};
    if (!(metres > 0)) return {SettingUse::kBuiltIn, "", "", "off, as in the original game"};
    const long thousandths = std::clamp(std::lround(std::min(metres, 1000.0) * 1000.0), 1L, kTerrainCullMax);
    return {row.use, row.setting, std::to_string(thousandths), row.note};
}

SettingMeaning Limited(const Row& row, const IniEntry& entry) {
    if (row.map == Map::kThousandths) return Length(row, entry);
    const bool megabytes = row.map == Map::kMegabytes;
    const long lowest = megabytes ? kMeshHeapMegabytes : 0, highest = megabytes ? kMeshHeapMegabytesMax : kTerrainNodesMax;
    long number = 0;
    if (!ParseIniInt(entry.value, number) || number < 0) return {SettingUse::kInvalid, "", "", "not a value of this key"};
    SettingMeaning meaning{row.use, row.setting, std::to_string(std::clamp(number, lowest, highest)), row.note};
    if (number > highest) meaning.note = "more than this port reserves: " + meaning.value;
    if (number < lowest) meaning.note = "the least there is: " + meaning.value;
    return meaning;
}

// Trespasser CE reads these in its own settings file alone, trespasser.ini beside the program.
SettingMeaning OfEngineFile(const Row& row, SettingFile file, const std::string& path, const IniEntry& entry) {
    bool on = false;
    long number = 0;
    const size_t slash = path.rfind('/');
    if ((ParseIniBool(entry.value, on) && !on) || (ParseIniInt(entry.value, number) && !number)) return {SettingUse::kBuiltIn, "", "", "off, as in the original game"};
    if (file != SettingFile::kTrespasserIni || !SameText(entry.section, row.note)) return {SettingUse::kIneffective, "", "", "Trespasser CE does not read it here"};
    if (!SameText(slash == std::string::npos ? path : path.substr(slash + 1), "trespasser.ini"))
        return {SettingUse::kRecommended, "", "", "in a settings file that the player may take over by hand"};
    return {SettingUse::kRequired, row.setting, "", ""};
}

SettingMeaning Mapped(const Row& row, SettingFile file, const std::string& path, const IniEntry& entry) {
    if (row.map == Map::kEngineFile) return OfEngineFile(row, file, path, entry);
    if (row.use == SettingUse::kLimit) return Limited(row, entry);
    SettingMeaning meaning{row.use, row.setting, "", row.note};
    bool on = false;
    long number = 0;
    if (row.map == Map::kBool && ParseIniBool(entry.value, on)) meaning.value = on ? "1" : "0";
    if (row.map == Map::kActiveAi && ParseIniInt(entry.value, number)) meaning.value = jpt_active_ai_keys[number > jpt_active_ai_limits[0] ? 1 : 0];
    if (meaning.value.empty() || !jpt_config_valid(meaning.key.c_str(), meaning.value.c_str())) return {SettingUse::kInvalid, "", "", "not a value of this key"};
    return meaning;
}

} // namespace

const char* SettingFileName(SettingFile file) {
    switch (file) {
    case SettingFile::kFmIni: return "fm.ini";
    case SettingFile::kTpModIni: return "tp_mod.ini";
    case SettingFile::kTrespasserIni: return "trespasser.ini";
    case SettingFile::kTpassIni: return "tpass.ini";
    }
    return "";
}

const char* SettingUseName(SettingUse use) {
    switch (use) {
    case SettingUse::kApplied: return "applied";
    case SettingUse::kMapped: return "mapped";
    case SettingUse::kLimit: return "session_limit";
    case SettingUse::kBuiltIn: return "built_in";
    case SettingUse::kRecommended: return "recommended";
    case SettingUse::kUserChoice: return "user_choice";
    case SettingUse::kObsolete: return "obsolete";
    case SettingUse::kNotActive: return "not_active";
    case SettingUse::kRequired: return "required";
    case SettingUse::kIneffective: return "ineffective";
    case SettingUse::kInvalid: return "invalid";
    case SettingUse::kUnknown: break;
    }
    return "unknown";
}

bool IsStructuralSetting(SettingFile file, const IniEntry& entry) {
    static constexpr const char* kPacks[] = {"StreamTPA", "EffectsTPA", "AmbientTPA", "MenuTPA", "StartLevel", "WinMovie"};
    static constexpr const char* kPaths[] = {"FMPath", "ModPaths", "UberModPaths", "ActiveFM"};
    const auto is = [&entry](const char* key) { return SameText(entry.key, key); };
    if (file == SettingFile::kTpModIni) return std::any_of(std::begin(kPaths), std::end(kPaths), is);
    if (file != SettingFile::kFmIni) return false;
    if (IsSceneSection(entry.section)) return is("LoadingImage") || std::any_of(std::begin(kPacks), std::begin(kPacks) + 4, is);
    return SameText(entry.section, "General") && std::any_of(std::begin(kPacks), std::end(kPacks), is);
}

SettingMeaning MeaningOf(SettingFile file, const std::string& path, const IniEntry& entry) {
    if (file == SettingFile::kTpassIni) {
        const bool known = std::any_of(std::begin(kRegistry), std::end(kRegistry), [&entry](const char* key) { return SameText(entry.key, key); });
        if (known) return {SettingUse::kUserChoice, "", "", "the player's own game settings stay"};
        return {};
    }
    for (const Row& row : kRows) {
        if (!SameText(entry.key, row.key)) continue;
        if (row.map != Map::kNone) return Mapped(row, file, path, entry);
        return {row.use, row.use == SettingUse::kRequired ? row.setting : "", "", row.note};
    }
    for (const SectionRow& row : kSections)
        if (SameText(entry.section, row.section)) return {row.use, "", "", row.note};
    return {};
}

} // namespace jpt

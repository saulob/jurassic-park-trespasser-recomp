#include "level_manifest.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <set>

#include "jpt_custom_level.h"
#include "jpt_level_features.h"
#include "json_text.h"
#include "updater/json.h"
#include "updater/update_paths.h"

namespace jpt {
namespace {

using Json = JsonText;

std::string PackName(const AudioPack& pack) {
    const char* names[] = {"StreamTPA", "EffectsTPA", "AmbientTPA"};
    return std::string(names[int(pack.kind)]) + (pack.additive ? "+=" : "=") + pack.declared;
}

const char* ConfigKey(SettingFile kind) {
    switch (kind) {
    case SettingFile::kFmIni: return "fm_ini";
    case SettingFile::kTpModIni: return "tp_mod_ini";
    case SettingFile::kTrespasserIni: return "trespasser_ini";
    case SettingFile::kTpassIni: return "tpass_ini";
    }
    return "";
}

void WriteAudio(Json& json, const PackageAnalysis& a) {
    json.Open("audio", '{');
    for (const AudioKind kind : {AudioKind::kStream, AudioKind::kEffects, AudioKind::kAmbient}) {
        json.Open((std::string(AudioKindName(kind)) + "_tpa").c_str(), '[');
        for (const AudioPack& pack : a.audio) {
            if (pack.kind != kind) continue;
            json.Open(nullptr, '{').Text("declared", pack.declared).Text("file", pack.path).Flag("additive", pack.additive);
            json.Text("level", pack.scene).Text("state", pack.state).Close();
        }
        json.Close();
    }
    json.Close();
}

void WriteLevels(Json& json, const PackageAnalysis& a) {
    json.Open("levels", '[');
    for (const LevelInfo& level : a.levels) {
        const auto image = std::find_if(a.images.begin(), a.images.end(), [&level](const LoadingImage& i) { return SameText(i.scene, level.file); });
        json.Open(nullptr, '{').Text("scn", level.path).Text("display_name", level.display_name).Flag("valid", level.valid);
        json.Flag("start_level", level.start).Flag("in_active_mod", level.configured).Flag("auxiliary", level.auxiliary);
        json.Number("scene_version", level.version).Text("auxiliary_reason", level.auxiliary_reason).Text("loading_image", image == a.images.end() || !level.configured ? "" : image->path);
        json.List("worlds", level.worlds).Close();
    }
    json.Close();
}

void WriteSettings(Json& json, const PackageAnalysis& a) {
    json.Open("settings", '[');
    for (const ConfigSetting& s : a.settings) {
        json.Open(nullptr, '{').Text("file", s.path).Text("section", s.entry.section).Text("key", s.entry.key).Text("operator", s.entry.additive ? "+=" : "=");
        json.Text("value", s.entry.value).Text("use", SettingUseName(s.meaning.use)).Text("note", s.meaning.note).Close();
    }
    json.Close();
    json.Open("session_overrides", '[');
    for (const SessionOverride& o : a.overrides) json.Open(nullptr, '{').Text("key", o.key).Text("value", o.value).Text("source", o.source).Close();
    json.Close();
    json.Open("session_limits", '[');
    for (const SessionLimit& l : a.limits) json.Open(nullptr, '{').Text("key", l.key).Number("value", l.value).Text("source", l.source).Close();
    json.Close();
    json.Open("unsupported_settings", '[');
    for (const ConfigSetting& s : a.settings)
        if (s.meaning.use == SettingUse::kNotActive || (s.meaning.use == SettingUse::kRequired && !jpt_level_feature_playable(s.meaning.key.c_str())) || s.meaning.use == SettingUse::kUnknown || s.meaning.use == SettingUse::kInvalid)
            json.Text(nullptr, std::string(SettingFileName(s.kind)) + ": " + s.entry.key);
    json.Close();
}

void WriteRuntime(Json& json, const PackageAnalysis& a) {
    json.Open("install_actions", '[');
    for (const InstallAction& action : a.actions) {
        json.Open(nullptr, '{').Text("action", action.kind).Text("source", action.source).Text("target", action.target);
        json.Text("origin", OriginName(action.origin)).Text("reason", action.reason).Close();
    }
    json.Close();
    json.Open("runtime", '[');
    for (const RuntimeFile& file : a.runtime) {
        json.Open(nullptr, '{').Text("target", file.target).Text("source", file.source).Text("origin", OriginName(file.origin));
        json.Text("state", file.active ? "active" : "preserved_not_active").Close();
    }
    json.Close();
}

double SchemaOf(const trg::update::json::Value& root) {
    const auto* schema = root.Find("schema_version");
    return schema && schema->IsNumber() ? schema->AsNumber() : 0;
}

std::vector<std::string> StringList(const trg::update::json::Value& root, const char* key) {
    std::vector<std::string> list;
    const auto* value = root.Find(key);
    if (!value || !value->IsArray()) return list;
    for (const auto& item : value->AsArray())
        if (item.IsString()) list.push_back(item.AsString());
    return list;
}

} // namespace

std::string DateText(int64_t seconds) {
    const std::time_t time = std::time_t(seconds);
    std::tm utc{};
    char text[80] = "";
#ifdef _WIN32
    if (gmtime_s(&utc, &time)) return {};
#else
    if (!gmtime_r(&time, &utc)) return {};
#endif
    std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return text;
}

int64_t DateOf(const std::string& text) {
    using namespace std::chrono;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    char end = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &s, &end) != 7 || end != 'Z') return 0;
    const year_month_day date{year(y), month(unsigned(mo)), day(unsigned(d))};
    if (!date.ok() || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) return 0;
    return duration_cast<seconds>(sys_days(date).time_since_epoch()).count() + h * 3600 + mi * 60 + s;
}

std::string ManifestJson(const PackageManifest& manifest) {
    const PackageAnalysis& a = manifest.analysis;
    Json json;
    json.Open(nullptr, '{');
    json.Number("schema_version", kManifestSchema).Text("package_id", manifest.package_id);
    json.Text("display_name", a.display_name.empty() ? manifest.package_id : a.display_name);
    json.Text("source_hash", manifest.origin.source_hash).Text("source_type", manifest.origin.source_type).Text("source_name", manifest.origin.source_name);
    json.Text("imported_at", manifest.origin.imported_at ? DateText(manifest.origin.imported_at) : "");
    json.Text("layout_type", a.layout).Text("mod_root", a.root).Text("start_level", a.start_level).Flag("skip_win_movie", a.skip_win_movie);
    WriteLevels(json, a);
    json.List("readmes", a.readmes);
    json.Open("configs", '{');
    for (const SettingFile kind : {SettingFile::kFmIni, SettingFile::kTpModIni, SettingFile::kTrespasserIni, SettingFile::kTpassIni}) {
        json.Open(ConfigKey(kind), '[');
        for (const ConfigFile& config : a.configs)
            if (config.kind == kind) json.Open(nullptr, '{').Text("file", config.path).Flag("used", config.used).Close();
        json.Close();
    }
    json.Close();
    WriteAudio(json, a);
    json.Open("directories", '{');
    for (const char* name : {"anim", "texhw", "menu", "maps"})
        json.Flag(name, std::find(a.directories.begin(), a.directories.end(), name) != a.directories.end());
    json.Close();
    json.Open("dependencies", '{');
    json.Text("ce", NeedName(a.need("Trespasser CE"))).Text("atx", NeedName(a.need("ATX"))).Open("evidence", '[');
    for (const Dependency& d : a.dependencies) json.Text(nullptr, d.name + ": " + d.evidence);
    json.Close().Close();
    json.Open("features", '{');
    for (const char* state : {"supported", "mapped", "not_required", "unsupported"}) {
        std::vector<std::string> names;
        for (const Feature& f : a.features)
            if (f.state == state) names.push_back(f.name);
        json.List(state, names);
    }
    json.Close();
    WriteSettings(json, a);
    WriteRuntime(json, a);
    json.Open("warnings", '[');
    for (const Warning& w : a.warnings) json.Open(nullptr, '{').Text("code", w.code).Text("detail", w.detail).Close();
    json.Close();
    json.List("refused_files", manifest.origin.refused);
    json.List("missing_required", manifest.missing);
    json.Close();
    return json.str() + "\n";
}

bool ManifestIsNewer(const std::string& text) {
    const std::optional<trg::update::json::Value> root = trg::update::json::Parse(text);
    return root && root->IsObject() && SchemaOf(*root) > kManifestSchema;
}

bool ReadManifestOrigin(const std::string& text, PackageOrigin& origin) {
    const std::optional<trg::update::json::Value> root = trg::update::json::Parse(text);
    if (!root || !root->IsObject() || SchemaOf(*root) < 1 || SchemaOf(*root) > kManifestSchema) return false;
    const auto field = [&root](const char* key) {
        const auto* value = root->Find(key);
        return value && value->IsString() ? value->AsString() : std::string();
    };
    origin = {field("source_type"), field("source_name"), field("source_hash"), StringList(*root, "refused_files"), DateOf(field("imported_at"))};
    return true;
}

std::string LoadingPictureFile(size_t index) { return std::string(JPT_CUSTOM_LEVEL_GENERATED_DIR) + "/loading" + std::to_string(index) + ".tga"; }

std::string RuntimeMap(const PackageAnalysis& a) {
    std::string map = "trespasser-runtime " + std::to_string(JPT_CUSTOM_LEVEL_RUNTIME_VERSION) + "\n";
    const auto line = [&map](const char* kind, std::initializer_list<std::string> fields) {
        map += kind;
        char separator = ' ';
        for (const std::string& field : fields) {
            map += separator + field;
            separator = '|';
        }
        map += '\n';
    };
    for (const LevelInfo& level : a.levels)
        if (level.configured) line("scene", {level.path});
    for (const RuntimeFile& file : a.runtime)
        if (file.active) line("file", {file.target, file.source});
    for (const AudioPack& pack : a.audio)
        if (pack.state == "ready") line(pack.additive ? "tpa+" : "tpa=", {AudioKindName(pack.kind), pack.scene.empty() ? "*" : pack.scene, pack.path});
    for (size_t i = 0; i < a.images.size(); ++i)
        if (a.images[i].state == "ready") line("loading", {a.images[i].scene, LoadingPictureFile(i)});
    for (const SessionOverride& o : a.overrides) line("set", {o.key, o.value});
    for (const SessionLimit& l : a.limits) line("limit", {l.key, std::to_string(l.value)});
    if (a.skip_win_movie) line("skip", {"win"});
    for (const int type : a.action_types) line("action", {std::to_string(type)});
    for (const auto& [feature, capability] : {std::pair("CE_FULL_RESTART", JPT_LEVEL_CAP_FULL_RESTART), std::pair("CE_SKY", JPT_LEVEL_CAP_SKY)})
        if (std::find(a.required.begin(), a.required.end(), feature) != a.required.end()) line("need", {jpt_custom_level_cap_name(capability)});
    return map;
}

bool IsGamePath(const std::string& target) {
    static constexpr const char* kNever[] = {".exe", ".dll", ".bat", ".cmd", ".ini", ".com", ".scr", ".img"};
    const std::string type = PathType(target);
    return !type.empty() && std::none_of(std::begin(kNever), std::end(kNever), [&type](const char* never) { return type == never; });
}

bool RuntimeIsSafe(const PackageAnalysis& a, const std::vector<PackageFile>& files, std::string& problem) {
    std::set<std::string> known, targets;
    for (const PackageFile& file : files) known.insert(file.path);
    for (const RuntimeFile& file : a.runtime) {
        std::string normalized;
        problem = file.target;
        if (!trg::update::NormalizeRelativePath(file.target, normalized) || normalized != file.target) return false;
        if (file.target.find('/') == std::string::npos || !IsGamePath(file.target)) return false;
        if (!known.count(file.source) || !targets.insert(LowerAscii(file.target)).second) return false;
    }
    for (const AudioPack& pack : a.audio) {
        problem = pack.path;
        if (!pack.path.empty() && !known.count(pack.path)) return false;
    }
    for (const LoadingImage& image : a.images) {
        problem = image.path;
        if (!image.path.empty() && !known.count(image.path)) return false;
    }
    problem.clear();
    return true;
}

bool IsGeneratedPath(const std::string& path) {
    const std::string lower = LowerAscii(path), folder = std::string(JPT_CUSTOM_LEVEL_GENERATED_DIR) + "/";
    return lower == kManifestFile || lower == JPT_CUSTOM_LEVEL_RUNTIME_MAP || lower.rfind(folder, 0) == 0 || lower + "/" == folder;
}

std::string AnalysisReport(const PackageManifest& manifest) {
    const PackageAnalysis& a = manifest.analysis;
    std::string out;
    const auto heading = [&out](const char* text) { out += std::string("\n") + text + "\n"; };
    const auto item = [&out](const std::string& text) { out += "  " + text + "\n"; };
    out += "Package: " + manifest.package_id + " (" + manifest.origin.source_type + ")\n";
    out += "Structure: " + a.layout + (a.root.empty() ? "" : ", mod folder " + a.root) + "\n";
    out += "Start level: " + (a.start_level.empty() ? std::string("none") : a.start_level) + "\n";
    heading("Levels");
    for (const LevelInfo& level : a.levels) {
        std::string text = level.path;
        if (!level.valid) text += "  NOT A SCENE";
        if (level.start) text += "  StartLevel";
        if (level.auxiliary) text += "  auxiliary (" + level.auxiliary_reason + ")";
        for (const std::string& world : level.worlds) text += "  " + world;
        item(text);
    }
    heading("Configuration files");
    for (const ConfigFile& c : a.configs) item(std::string(SettingFileName(c.kind)) + ": " + c.path + (c.used ? "" : "  (kept, not used)"));
    heading("Extra sound files");
    for (const AudioPack& p : a.audio) item(PackName(p) + (p.scene.empty() ? "" : " [" + p.scene + "]") + "  " + p.state + (p.path.empty() ? "" : " " + p.path));
    heading("Loading images");
    for (const LoadingImage& i : a.images) item("[" + i.scene + "] " + i.declared + "  " + i.state + (i.path.empty() ? "" : " " + i.path));
    heading("Install actions");
    for (const InstallAction& action : a.actions)
        item(action.kind + " " + (action.source.empty() ? "." : action.source) + (action.target.empty() ? "" : " -> " + action.target) + "  [" + OriginName(action.origin) + "] " + action.reason);
    heading("Runtime files");
    size_t kept = 0;
    for (const RuntimeFile& f : a.runtime) {
        const std::string lower = LowerAscii(f.target);
        if (lower.rfind("data/menu/", 0) == 0 || lower.rfind("data/texhw/", 0) == 0) ++kept;
        else item(f.target + " <- " + f.source + (f.active ? "" : "  preserved_not_active"));
    }
    if (kept) item("data/menu and data/texhw: " + std::to_string(kept) + " files, preserved_not_active");
    heading("Dependencies");
    for (const Dependency& d : a.dependencies) item(d.name + " " + NeedName(d.need) + ": " + d.evidence);
    for (const char* state : {"supported", "mapped", "not_required", "unsupported"}) {
        std::string names;
        for (const Feature& f : a.features)
            if (f.state == state) names += (names.empty() ? "" : ", ") + f.name;
        if (!names.empty()) item(std::string("features ") + state + ": " + names);
    }
    heading("Session overrides");
    for (const SessionOverride& o : a.overrides) item(o.key + " = " + o.value + "  (" + o.source + ")");
    heading("Session limits");
    for (const SessionLimit& l : a.limits) item(l.key + " = " + std::to_string(l.value) + "  (" + l.source + ")");
    heading("Settings");
    for (const ConfigSetting& s : a.settings)
        item(std::string(SettingUseName(s.meaning.use)) + "  " + SettingFileName(s.kind) + " [" + s.entry.section + "] " + s.entry.key + (s.entry.additive ? "+=" : "=") + s.entry.value +
             (s.meaning.note.empty() ? "" : "  (" + s.meaning.note + ")"));
    heading("Warnings");
    for (const Warning& w : a.warnings) item(w.code + ": " + w.detail);
    heading("Refused files");
    for (const std::string& name : manifest.origin.refused) item(name);
    return out;
}

} // namespace jpt

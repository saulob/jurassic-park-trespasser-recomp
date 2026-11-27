#include "level_import_log.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>

namespace jpt {
namespace {

constexpr size_t kGroupLines = 400;
constexpr size_t kTextColumns = 400;

class Lines {
public:
    void Add(const std::string& text) { lines_.push_back(text); }
    void Group(const std::string& title, const std::vector<std::string>& items) {
        if (items.empty()) return;
        Add(title + " (" + std::to_string(items.size()) + "):");
        for (size_t i = 0; i < items.size() && i < kGroupLines; ++i) Add("  " + items[i]);
        if (items.size() > kGroupLines) Add("  ... and " + std::to_string(items.size() - kGroupLines) + " more");
    }
    void Text(const std::string& text) {
        for (size_t at = 0; at < text.size();) {
            const size_t end = std::min(text.find('\n', at), text.size());
            size_t cut = std::min(end, at + kTextColumns);
            while (cut < end && cut > at + 1 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
            Add("| " + text.substr(at, cut - at));
            at = cut == end ? end + 1 : cut;
        }
    }
    std::vector<std::string> Take() { return std::move(lines_); }

private:
    std::vector<std::string> lines_;
};

constexpr char kOther[] = "Other runtime files";
constexpr const char* kGroups[] = {"SCN", "GRF", "PID", "SWP", "SPZ", "WTD", "TPA", "INI", "TXT", "MD", "NFO", "ASA", "BMP", "TGA", "DDS", "SKY", kOther};

const char* GroupOf(const std::string& path) {
    static const std::map<std::string, const char*> kTypes = {
        {".scn", "SCN"}, {".grf", "GRF"}, {".pid", "PID"}, {".swp", "SWP"}, {".spz", "SPZ"}, {".spp", "SPZ"}, {".wtd", "WTD"}, {".tpa", "TPA"}, {".ini", "INI"}, {".txt", "TXT"},
        {".md", "MD"}, {".markdown", "MD"}, {".nfo", "NFO"}, {".asa", "ASA"}, {".asb", "ASA"}, {".bmp", "BMP"}, {".tga", "TGA"}, {".dds", "DDS"}, {".sky", "SKY"}};
    const auto it = kTypes.find(PathType(path));
    return it == kTypes.end() ? kOther : it->second;
}

// A folder of many files is one line.
std::vector<std::string> ByFolder(const std::vector<std::string>& paths) {
    std::map<std::string, std::vector<std::string>> folders;
    for (const std::string& path : paths)
        folders[PathFolder(path)].push_back(path);
    std::vector<std::string> items;
    for (const auto& [folder, files] : folders) {
        if (files.size() <= 6) items.insert(items.end(), files.begin(), files.end());
        else items.push_back((folder.empty() ? std::string(".") : folder) + "/ (" + std::to_string(files.size()) + " files)");
    }
    return items;
}

std::vector<std::string> Named(const std::vector<std::string>& paths, const std::string& group) {
    std::vector<std::string> items;
    std::copy_if(paths.begin(), paths.end(), std::back_inserter(items), [&group](const std::string& path) { return group == GroupOf(path); });
    return items;
}

std::vector<std::string> RefusedLines(const ImportRecord& record, bool rejected) {
    std::map<std::string, std::vector<std::string>> reasons;
    for (const RefusedFile& file : record.refused)
        if (file.rejected == rejected) reasons[file.reason].push_back(file.path);
    std::vector<std::string> items;
    for (const auto& [reason, paths] : reasons)
        for (const std::string& item : ByFolder(paths)) items.push_back(item + " - " + reason);
    return items;
}

// The files the game asks for elsewhere than the package keeps them.
std::vector<std::string> MovedFiles(const PackageAnalysis& a) {
    std::vector<std::string> items;
    for (const RuntimeFile& file : a.runtime) {
        if (file.origin != Origin::kLayout) items.push_back(file.target + " <- " + file.source + " [" + OriginName(file.origin) + "]" + (file.active ? "" : " kept, not used"));
    }
    return items;
}

// Level data nothing points the game at.
std::vector<std::string> Unplaced(const ImportRecord& r) {
    static constexpr const char* kData[] = {".scn", ".grf", ".wtd", ".pid", ".swp", ".spz", ".spp", ".asa", ".asb", ".tpa", ".bmp", ".tga", ".dds"};
    std::set<std::string> placed;
    for (const RuntimeFile& file : r.analysis.runtime) placed.insert(file.source);
    for (const AudioPack& pack : r.analysis.audio) placed.insert(pack.path);
    for (const LoadingImage& image : r.analysis.images) placed.insert(image.path);
    std::vector<std::string> items;
    for (const std::string& path : r.accepted)
        if (!placed.count(path) && std::any_of(std::begin(kData), std::end(kData), [type = PathType(path)](const char* t) { return type == t; })) items.push_back(path);
    return items;
}

std::string RuntimeSummary(const PackageAnalysis& a) {
    std::map<std::string, std::pair<size_t, size_t>> folders; // active, kept
    for (const RuntimeFile& file : a.runtime) {
        std::pair<size_t, size_t>& count = folders[PathFolder(file.target)];
        ++(file.active ? count.first : count.second);
    }
    std::string text;
    for (const auto& [folder, count] : folders) {
        text += (text.empty() ? "" : ", ") + folder + " " + std::to_string(count.first);
        if (count.second) text += " (+" + std::to_string(count.second) + " kept, not used)";
    }
    return text;
}

const char* StateName(LevelState state) {
    switch (state) {
    case LevelState::kReady: return "Ready";
    case LevelState::kIncomplete: return "Incomplete";
    case LevelState::kAuxiliary: return "Auxiliary";
    case LevelState::kDamaged: return "Damaged";
    case LevelState::kNotSupported: return "Not supported";
    case LevelState::kLimited: return "Limited compatibility";
    case LevelState::kUnsupported:
    case LevelState::kUnsupportedFormat: return "Unsupported";
    case LevelState::kUnknown: break;
    }
    return "Unknown";
}

void AddDocuments(const ImportRecord& r, Lines& lines) {
    for (const LogDocument& d : r.documents) {
        const std::string kind = d.readme ? "README" : "TXT";
        lines.Add(kind + " BEGIN " + d.path + " (" + std::to_string(d.bytes) + " bytes" + (d.sha256.empty() ? "" : ", SHA-256 " + d.sha256) + ")");
        if (d.readable) lines.Text(d.text);
        else lines.Add("| not a readable text");
        if (d.truncated) lines.Add("| ... truncated: the first " + std::to_string(kLogDocumentBytes) + " of " + std::to_string(d.bytes) + " bytes");
        lines.Add(kind + " END " + d.path);
    }
    if (r.documents_skipped) lines.Add(std::to_string(r.documents_skipped) + " more text(s) are not logged");
    std::vector<std::string> readmes;
    for (const LogDocument& d : r.documents)
        if (d.readme) readmes.push_back(d.path);
    lines.Group("README", readmes);
}

void AddLevels(const ImportRecord& r, Lines& lines) {
    const PackageAnalysis& a = r.analysis;
    std::string configs, needs;
    for (const ConfigFile& config : a.configs)
        if (config.used) configs += (configs.empty() ? "" : ", ") + config.path;
    for (const Dependency& d : a.dependencies) needs += (needs.empty() ? "" : "; ") + d.name + " " + NeedName(d.need) + " (" + d.evidence + ")";
    for (const LevelScene& scene : r.scenes) {
        const auto info = std::find_if(a.levels.begin(), a.levels.end(), [&scene](const LevelInfo& l) { return l.path == scene.path; });
        lines.Add("level " + scene.path + " - " + StateName(scene.state) + (scene.note.empty() ? "" : ", " + scene.note));
        lines.Add("  scene version " + (scene.version ? std::to_string(scene.version) : std::string("unknown")) +
                  (scene.version > JPT_SCENE_VERSION_OWN && scene.version <= JPT_SCENE_VERSION_MAX ? ", read as the game's own " + std::to_string(JPT_SCENE_VERSION_OWN) : std::string()) +
                  (scene.start ? ", StartLevel" : ""));
        for (const std::string& text : scene.left_out) lines.Add("  " + text);
        for (const std::string& use : scene.uses) lines.Add("  " + use);
        if (info != a.levels.end() && info->configured && !configs.empty()) lines.Add("  config: " + configs);
        if (!needs.empty()) lines.Add("  depends on: " + needs);
        for (const std::string& name : scene.missing) lines.Add("  missing required: " + name);
        lines.Add(std::string("  class: ") + LevelClassName(scene.level_class()));
        lines.Add(std::string("  compatibility: ") + jpt_compat_name(scene.compatibility()) + " (" + jpt_compat_title(scene.compatibility()) + ")");
        for (const LevelIssue& issue : scene.issues)
            lines.Add(std::string(issue.blocks() ? "  " : "  info: ") + issue.code + " " + issue.subject + ": " + issue.detail +
                      (issue.blocks() ? std::string(" [") + jpt_issue_kind_name(issue.kind()) + ", " + jpt_compat_name(issue.compat()) + "]" : std::string()));
    }
}

} // namespace

std::vector<std::string> ImportLog(const ImportRecord& r) {
    Lines lines;
    const PackageAnalysis& a = r.analysis;
    const auto count = [&r](bool rejected) {
        return std::to_string(std::count_if(r.refused.begin(), r.refused.end(), [rejected](const RefusedFile& f) { return f.rejected == rejected; }));
    };
    lines.Add("custom level import source: " + r.type + " \"" + r.name + "\"" + (r.bytes ? ", " + std::to_string(r.bytes) + " bytes" : ""));
    if (!r.path.empty()) lines.Add("source path: " + r.path);
    if (!r.sha256.empty()) lines.Add("source SHA-256: " + r.sha256);
    AddDocuments(r, lines);
    if (!r.package.empty()) lines.Add("package " + r.package + ", layout " + (a.layout.empty() ? "unknown" : a.layout) + (a.root.empty() ? "" : ", mod folder " + a.root));
    lines.Add("files found: " + std::to_string(r.found) + ", accepted: " + std::to_string(r.accepted.size()) + ", ignored: " + count(false) + ", rejected: " + count(true));
    for (const char* group : kGroups) lines.Group(group, Named(r.accepted, group));
    lines.Group("Ignored", RefusedLines(r, false));
    lines.Group("Rejected", RefusedLines(r, true));
    if (!a.runtime.empty()) lines.Add("runtime folders: " + RuntimeSummary(a));
    lines.Group("Runtime paths", MovedFiles(a));
    if (!a.runtime.empty()) lines.Group("Kept, without a runtime path", ByFolder(Unplaced(r)));
    AddLevels(r, lines);
    std::vector<std::string> settings, warnings;
    for (const AudioPack& pack : a.audio) settings.push_back(std::string(AudioKindName(pack.kind)) + (pack.additive ? " += " : " = ") + pack.declared + " (" + pack.state + ")");
    for (const LoadingImage& image : a.images) settings.push_back("loading image of " + image.scene + " = " + image.declared + " (" + image.state + ")");
    for (const SessionOverride& o : a.overrides) settings.push_back(o.key + " = " + o.value + " (" + o.source + ")");
    for (const SessionLimit& l : a.limits) settings.push_back(l.key + " = " + std::to_string(l.value) + " (" + l.source + ")");
    for (const Warning& w : a.warnings) warnings.push_back(w.code + ": " + w.detail);
    std::vector<std::string> configs, needs;
    for (const ConfigFile& config : a.configs) configs.push_back(config.path + (config.used ? "" : " (kept, not used)"));
    for (const Dependency& d : a.dependencies) needs.push_back(d.name + " " + NeedName(d.need) + ": " + d.evidence);
    lines.Group("Configs", configs);
    lines.Group("Dependencies", needs);
    lines.Group("Session", settings);
    lines.Group("Warnings", warnings);
    const bool linked = r.status == ImportStatus::kLinked, ok = r.status == ImportStatus::kInstalled || linked;
    // A report carries the end of the log alone, which a long readme would otherwise fill.
    if (!r.documents.empty()) lines.Add("source: " + r.type + " \"" + r.name + "\"" + (r.sha256.empty() ? "" : ", SHA-256 " + r.sha256));
    size_t counts[3] = {};
    for (const LevelScene& scene : r.scenes)
        if (scene.valid) ++counts[scene.compatibility()];
    lines.Add(std::string("result: ") + (linked ? "linked" : ok ? "installed" : "rejected") +
              (ok ? "" : std::string(", ") + ImportStatusTitle(r.status)) + (r.detail.empty() ? "" : " (" + r.detail + ")") + "; levels ready " +
              std::to_string(counts[JPT_COMPAT_READY]) + ", limited " + std::to_string(counts[JPT_COMPAT_LIMITED]) + ", invalid " + std::to_string(counts[JPT_COMPAT_INVALID]));
    return lines.Take();
}

} // namespace jpt

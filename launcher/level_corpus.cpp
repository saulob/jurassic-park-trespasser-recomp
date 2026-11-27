#include "level_corpus.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <system_error>

#include "archive_reader.h"
#include "json_text.h"
#include "jpt_custom_level.h"
#include "jpt_groff.h"
#include "jpt_hash.h"
#include "level_limits.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr size_t kExamples = 6;
constexpr size_t kTopProblems = 20;

struct Found {
    std::string path;
    fs::path file;
    bool archive = false, other = false;
};

class Walker {
public:
    Walker(const fs::path& root, const CorpusLimits& limits, std::vector<std::string>& notes) : root_(root), limits_(limits), notes_(notes) {}

    std::vector<Found> Run() {
        Visit(root_, "", 0);
        std::sort(found_.begin(), found_.end(), [](const Found& a, const Found& b) { return std::make_pair(LowerAscii(a.path), a.path) < std::make_pair(LowerAscii(b.path), b.path); });
        if (found_.size() > limits_.max_packages) {
            notes_.push_back("more than " + std::to_string(limits_.max_packages) + " packages: the rest is left out");
            found_.resize(limits_.max_packages);
        }
        return std::move(found_);
    }

private:
    static std::vector<fs::directory_entry> Entries(const fs::path& folder) {
        std::vector<fs::directory_entry> entries;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type_ec;
            const fs::file_status status = it->symlink_status(type_ec);
            if (!type_ec && (fs::is_regular_file(status) || fs::is_directory(status))) entries.push_back(*it);
        }
        return entries;
    }

    static bool HoldsScene(const fs::path& folder) {
        for (const fs::directory_entry& entry : Entries(folder))
            if (entry.is_regular_file() && IsScenePath(Utf8(entry.path().filename()))) return true;
        return false;
    }

    // An extracted package: its scenes, its data folder or its mod settings are right here.
    static bool IsPackage(const fs::path& folder) {
        for (const fs::directory_entry& entry : Entries(folder)) {
            const std::string name = LowerAscii(Utf8(entry.path().filename()));
            std::error_code ec;
            if (entry.is_directory(ec) ? name == "data" && HoldsScene(entry.path()) : IsScenePath(name) || name == "fm.ini" || name == "tp_mod.ini") return true;
        }
        return false;
    }

    void Visit(const fs::path& folder, const std::string& path, size_t depth) {
        if (++folders_ > limits_.max_folders || depth > limits_.max_depth) {
            if (std::find(notes_.begin(), notes_.end(), kTooMany) == notes_.end()) notes_.push_back(kTooMany);
            return;
        }
        if (IsPackage(folder)) {
            found_.push_back({path.empty() ? "." : path, folder, false, false});
            return;
        }
        for (const fs::directory_entry& entry : Entries(folder)) {
            const std::string name = Utf8(entry.path().filename()), below = path.empty() ? name : path + "/" + name, type = PathType(name);
            std::error_code ec;
            if (entry.is_directory(ec)) Visit(entry.path(), below, depth + 1);
            else if (type == ".zip" || type == ".7z") found_.push_back({below, entry.path(), true, false});
            else if (type == ".rar") found_.push_back({below, entry.path(), true, !IsArchiveKnown(entry.path())});
        }
    }

    static constexpr char kTooMany[] = "folders too deep or too many were not searched";
    fs::path root_;
    const CorpusLimits& limits_;
    std::vector<std::string>& notes_;
    std::vector<Found> found_;
    size_t folders_ = 0;
};

// By name and size: what two copies of a package share, packed or not.
std::string ContentsOf(const AnalysisResult& result) {
    std::vector<std::string> lines;
    for (const PackageFile& file : result.files)
        if (PathType(file.path) == ".scn" || PathType(file.path) == ".grf" || PathType(file.path) == ".pid" || PathType(file.path) == ".wtd")
            lines.push_back(LowerAscii(PathName(file.path)) + " " + std::to_string(file.size));
    std::sort(lines.begin(), lines.end());
    std::string text;
    for (const std::string& line : lines) text += line + "\n";
    return text;
}

struct Tally {
    bool blocks = false;
    std::set<std::string> levels, packages;
};

void Count(std::map<std::string, Tally>& tallies, const std::string& signature, bool blocks, const std::string& package, const std::string& level) {
    Tally& tally = tallies[signature];
    tally.blocks = tally.blocks || blocks;
    tally.levels.insert(package + ": " + level);
    tally.packages.insert(package);
}

std::vector<CorpusProblem> Ranked(const std::map<std::string, Tally>& tallies) {
    std::vector<CorpusProblem> problems;
    for (const auto& [signature, tally] : tallies) {
        CorpusProblem p{signature, tally.blocks, tally.levels.size(), tally.packages.size(), {}};
        for (const std::string& level : tally.levels)
            if (p.examples.size() < kExamples) p.examples.push_back(level);
        problems.push_back(std::move(p));
    }
    std::sort(problems.begin(), problems.end(), [](const CorpusProblem& a, const CorpusProblem& b) {
        return a.levels != b.levels ? a.levels > b.levels : a.signature < b.signature;
    });
    return problems;
}

bool Analysed(const CorpusPackage& p) { return p.unread.empty() && p.duplicate_of.empty() && !p.result.scenes.empty(); }

bool AnyFile(const CorpusPackage& p, const char* type) {
    return std::any_of(p.result.files.begin(), p.result.files.end(), [type](const PackageFile& f) { return PathType(f.path) == type; });
}

bool HasFolder(const CorpusPackage& p, const char* name) {
    const auto& d = p.result.analysis.directories;
    return std::find(d.begin(), d.end(), name) != d.end();
}

void CountLevel(const CorpusPackage& package, const LevelScene& scene, CorpusReport& report, std::map<std::string, Tally>& categories, std::map<std::string, Tally>& problems) {
    ++report.totals["levels"];
    ++report.totals[LevelClassName(scene.level_class())];
    ++report.totals[std::string("compatibility ") + jpt_compat_name(scene.compatibility())];
    if (scene.playable()) ++report.totals["playable"];
    if (scene.auxiliary) ++report.totals["auxiliary"];
    if (scene.version) ++report.scene_versions[scene.version];
    const auto uses_game = [](const LevelResource& r) { return r.place == kInstalledGame && r.type != "SCN"; };
    if (std::any_of(scene.resources.begin(), scene.resources.end(), uses_game)) ++report.totals["use_game_resources"];
    if (!scene.missing.empty()) ++report.totals["missing_resources"];
    std::set<int> actions;
    for (const auto& [world, facts] : scene.worlds)
        for (const auto& [type, count] : facts.actions)
            if (type < 0 || type > JPT_GROFF_ACTION_OWN_LAST) actions.insert(type);
    for (const int type : actions) ++report.actions[type];
    for (const auto& [world, facts] : scene.worlds) {
        if (facts.count("curved_missing")) ++report.totals["crash_signature_curved_bump"];
        for (const std::string& name : LimitsExceeded(facts, true)) ++report.totals["over_1998_" + name];
        for (const std::string& name : LimitsExceeded(facts, false)) ++report.totals["over_port_" + name];
    }
    if (std::any_of(scene.issues.begin(), scene.issues.end(), [](const LevelIssue& i) { return i.code == "MISSING_REQUIRED_TERRAIN"; })) ++report.totals["crash_signature_no_terrain"];
    for (const LevelIssue& issue : scene.issues) {
        if (!issue.blocks()) continue;
        Count(categories, issue.code, issue.blocks(), package.path, scene.path);
        Count(problems, issue.code + ": " + LowerAscii(issue.subject), issue.blocks(), package.path, scene.path);
    }
}

// The worst class of its levels.
LevelClass PackageClass(const AnalysisResult& r) {
    LevelClass worst = LevelClass::kReady;
    for (const LevelScene& scene : r.scenes) worst = std::max(worst, scene.level_class());
    return worst;
}

void CountPackage(const CorpusPackage& package, CorpusReport& report) {
    ++report.totals["packages"];
    ++report.totals[std::string("package ") + LevelClassName(PackageClass(package.result))];
    const std::pair<const char*, bool> facts[] = {
        {"with_dds", AnyFile(package, ".dds")},       {"with_tga", AnyFile(package, ".tga")},        {"with_tpa", AnyFile(package, ".tpa")},
        {"with_animations", AnyFile(package, ".asa") || AnyFile(package, ".asb")}, {"with_sky", AnyFile(package, ".sky")}, {"with_texhw", HasFolder(package, "texhw")},
        {"with_menu", HasFolder(package, "menu")},    {"with_start_level", std::any_of(package.result.scenes.begin(), package.result.scenes.end(), [](const LevelScene& s) { return s.start; })},
        {"mention_ce", package.result.analysis.need("Trespasser CE") != Need::kNone}, {"mention_atx", package.result.analysis.need("ATX") != Need::kNone}};
    for (const auto& [name, holds] : facts) report.totals[name] += holds;
}

std::string Number(uint64_t value) { return std::to_string(value); }

std::vector<std::string> Inventory(const CorpusPackage& p) {
    static constexpr const char* kTypes[] = {".scn", ".grf", ".pid", ".swp", ".spz", ".wtd", ".asa", ".asb", ".tpa", ".bmp", ".tga", ".dds", ".sky", ".ddf", ".ini", ".txt"};
    std::vector<std::string> counts;
    for (const char* type : kTypes) {
        const size_t n = size_t(std::count_if(p.result.files.begin(), p.result.files.end(), [type](const PackageFile& f) { return PathType(f.path) == type; }));
        if (n) counts.push_back(std::string(type + 1) + " " + Number(n));
    }
    return counts;
}

std::string Joined(const std::vector<std::string>& parts, const char* separator = ", ") {
    std::string text;
    for (const std::string& part : parts) text += (text.empty() ? "" : separator) + part;
    return text;
}

std::string LevelLine(const LevelScene& scene) {
    std::string text = scene.path + "  " + LevelClassName(scene.level_class()) + "  scene " + (scene.version ? Number(uint64_t(scene.version)) : "?");
    if (scene.start) text += "  StartLevel";
    if (scene.auxiliary) text += "  auxiliary";
    if (!scene.led_to_by.empty()) text += "  loaded by " + Joined(scene.led_to_by);
    return text + "  compatibility " + jpt_compat_name(scene.compatibility());
}

void WritePackage(const CorpusPackage& p, std::string& out) {
    const AnalysisResult& r = p.result;
    out += "\n" + p.path + "  [" + r.type + (r.bytes ? ", " + Number(r.bytes) + " bytes" : "") + (r.sha256.empty() ? "" : ", SHA-256 " + r.sha256) + "]\n";
    if (!p.unread.empty()) return void(out += "  not analysed: " + p.unread + "\n");
    if (!p.duplicate_of.empty()) return void(out += "  the same level files as " + p.duplicate_of + "\n");
    if (r.scenes.empty()) return void(out += std::string("  ") + ImportStatusTitle(r.import.status) + (r.import.detail.empty() ? "" : ": " + r.import.detail) + "\n");
    out += "  " + std::string(LevelClassName(PackageClass(r))) + ", import: " + (r.import.ok() ? "can be installed" : ImportStatusTitle(r.import.status)) + "\n";
    out += "  layout " + r.analysis.layout + (r.analysis.root.empty() ? "" : ", mod folder " + r.analysis.root) + (r.analysis.start_level.empty() ? "" : ", start " + r.analysis.start_level) + "\n";
    out += "  files: " + Joined(Inventory(p)) + (r.import.refused.empty() ? "" : "; left out " + Number(r.import.refused.size())) + "\n";
    for (const LevelScene& scene : r.scenes) {
        out += "  " + LevelLine(scene) + "\n";
        for (const LevelResource& res : scene.resources) out += "    " + res.type + " " + res.name + ": " + (!res.place.empty() ? res.place : !res.note.empty() ? res.note : "TRULY_MISSING") + "\n";
        for (const auto& [world, facts] : scene.worlds) {
            std::vector<std::string> counts;
            for (const auto& [name, count] : facts.counts) counts.push_back(name + " " + Number(count));
            out += "    " + world + (facts.borrowed ? " (texture set of the installed game)" : "") + ": " + Joined(counts) + "\n";
            const std::vector<std::string> over = LimitsExceeded(facts, true);
            if (!over.empty()) out += "    beyond the 1998 limits: " + Joined(over) + "\n";
        }
        for (const LevelIssue& issue : scene.issues) out += "    " + issue.code + " " + issue.subject + ": " + issue.detail + "\n";
    }
}

void WriteProblems(const char* title, const std::vector<CorpusProblem>& problems, size_t most, std::string& out) {
    out += std::string("\n") + title + "\n";
    for (size_t i = 0; i < problems.size() && i < most; ++i) {
        const CorpusProblem& p = problems[i];
        out += "  " + Number(p.levels) + " level(s), " + Number(p.packages) + " package(s)  " + (p.blocks ? "blocks  " : "") + p.signature + "  (" + Joined(p.examples) + (p.levels > p.examples.size() ? ", ..." : "") + ")\n";
    }
}

const char* IssuePolicyName(IssuePolicy policy) {
    switch (policy) {
    case IssuePolicy::kInfo: return "info";
    case IssuePolicy::kWarning: return "other";
    case IssuePolicy::kMissing: return "missing";
    case IssuePolicy::kUnsupported: return "unsupported";
    case IssuePolicy::kMalformed: break;
    }
    return "malformed";
}

void WriteIssues(JsonText& json, const LevelScene& scene) {
    json.Open("resources", '[');
    for (const LevelResource& r : scene.resources)
        json.Open(nullptr, '{').Text("type", r.type).Text("name", r.name).Text("place", r.place).Text("state", r.place.empty() ? (r.note.empty() ? "TRULY_MISSING" : "NOT_NEEDED") : r.place == kInstalledGame ? "RESOLVED_FROM_GAME" : "IN_PACKAGE").Close();
    json.Close().Open("issues", '[');
    for (const LevelIssue& i : scene.issues) json.Open(nullptr, '{').Text("code", i.code).Text("subject", i.subject).Text("detail", i.detail).Text("policy", IssuePolicyName(i.policy)).Flag("blocks", i.blocks()).Close();
    json.Close().Open("worlds", '[');
    for (const auto& [world, facts] : scene.worlds) {
        json.Open(nullptr, '{').Text("name", world).Text("terrain", facts.terrain).Flag("borrowed_textures", facts.borrowed).Open("counts", '{');
        for (const auto& [name, count] : facts.counts) json.Number(name.c_str(), long(count));
        json.Close().List("levels", facts.levels).Open("action_types", '[');
        for (const auto& [type, count] : facts.actions) json.Open(nullptr, '{').Number("type", type).Number("count", long(count)).Close();
        json.Close().Close();
    }
    json.Close();
}

} // namespace

CorpusReport ScanCorpus(const fs::path& root, const fs::path& data_dir, const CorpusLimits& limits, const std::function<void(const std::string&)>& progress) {
    CorpusReport report;
    std::map<std::string, std::string> by_hash, by_contents;
    std::map<std::string, Tally> categories, problems;
    for (const Found& found : Walker(root, limits, report.notes).Run()) {
        CorpusPackage package;
        package.path = found.path;
        if (progress) progress(found.path);
        report.totals[found.archive ? "archives" : "folders"]++;
        const auto copy = limits.extracted.find(found.path);
        package.extracted = found.other && copy != limits.extracted.end();
        if (found.other && !package.extracted) package.unread = "an archive of a kind the import does not read here; extracted, it is analysed as a folder";
        else package.result = AnalyzeLevelSource(package.extracted ? copy->second : found.file, data_dir, limits.archive);
        if (found.other) {
            std::error_code ec;
            package.result.type = LowerAscii(PathType(found.path)).substr(1);
            package.result.bytes = fs::file_size(found.file, ec);
            package.result.sha256 = ArchiveSha256(found.file);
        }
        const std::string contents = ContentsOf(package.result);
        if (!package.result.sha256.empty()) {
            const auto [it, first] = by_hash.emplace(package.result.sha256, package.path);
            if (!first) package.duplicate_of = it->second;
        }
        if (package.duplicate_of.empty() && !contents.empty()) {
            const auto [it, first] = by_contents.emplace(contents, package.path);
            if (!first) package.duplicate_of = it->second;
        }
        if (!package.duplicate_of.empty()) ++report.totals["duplicates"];
        if (Analysed(package)) {
            CountPackage(package, report);
            for (const LevelScene& scene : package.result.scenes) CountLevel(package, scene, report, categories, problems);
        }
        report.packages.push_back(std::move(package));
    }
    report.categories = Ranked(categories);
    report.problems = Ranked(problems);
    return report;
}

std::string CorpusText(const CorpusReport& report) {
    std::string out = "Totals\n";
    for (const auto& [name, count] : report.totals) out += "  " + name + ": " + Number(count) + "\n";
    out += "  scene versions:";
    for (const auto& [version, count] : report.scene_versions) out += " " + Number(uint64_t(version)) + " x" + Number(count);
    out += "\n  actions of ATX and Trespasser CE (levels with them):";
    for (const auto& [type, count] : report.actions) out += " " + std::to_string(type) + " x" + Number(count);
    out += "\n";
    for (const std::string& note : report.notes) out += "  " + note + "\n";
    WriteProblems("Problems by category", report.categories, report.categories.size(), out);
    WriteProblems("The most frequent problems", report.problems, kTopProblems, out);
    out += "\nPackages\n";
    for (const CorpusPackage& package : report.packages) WritePackage(package, out);
    return out;
}

std::string CorpusJson(const CorpusReport& report) {
    JsonText json;
    json.Open(nullptr, '{').Open("totals", '{');
    for (const auto& [name, count] : report.totals) json.Number(name.c_str(), long(count));
    json.Close().Open("scene_versions", '{');
    for (const auto& [version, count] : report.scene_versions) json.Number(std::to_string(version).c_str(), long(count));
    json.Close().Open("actions", '{');
    for (const auto& [type, count] : report.actions) json.Number(std::to_string(type).c_str(), long(count));
    json.Close().List("notes", report.notes);
    for (const auto& [key, problems] : {std::make_pair("categories", &report.categories), std::make_pair("problems", &report.problems)}) {
        json.Open(key, '[');
        for (const CorpusProblem& p : *problems)
            json.Open(nullptr, '{').Text("signature", p.signature).Flag("blocks", p.blocks).Number("levels", long(p.levels)).Number("packages", long(p.packages)).List("examples", p.examples).Close();
        json.Close();
    }
    json.Open("packages", '[');
    for (const CorpusPackage& p : report.packages) {
        const AnalysisResult& r = p.result;
        json.Open(nullptr, '{').Text("path", p.path).Text("type", r.type).Text("bytes", Number(r.bytes)).Text("sha256", r.sha256).Text("duplicate_of", p.duplicate_of).Text("unread", p.unread);
        json.Text("import", r.scenes.empty() ? ImportStatusTitle(r.import.status) : r.import.ok() ? "can be installed" : ImportStatusTitle(r.import.status));
        json.Text("class", r.scenes.empty() ? "" : LevelClassName(PackageClass(r))).Text("layout", r.analysis.layout).Text("mod_folder", r.analysis.root).Text("start_level", r.analysis.start_level).List("files", Inventory(p)).Open("levels", '[');
        for (const LevelScene& scene : r.scenes) {
            json.Open(nullptr, '{').Text("scn", scene.path).Text("class", LevelClassName(scene.level_class())).Text("compatibility", jpt_compat_name(scene.compatibility())).Number("scene_version", scene.version);
            json.Flag("start_level", scene.start).Flag("auxiliary", scene.auxiliary).List("loaded_by", scene.led_to_by).List("features", RequiredFeatures(r.analysis, scene));
            WriteIssues(json, scene);
            json.Close();
        }
        json.Close().Close();
    }
    json.Close().Close();
    return json.str() + "\n";
}

} // namespace jpt

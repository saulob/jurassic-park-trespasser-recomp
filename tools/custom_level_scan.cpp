// The compatibility report of a folder of custom level packages: a summary here, the whole in a Markdown file. Nothing is installed.
//   custom_level_scan ROOT [--data DIR] [--md FILE] [--json FILE] [--state FILE] [--baseline FILE] [--extracted ARCHIVE=DIR]...
//   custom_level_scan --extract ARCHIVE DIR
//   custom_level_scan --analyze PACKAGE [DATA]
//   custom_level_scan --guide WORLD (the level guide the launcher writes for a world file)
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "jpt_groff.h"
#include "jpt_level_features.h"
#include "jpt_level_world.h"
#include "level_guide.h"
#include "archive_reader.h"
#include "jpt_launcher.h"
#include "level_corpus.h"
#include "updater/update_paths.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace jpt;

namespace {

struct Row {
    std::string package, level, status, required, blockers;
    int version = 0;
    std::vector<LevelIssue> missing, refused;
    std::vector<std::string> features;
    std::string game;
};

struct Before {
    std::string status, blockers;
};

std::string Join(const std::vector<std::string>& parts, const char* separator = ", ") {
    std::string text;
    for (const std::string& part : parts) text += (text.empty() ? "" : separator) + part;
    return text;
}

std::string Cell(std::string text) {
    for (char& c : text)
        if (c == '|' || c == '\n' || c == '\t') c = '/';
    return text.empty() ? "-" : text;
}

std::string Signature(const LevelIssue& issue) {
    if (issue.code == "UNSUPPORTED_ACTION" || issue.code == "UNSUPPORTED_FEATURE") return issue.subject;
    return issue.code;
}

Row RowOf(const CorpusPackage& package, const LevelScene& scene) {
    Row row{package.path, scene.path, LevelClassName(scene.level_class()), "", "", scene.version};
    std::vector<std::string> blockers;
    for (const LevelIssue& issue : scene.issues) {
        if (!issue.blocks()) continue;
        const std::string name = Signature(issue);
        if (std::find(blockers.begin(), blockers.end(), name) == blockers.end()) blockers.push_back(name);
        if (issue.policy == IssuePolicy::kMissing) row.missing.push_back(issue);
        else row.refused.push_back(issue);
    }
    std::vector<std::string> game;
    for (const LevelResource& r : scene.resources)
        if (r.place == kInstalledGame) game.push_back(r.name);
    row.blockers = Join(blockers);
    row.game = Join(game);
    row.features = RequiredFeatures(package.result.analysis, scene);
    row.required = Join(row.features);
    return row;
}

const char* SourceStatus(const CorpusPackage& p) {
    if (!p.unread.empty()) return "UNSUPPORTED_ARCHIVE";
    if (!p.duplicate_of.empty()) return "DUPLICATE";
    if (p.result.scenes.empty()) return "INVALID";
    return p.extracted ? "EXTRACTED_EQUIVALENT_ANALYZED" : "ANALYZED";
}

bool Covered(const CorpusPackage& p) { return p.unread.empty() && (!p.duplicate_of.empty() || !p.result.scenes.empty()); }

std::map<std::string, Before> ReadBaseline(const std::string& file) {
    std::map<std::string, Before> rows;
    std::ifstream in(file, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> f;
        std::stringstream fields(line);
        for (std::string field; std::getline(fields, field, '\t');) f.push_back(field);
        if (f.size() >= 3) rows[f[0] + "\t" + f[1]] = {f[2], f.size() > 3 ? f[3] : ""};
    }
    return rows;
}

struct Blocker {
    std::set<std::string> levels, missing_too;
};

struct Report {
    CorpusReport corpus;
    std::vector<Row> rows;
    std::map<std::string, size_t> classes;
    std::map<std::string, Blocker> blockers;
    std::map<int, std::set<std::string>> actions;
    std::map<std::string, std::set<std::string>> users; // of a feature
    std::map<std::string, Before> before;
    size_t covered = 0, not_analyzed = 0;

    const Before* Was(const Row& row) const {
        const auto it = before.find(row.package + "\t" + row.level);
        return it == before.end() ? nullptr : &it->second;
    }
    std::string BeforeOf(const Row& row) const {
        if (const Before* was = Was(row)) return was->status;
        return before.empty() ? "" : "SCAN_UNSUPPORTED_ARCHIVE";
    }
    double coverage() const { return corpus.packages.empty() ? 0 : 100.0 * double(covered) / double(corpus.packages.size()); }
};

Report Build(CorpusReport corpus, std::map<std::string, Before> before) {
    Report r;
    r.before = std::move(before);
    for (const CorpusPackage& package : corpus.packages) {
        Covered(package) ? ++r.covered : ++r.not_analyzed;
        if (!package.duplicate_of.empty()) continue;
        for (const LevelScene& scene : package.result.scenes) {
            Row row = RowOf(package, scene);
            const std::string id = row.package + ": " + row.level;
            ++r.classes[row.status];
            for (const LevelIssue& issue : row.refused) r.blockers[Signature(issue)].levels.insert(id);
            for (const LevelIssue& issue : row.missing) r.blockers[issue.code].levels.insert(id);
            for (const std::string& name : row.features) r.users[name].insert(id);
            for (const auto& [world, facts] : scene.worlds)
                for (const auto& [type, count] : facts.actions)
                    if (type < 0 || type > JPT_GROFF_ACTION_OWN_LAST) r.actions[type].insert(id);
            r.rows.push_back(std::move(row));
        }
    }
    r.corpus = std::move(corpus);
    return r;
}

std::vector<std::pair<std::string, Blocker>> Ranked(const Report& r) {
    std::vector<std::pair<std::string, Blocker>> list(r.blockers.begin(), r.blockers.end());
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.levels.size() > b.second.levels.size(); });
    return list;
}

// What was in the way before and is not any more.
std::map<std::string, std::vector<const Row*>> Resolved(const Report& r) {
    std::map<std::string, std::vector<const Row*>> gone;
    for (const Row& row : r.rows) {
        const Before* was = r.Was(row);
        if (!was) continue;
        std::stringstream in(was->blockers);
        for (std::string name; std::getline(in, name, ',');) {
            name.erase(0, name.find_first_not_of(' '));
            if (!name.empty() && (", " + row.blockers + ",").find(", " + name + ",") == std::string::npos) gone[name].push_back(&row);
        }
    }
    return gone;
}

size_t CountOf(const Report& r, const char* name) {
    const auto it = r.classes.find(name);
    return it == r.classes.end() ? 0 : it->second;
}

const char* Work(const Row& row) {
    if (row.status == "READY") return "-";
    if (!row.refused.empty()) return row.missing.empty() ? "large" : "large, and its files do not exist";
    return "none: its files do not exist";
}

std::string Cut(const std::string& text, size_t most) { return text.size() <= most ? text : text.substr(0, most - 3) + "..."; }

void Print(const Report& r) {
    std::printf("Coverage\n  Sources               %zu\n  Packages              %zu\n  Levels                %zu\n", r.corpus.packages.size(), r.covered, r.rows.size());
    for (const char* name : {"READY", "REJECTED_MISSING", "REJECTED_UNSUPPORTED", "REJECTED_MALFORMED", "REJECTED_OTHER"}) std::printf("  %-21s %zu\n", name, CountOf(r, name));
    std::printf("  Not analyzed          %zu\n  Coverage              %.1f%%\n", r.not_analyzed, r.coverage());
    const auto gone = Resolved(r);
    std::printf("\nTop blockers\n  %-34s %8s %15s %10s\n", "Blocker", "Affected", "Resolved in run", "Remaining");
    std::set<std::string> shown;
    for (const auto& [name, blocker] : Ranked(r)) {
        const auto it = gone.find(name);
        std::printf("  %-34s %8zu %15zu %10zu\n", Cut(name, 34).c_str(), blocker.levels.size() + (it == gone.end() ? 0 : it->second.size()), it == gone.end() ? size_t(0) : it->second.size(), blocker.levels.size());
        shown.insert(name);
    }
    for (const auto& [name, rows] : gone)
        if (!shown.count(name)) std::printf("  %-34s %8zu %15zu %10d\n", Cut(name, 34).c_str(), rows.size(), rows.size(), 0);
    std::printf("\nChanges in this run\n");
    for (const Row& row : r.rows) {
        const std::string was = r.BeforeOf(row);
        if (!was.empty() && was != row.status) std::printf("  %-34s %-26s -> %s\n", Cut(row.level, 34).c_str(), was.c_str(), row.status.c_str());
    }
    std::printf("\nRemaining rejected levels\n");
    for (const Row& row : r.rows)
        if (row.status != "READY") std::printf("  %-32s %-30s %s\n", Cut(row.package, 32).c_str(), Cut(row.level.substr(row.level.rfind('/') + 1), 30).c_str(), Cut(row.blockers, 90).c_str());
}

void Features(const Report& r, const char* family, std::string& md) {
    md += "\n| Feature | Status | Evidence | Notes |\n|---|---|---|---|\n";
    const jpt_level_feature_t* f;
    for (int i = 0; (f = jpt_level_feature_at(i)) != nullptr; ++i) {
        if (std::string(f->name).rfind(family, 0) != 0 || std::string(f->name).rfind("ATX_ACTION_", 0) == 0) continue;
        const auto it = r.users.find(f->name);
        md += std::string("| ") + f->title + " (`" + f->name + "`) | " + jpt_feature_status_name(f->status) + " | " + Cell(f->evidence) + " | " + Cell(std::string(f->note) + (it == r.users.end() ? " (no level of the corpus)" : " (" + std::to_string(it->second.size()) + " level(s))")) + " |\n";
    }
}

std::string Markdown(const Report& r) {
    std::string md = "# Custom Level Compatibility Report\n\nGenerated by `tools/custom-level-compatibility.ps1`. Not part of the repository.\n\n## Coverage\n\n";
    char text[128];
    std::snprintf(text, sizeof text, "%zu sources, %zu analysed: %.1f%%.\n", r.corpus.packages.size(), r.covered, r.coverage());
    md += text;
    md += "\n| Source | Type | Bytes | SHA-256 | Status | Package class | Levels | Files |\n|---|---|---|---|---|---|---|---|\n";
    for (const CorpusPackage& p : r.corpus.packages) {
        std::map<std::string, size_t> types;
        for (const PackageFile& file : p.result.files) ++types[PathType(file.path)];
        std::vector<std::string> files;
        for (const auto& [type, count] : types)
            if (!type.empty()) files.push_back(type.substr(1) + " " + std::to_string(count));
        size_t ready = 0;
        for (const LevelScene& scene : p.result.scenes) ready += scene.level_class() == LevelClass::kReady;
        md += "| " + Cell(p.path) + " | " + Cell(p.result.type) + " | " + std::to_string(p.result.bytes) + " | `" + Cell(p.result.sha256) + "` | " + SourceStatus(p) + " | " +
              (p.result.scenes.empty() ? "-" : ready == p.result.scenes.size() ? "READY" : "rejected") + " | " + std::to_string(p.result.scenes.size()) + " | " + Cell(Join(files)) + " |\n";
    }
    md += "\n## Summary\n\n| | Levels |\n|---|---|\n";
    for (const char* name : {"READY", "REJECTED_MISSING", "REJECTED_UNSUPPORTED", "REJECTED_MALFORMED", "REJECTED_OTHER"}) md += std::string("| ") + name + " | " + std::to_string(CountOf(r, name)) + " |\n";
    md += "| Not analyzed (sources) | " + std::to_string(r.not_analyzed) + " |\n\n`REJECTED_OTHER`: the level itself is complete, another level of its package is not.\n\n## Supported CE Features\n";
    Features(r, "CE_", md);
    md += "\n## Supported ATX Features\n";
    Features(r, "ATX_", md);
    md += "\n## ActionTypes\n\n0 to 35 are the 1998 game's own.\n\n| ActionType | Name | Implemented | Levels using it | Notes |\n|---|---|---|---|---|\n";
    std::set<int> types;
    for (int type = JPT_GROFF_ACTION_OWN_LAST + 1; type < 200; ++type)
        if (jpt_level_action_feature(type)) types.insert(type);
    for (const auto& [type, levels] : r.actions) types.insert(type);
    for (const int type : types) {
        const jpt_level_feature_t* f = jpt_level_action_feature(type);
        const auto it = r.actions.find(type);
        md += "| " + std::to_string(type) + " | " + (f ? f->title : "none") + " | " + (f ? jpt_feature_status_name(f->status) : "nothing to implement") + " | " + Cell(it == r.actions.end() ? "" : Join({it->second.begin(), it->second.end()})) + " | " +
              Cell(f ? f->note : "neither ATX nor Trespasser CE gives it an effect") + " |\n";
    }
    md += "\n## Level Matrix\n\n| Package | Level | SCN | Status | Blockers | Required Features | Retail resources | texhw | Complexity | Implementable now |\n|---|---|---|---|---|---|---|---|---|---|\n";
    for (const Row& row : r.rows)
        md += "| " + Cell(row.package) + " | " + Cell(row.level) + " | " + std::to_string(row.version) + " | " + row.status + " | " + Cell(row.blockers) + " | " + Cell(row.required) + " | " + Cell(row.game) + " | " + (row.required.find("TEXHW") != std::string::npos ? "yes" : "no") + " | " +
              Work(row) + " | " + (row.status == "READY" ? "done" : "no") + " |\n";
    md += "\n## Missing Files\n\nLooked for in the package (the level's folder, its other folders, texhw and Map) and in the installed game.\n\n| Package | Level | File | Reason |\n|---|---|---|---|\n";
    for (const Row& row : r.rows)
        for (const LevelIssue& issue : row.missing) md += "| " + Cell(row.package) + " | " + Cell(row.level) + " | " + Cell(issue.code + " " + issue.subject) + " | " + Cell(issue.detail) + " |\n";
    md += "\n## Unsupported Features\n\n| Feature | Affected levels | Why unsupported |\n|---|---|---|\n";
    std::map<std::string, std::string> why;
    for (const Row& row : r.rows)
        for (const LevelIssue& issue : row.refused) why.emplace(Signature(issue), issue.detail);
    for (const auto& [name, detail] : why) md += "| " + Cell(name) + " | " + std::to_string(r.blockers.at(name).levels.size()) + " | " + Cell(detail) + " |\n";
    md += "\n## Improvements In This Run\n\n| Feature | Affected levels | Before | After |\n|---|---|---|---|\n";
    for (const auto& [name, rows] : Resolved(r))
        for (const Row* row : rows) md += "| " + Cell(name) + " | " + Cell(row->package + ": " + row->level) + " | " + r.BeforeOf(*row) + " | " + row->status + " |\n";
    for (const Row& row : r.rows)
        if (!r.before.empty() && !r.Was(row)) md += "| archive read | " + Cell(row.package + ": " + row.level) + " | SCAN_UNSUPPORTED_ARCHIVE | " + row.status + " |\n";
    md += "\n## Remaining Work\n\n| Blocker | Levels | Levels that nothing else blocks |\n|---|---|---|\n";
    for (const auto& [name, blocker] : Ranked(r)) {
        std::vector<std::string> alone;
        for (const Row& row : r.rows)
            if (row.blockers == name) alone.push_back(row.level.substr(row.level.rfind('/') + 1));
        md += "| " + Cell(name) + " | " + std::to_string(blocker.levels.size()) + " | " + Cell(Join(alone)) + " |\n";
    }
    md += "\n## Session Isolation\n\n"
          "- **Retail session**: Play in the launcher. No package is active; every lookup, setting, limit, capability and override of this layer answers nothing, and the hooks of the custom actions are not installed.\n"
          "- **Custom session**: started from an installed package and custom until the process ends. Only the capabilities its content requires are on.\n"
          "- **Custom to retail transition**: a level of the original game that the mod leads to is played in the custom session: its files come from the game, and the package's roots, sounds, texhw, settings, limits and capabilities stay.\n"
          "- **Cleanup**: ending the session clears the package, its maps, limits, capabilities and what the hosts kept of them; another package starts from none.\n"
          "- **Save isolation**: saves go to `saves/custom_levels/<package>` and carry the package and its content, in levels of the original game too; a retail session refuses them.\n";
    return md;
}

bool WriteText(const std::string& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    std::printf("\n%s %s\n", out ? "Wrote" : "COULD NOT WRITE", file.c_str());
    return bool(out);
}

// The files of an archive, as the import reads them: none outside the folder.
int Extract(const fs::path& archive, const fs::path& folder) {
    ArchiveLimits limits;
    limits.max_total_bytes = 8ull << 30;
    limits.max_file_bytes = 2ull << 30;
    limits.max_archive_bytes = 4ull << 30;
    limits.max_entries = 65536;
    ArchiveError error = ArchiveError::kNone;
    const auto reader = OpenArchive(archive, limits, error);
    if (!reader) return std::fprintf(stderr, "cannot be read (%d)\n", int(error)), 1;
    for (size_t i = 0; i < reader->entries().size(); ++i) {
        const ArchiveEntry& entry = reader->entries()[i];
        std::string relative;
        if (entry.directory) continue;
        if (entry.link || entry.encrypted || !trg::update::NormalizeRelativePath(entry.name, relative)) return std::fprintf(stderr, "refused: %s\n", entry.name.c_str()), 1;
        const fs::path file = folder / PathFromUtf8(relative);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        error = reader->Extract(i, [&out](const uint8_t* data, size_t size) { return bool(out.write(reinterpret_cast<const char*>(data), std::streamsize(size))); });
        if (error != ArchiveError::kNone || !out) return std::fprintf(stderr, "damaged: %s (%d)\n", entry.name.c_str(), int(error)), 1;
    }
    return 0;
}

} // namespace

int Guide(const char* world) {
    size_t size = 0;
    char problem[256];
    uint8_t* bytes = jpt_level_world_load(world, &size);
    jpt_groff_t* g = bytes ? jpt_groff_open(fs::path(world).filename().string().c_str(), bytes, size, problem, sizeof problem) : nullptr;
    if (!g) {
        std::fprintf(stderr, "%s cannot be read%s%s\n", world, bytes ? ": " : "", bytes ? problem : "");
        std::free(bytes);
        return 1;
    }
    const LevelGuide guide = BuildLevelGuide(g);
    std::fputs(LevelGuideText(guide, fs::path(world).filename().string(), false).c_str(), stdout);
    std::fprintf(stderr, "%s\n", LevelGuideSummary(guide).c_str());
    jpt_groff_close(g);
    std::free(bytes);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--guide") return Guide(argv[2]);
    if (argc == 4 && std::string(argv[1]) == "--extract") return Extract(argv[2], argv[3]);
    if (argc >= 3 && std::string(argv[1]) == "--analyze") return jpt_analyze_custom_level(argv[2], argc > 3 ? argv[3] : "", 0);
    std::string root, data, md, json, state, baseline;
    CorpusLimits limits;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string* value = arg == "--data" ? &data : arg == "--md" ? &md : arg == "--json" ? &json : arg == "--state" ? &state : arg == "--baseline" ? &baseline : nullptr;
        if (value && i + 1 < argc) *value = argv[++i];
        else if (arg == "--extracted" && i + 1 < argc) {
            const std::string pair = argv[++i];
            const size_t equals = pair.find('=');
            if (equals != std::string::npos) limits.extracted[pair.substr(0, equals)] = pair.substr(equals + 1);
        } else if (root.empty() && arg.rfind("--", 0) != 0) root = arg;
        else return std::fprintf(stderr, "unknown argument %s\n", arg.c_str()), 2;
    }
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return std::fprintf(stderr, "usage: custom_level_scan ROOT [--data DIR] [--md FILE] [--json FILE] [--state FILE] [--baseline FILE] [--extracted ARCHIVE=DIR]...\n"), 2;
    const Report report = Build(ScanCorpus(root, data, limits, [](const std::string& path) { std::fprintf(stderr, "%s\n", path.c_str()); }), baseline.empty() ? std::map<std::string, Before>() : ReadBaseline(baseline));
    Print(report);
    bool ok = true;
    if (!state.empty()) {
        std::string lines;
        for (const Row& row : report.rows) lines += row.package + "\t" + row.level + "\t" + row.status + "\t" + row.blockers + "\n";
        ok = WriteText(state, lines) && ok;
    }
    if (!json.empty()) ok = WriteText(json, CorpusJson(report.corpus)) && ok;
    if (!md.empty()) ok = WriteText(md, Markdown(report)) && ok;
    return ok ? 0 : 1;
}

// The custom level packages of user_data/custom_levels: import, link, listing, README and removal. A package is data only: nothing
// in it is ever run, and the installed game versions are never written. A linked package's files stay in its own folder, which is
// only read; its records and saves are kept in user_data like an imported one's.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "archive_reader.h"
#include "jpt_custom_level.h"
#include "level_analysis.h"
#include "level_check.h"
#include "level_link.h"

namespace jpt {

enum class ImportStatus {
    kInstalled,
    kAlreadyInstalled,
    kCancelled,
    kUnreadable,
    kUnsupported,
    kDamaged,
    kUnsafe,
    kTooLarge,
    kNoLevel,
    kMissingFiles,
    kNotSupported,
    kUnsupportedFormat,
    kUnsafeToLoad,
    kSeveralPackages,
    kNoSpace,
    kWriteFailed,
    kLinked,
    kAlreadyLinked,
    kFolderUnavailable,
    kPathNotUsable, // the game cannot open the folder's files by their paths
};

const char* ImportStatusTitle(ImportStatus status);

// What the page says when an import or a start fails: the reasons stay in the log.
struct LevelNotice {
    std::string title, reason;
    bool failed = false;
};
LevelNotice ImportNotice(ImportStatus status);
LevelNotice LinkNotice(ImportStatus status);

// A file of a source that is not installed. Rejected ones could run; the others are only of no use to the game.
struct RefusedFile {
    std::string path, reason;
    bool rejected = false;
};

struct ImportResult {
    ImportStatus status = ImportStatus::kUnreadable;
    std::string detail;
    std::string package;
    size_t files = 0;
    size_t skipped = 0; // programs and other files that are not level data
    std::vector<RefusedFile> refused;
    size_t levels = 0;  // READY
    size_t limited = 0; // LIMITED COMPATIBILITY: played only with Play Anyway
    std::vector<std::string> log;
    bool ok() const { return status == ImportStatus::kInstalled; }
};

struct ImportHooks {
    std::function<void(const char*)> phase; // one of kImportPhases, in order
    std::function<bool(uint64_t done, uint64_t total)> progress; // false cancels
};

inline constexpr const char* kImportPhases[] = {"Reading the package...", "Copying the level files...", "Checking the level files..."};

// The whole import as one fraction of kTotal that never goes back: a phase without counts keeps what the one before
// reached, copying moves with its bytes, and only an installed package reaches kTotal.
class ImportProgress {
public:
    static constexpr long long kTotal = 1000;
    void Phase(const std::string& label);
    void Bytes(uint64_t done, uint64_t total);
    void Complete() { done_ = kTotal; }
    long long done() const { return done_; }

private:
    void Raise(long long value);
    long long done_ = 0;
};

// An import as the page runs it on its worker: publish gets the phase and the progress after every step.
struct ImportRequest {
    std::filesystem::path source, user_dir, data_dir;
    bool folder = false;
};
ImportResult RunImport(const ImportRequest& request, const std::function<void(const std::string& phase, long long done, long long total)>& publish,
                       const std::function<bool()>& cancelled);

std::filesystem::path CustomLevelsDir(const std::filesystem::path& user_dir);

// Everything is unpacked and checked in a staging folder beside the packages; the package appears whole or not at all.
// data_dir: the selected game version's data folder. A package with at least one READY or LIMITED COMPATIBILITY level is installed;
// every level keeps its own state, and INVALID / UNSAFE ones are never played.
ImportResult ImportLevelArchive(const std::filesystem::path& archive, const std::filesystem::path& user_dir, const ImportHooks& hooks,
                                const ArchiveLimits& limits = {}, const std::filesystem::path& data_dir = {});
ImportResult ImportLevelFolder(const std::filesystem::path& folder, const std::filesystem::path& user_dir, const ImportHooks& hooks,
                               const ArchiveLimits& limits = {}, const std::filesystem::path& data_dir = {});
// What an interrupted import or removal left behind. Never called while one runs.
void CleanLevelStaging(const std::filesystem::path& user_dir);

// Link External Folder: every package below the folder is checked as the import checks one and registered where it is, never
// copied, moved or written. Each fan mission of a tp_mod.ini tree is a package of its own with the tree's shared folders.
struct LinkResult {
    ImportStatus status = ImportStatus::kNoLevel; // kLinked when one package or more was linked
    std::string detail;
    std::vector<std::string> linked, already; // package ids
    std::vector<std::string> log;
    size_t levels = 0, limited = 0; // READY and LIMITED COMPATIBILITY, of the packages linked now
    bool ok() const { return status == ImportStatus::kLinked; }
};
inline constexpr const char* kLinkPhases[] = {"Reading the folder...", "Checking the level files..."};
LinkResult LinkLevelFolder(const std::filesystem::path& folder, const std::filesystem::path& user_dir, const ImportHooks& hooks,
                           const ArchiveLimits& limits = {}, const std::filesystem::path& data_dir = {});
LinkResult RunLink(const ImportRequest& request, const std::function<void(const std::string& phase, long long done, long long total)>& publish,
                   const std::function<bool()>& cancelled);

// Points a linked package at the folder it was moved to, when that folder has the same scenes; its saves stay with it.
bool RelocateLevelPackage(const std::filesystem::path& user_dir, const std::string& id, const std::filesystem::path& folder, std::string& error);
// Forgets a linked package: its records go, its folder and its saves stay.
bool UnlinkLevelPackage(const std::filesystem::path& user_dir, const std::string& id, std::string& error);
// Drops what is kept of a package's analysis, so that the next listing reads its files again.
bool RescanLevelPackage(const std::filesystem::path& user_dir, const std::string& id);

// Whether the file is level data, a text or a picture; anything else in a package is left out.
bool IsLevelPackageFile(const std::string& relative_path);

enum class LevelState {
    kReady,
    kIncomplete,        // a file the game cannot load the level without is nowhere
    kAuxiliary,         // another state of a level, or a part of one
    kUnsupported,       // not a scene of this game
    kUnsupportedFormat, // a scene of a format the port cannot read
    kDamaged,           // a file of it cannot be read
    kNotSupported,      // it needs what the port cannot load safely
    kUnknown,           // its format cannot be told
    kLimited,           // it uses what the port leaves out: Play Anyway
};

struct LevelScene {
    std::string path; // relative to the package
    std::string file;
    bool valid = false; // a Trespasser scene
    bool start = false; // the fan mission's first level
    bool auxiliary = false; // not a level of its own
    int version = 0;
    std::vector<std::string> missing;
    LevelState state = LevelState::kUnsupported;
    std::vector<std::string> uses; // for the log: its files and where they come from
    std::string note;              // for the log
    std::vector<std::string> left_out; // for the log
    std::vector<LevelResource> resources;
    std::vector<LevelIssue> issues;
    std::vector<std::string> led_to_by; // the other scenes of the package that load it
    std::vector<std::pair<std::string, WorldFacts>> worlds; // the package's own, read
    bool scripted = false;                    // its Trespasser CE scripts act in it: it has a VM
    std::vector<std::string> script_files;    // that VM loads, in order
    std::vector<std::string> script_required; // what they ask of the port that it does not have yet
    bool playable() const { return state == LevelState::kReady; }
    bool limited() const { return state == LevelState::kLimited; }
    bool launchable() const { return playable() || limited(); }
    LevelClass level_class() const { return ClassOf(issues); }
    jpt_compat_t compatibility() const;
    bool rejected() const { return compatibility() != JPT_COMPAT_READY; }
};

// What a row of the Custom Levels page offers. The compatibility report is Bug Report with the level's reasons in the log.
struct RowActions {
    bool play = false, play_anyway = false, readme = false, report = false;
};
RowActions ActionsOf(const LevelScene* scene, bool has_readme);

// A reason of a level's state, for the page, the warning and the reports.
struct LevelReason {
    jpt_compat_t compat = JPT_COMPAT_READY;
    jpt_issue_kind_t kind = JPT_ISSUE_NOTE;
    std::string code, subject, detail;
    std::string key() const { return code + " " + subject; }
};
std::vector<LevelReason> ReasonsOf(const LevelScene& scene);
const char* IssueKindLabel(jpt_issue_kind_t kind); // "Missing feature", "Damaged file", ...

// The package as it would be installed, without writing anything. The text is empty when the source cannot be imported.
struct AnalysisResult {
    ImportResult import; // kInstalled: it would be
    std::string report, json;
    std::string type, sha256; // of the source: "zip", "7z", "rar", "folder"; of an archive
    uint64_t bytes = 0;
    PackageAnalysis analysis;
    std::vector<LevelScene> scenes;
    std::vector<PackageFile> files;
};
// Everything a level needs beyond the 1998 game, by name in jpt_level_features.
std::vector<std::string> RequiredFeatures(const PackageAnalysis& analysis, const LevelScene& scene);
std::string ArchiveSha256(const std::filesystem::path& archive);
AnalysisResult AnalyzeLevelSource(const std::filesystem::path& source, const std::filesystem::path& data_dir = {}, const ArchiveLimits& limits = {});

const char* LevelStatusText(const LevelScene& scene); // "Ready", "Limited Compatibility", "Invalid / Unsafe"
// The levels a ready level leads to that its package does not include: the game stays in it when one is asked for.
std::vector<std::string> LevelNotes(const LevelScene& scene);

enum class LevelSort { kNewest, kOldest, kNameUp, kNameDown };
const char* LevelSortKey(LevelSort sort); // as the settings file spells it
LevelSort LevelSortOf(const std::string& key);

struct LevelPackage {
    std::string id;
    std::filesystem::path dir;     // its files: user_data/custom_levels/<id>, or a linked package's folder
    std::filesystem::path records; // what the launcher writes about it, always user_data/custom_levels/<id>
    bool linked = false;
    bool available = true;         // a linked package's folder is where its link says
    PackageLink link;
    std::string origin;            // the manifest's source_type: "zip", "7z", "rar", "folder", "external"
    std::vector<LevelScene> scenes; // the start level first
    std::string readme; // relative to the package; empty when it has no instructions
    Need ce = Need::kNone, atx = Need::kNone;
    bool config_warnings = false;
    int64_t imported_at = 0; // seconds since 1970
    std::string layout;
    bool recorded = false; // its manifest and runtime map are on disk and current
    bool cached = false; // read from its listing record, without analysing its files again
    std::vector<std::string> log; // what the analysis noticed, for the launcher's log

    // The ones that can be played: the start level alone of a mission that names one, since it leads to the others.
    std::vector<const LevelScene*> levels() const;
    // The ones the list shows: levels(), and those that cannot be played and are no part of a mission that can.
    std::vector<const LevelScene*> rows() const;
};

struct LevelRow {
    const LevelPackage* package = nullptr;
    const LevelScene* scene = nullptr; // nullptr: the package has no level to show
};
// By date, then the start level first, then by name.
std::vector<LevelRow> SortedRows(const std::vector<LevelPackage>& packages, LevelSort sort);

struct ListingStats {
    size_t cached = 0, analysed = 0;
    double game_ms = 0, cached_ms = 0, analysed_ms = 0;
    bool cancelled = false;
};

// data_dir: the selected game version's data folder, for the original files a level may reuse. Writes the manifest and the
// runtime map of a package that lacks them or whose files changed, as for one installed by an earlier version. A package
// whose files, the game's files and the analysis are as its listing record says is read from that record alone.
std::vector<LevelPackage> ListLevelPackages(const std::filesystem::path& user_dir, const std::filesystem::path& data_dir,
                                            ListingStats* stats = nullptr, const std::atomic<bool>* cancel = nullptr);

size_t PlayableLevels(const std::vector<LevelPackage>& packages);

// What the player confirmed in Compatibility Warning, for one launch: the reasons it showed.
struct PlayConsent {
    std::vector<std::string> reasons; // PlayCheck::keys as the warning showed them
};

// The level as the runtime would find it now, analysed again when a file of it changed. log: for the launcher's log.
struct PlayCheck {
    bool ok = false;
    ImportStatus reason = ImportStatus::kMissingFiles;
    jpt_compat_t compat = JPT_COMPAT_INVALID;
    bool needs_consent = false; // LIMITED COMPATIBILITY without a consent that covers its reasons
    bool experimental = false;  // started by Play Anyway
    std::string layout;
    std::vector<std::string> authorized; // the LIMITED scenes the consent covers: this one and those it leads to
    std::vector<LevelReason> reasons;    // of those scenes, for the warning
    std::vector<std::string> keys;       // what a PlayConsent must hold to cover them
    std::vector<std::string> log;
};
PlayCheck CheckBeforePlay(const std::filesystem::path& user_dir, const std::filesystem::path& data_dir, const std::string& id, const std::string& scene,
                          const PlayConsent* consent = nullptr);
// The scene and the LIMITED COMPATIBILITY scenes of its package that it leads to, directly or through others: what one Play
// Anyway covers.
std::vector<const LevelScene*> PlayAnywayScenes(const LevelPackage& package, const LevelScene& scene);
bool LeadsToLimited(const LevelPackage& package, const LevelScene& scene);
// The request the game is started with; the authorized scenes only for an experimental check.
jpt_custom_level_request_t LaunchRequest(const std::string& id, const std::string& scene, const PlayCheck& check);

// A level's problems, for the launcher's log.
std::vector<std::string> LevelProblems(const LevelPackage& package, const LevelScene& scene, const std::filesystem::path& data_dir);
// What Bug Report logs for a level: its own problems only, never a sibling's; without one, every level that cannot be played.
std::vector<std::string> ReportLines(const LevelPackage& package, const LevelScene* scene, const std::filesystem::path& data_dir);

// Removes the package's folder and nothing else; its saves stay.
bool DeleteLevelPackage(const std::filesystem::path& user_dir, const std::string& id, std::string& error);

// The readme file of an installed package, for the system to open: inside the package's own folder, a plain file (never a
// link) and of a kind ChooseReadme accepts, so nothing executable is ever handed over.
bool ResolveReadme(const std::filesystem::path& user_dir, const LevelPackage& package, std::filesystem::path& file);
bool ReadLevelText(const std::filesystem::path& file, std::string& text);

} // namespace jpt

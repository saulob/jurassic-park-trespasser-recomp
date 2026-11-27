// The launcher's Custom Levels page: import or link, list, play, open the readme of, rescan, relocate, unlink and remove the
// packages of user_data/custom_levels.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "custom_levels.h"
#include "jpt_custom_level.h"
#include "level_listing.h"
#include "level_warning.h"
#include "trg/launcher.h"
#include "trg/platform.h"

namespace jpt {

// What a chosen path is for.
enum class LevelPick { kArchive, kFolder, kLink, kRelocate };

// A start the page asked for. anyway: Play Anyway, which shows Compatibility Warning before anything LIMITED starts; consent:
// what the player confirmed there, for this start alone.
struct PendingPlay {
    std::string id, scene;
    bool anyway = false;
    std::optional<PlayConsent> consent;
};

// Compatibility Warning as it is shown: Cancel is the default, and only Play Anyway starts the level.
struct CompatibilityWarning {
    std::string id, scene, file;
    std::vector<LevelReason> reasons;
    std::vector<std::string> keys;
    bool changed = false; // the level changed after an earlier confirmation
};


struct CustomLevelsHooks {
    std::function<void(LevelPick)> choose; // the chosen path comes back through Picked, now or on a later frame
    std::function<bool()> game_ready;
    std::function<std::filesystem::path()> data_dir;
    std::function<void(const jpt_custom_level_request_t&)> play;
    std::function<void(const std::filesystem::path&)> open_folder;
    std::function<void(const std::filesystem::path&)> open_file; // with the system's own application
    // About a level, with the log of its last run when there is one; without a level, about this session.
    std::function<void(const std::string& package, const std::string& scene)> open_bug_report;
    std::function<std::string()> sort;
    std::function<void(const std::string&)> set_sort;
};

class CustomLevelsPage {
public:
    CustomLevelsPage(std::filesystem::path user_dir, CustomLevelsHooks hooks);
    ~CustomLevelsPage();

    void Draw(trg::Ui& ui);
    void Picked(const std::string& path, LevelPick pick);
    // Every frame, whichever page is open: picks up a finished import.
    void Poll(trg::Launcher& launcher);
    bool importing() const { return import_.running(); }

private:
    void LogListing(const Listing& listing);
    void DrawImport(trg::Ui& ui);
    void DrawNotice(trg::Ui& ui);
    void DrawSort(trg::Ui& ui);
    void DrawList(trg::Ui& ui, const std::vector<LevelPackage>& packages);
    void DrawRow(trg::Ui& ui, const LevelPackage& package, const LevelScene* scene);
    void ReportLevel(const LevelPackage& package, const LevelScene* scene);
    void Play(const PendingPlay& play);
    void DrawWarning(trg::Ui& ui);
    void DrawReasons(trg::Ui& ui, const LevelPackage& package, const LevelScene& scene);
    void OpenReadme(trg::Ui& ui, const LevelPackage& package);
    void DrawDelete(trg::Ui& ui);
    void DrawUnlink(trg::Ui& ui);
    void DrawOrigin(trg::Ui& ui, const LevelPackage& package);
    void RunPending(trg::Ui& ui);

    std::filesystem::path user_dir_;
    CustomLevelsHooks hooks_;
    LevelListing listing_;
    size_t logged_packages_ = size_t(-1);
    int last_frame_ = -2;
    trg::Task import_;
    std::shared_ptr<ImportResult> outcome_;
    std::shared_ptr<std::atomic<bool>> import_shown_done_; // the page has drawn the import at 100%, or is not shown
    LevelNotice notice_;
    // Started once the listing job, which may be writing the same package's records, is idle.
    std::optional<PendingPlay> pending_play_;
    std::optional<CompatibilityWarning> warning_;
    bool open_warning_ = false;
    std::string delete_id_;
    bool open_delete_ = false;
    std::shared_ptr<LinkResult> link_outcome_;
    std::string unlink_id_, relocate_id_;
    bool open_unlink_ = false;
    // Wait, like a Play, for the listing job, which may be writing the same package's records.
    std::optional<std::string> pending_rescan_;
    std::optional<std::pair<std::string, std::string>> pending_relocate_; // package, folder
    std::set<std::string> logged_;
};

} // namespace jpt

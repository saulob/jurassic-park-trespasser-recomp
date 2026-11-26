// The launcher's Custom Levels page: import, list, play, open the readme of and remove the packages of user_data/custom_levels.
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
#include "trg/launcher.h"
#include "trg/platform.h"

namespace jpt {

struct CustomLevelsHooks {
    std::function<void(bool folder)> choose; // the chosen path comes back through Picked, now or on a later frame
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
    void Picked(const std::string& path, bool folder);
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
    void Play(const std::string& id, const std::string& scene);
    void OpenReadme(trg::Ui& ui, const LevelPackage& package);
    void DrawDelete(trg::Ui& ui);

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
    std::optional<std::pair<std::string, std::string>> pending_play_;
    std::string delete_id_;
    bool open_delete_ = false;
    std::set<std::string> logged_;
};

} // namespace jpt

// The launcher's Custom Levels page: import, list, play, open the readme of and remove the packages of user_data/custom_levels.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "custom_levels.h"
#include "jpt_custom_level.h"
#include "trg/launcher.h"
#include "trg/platform.h"

namespace jpt {

struct CustomLevelsHooks {
    std::function<void(bool folder)> choose; // the chosen path comes back through Picked, now or on a later frame
    std::function<bool()> game_ready;
    std::function<std::string()> version;
    std::function<std::filesystem::path()> data_dir;
    std::function<void(const jpt_custom_level_request_t&)> play;
    std::function<void(const std::filesystem::path&)> open_folder;
    std::function<void(const std::filesystem::path&)> open_file; // with the system's own application
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
    void Refresh();
    void DrawImport(trg::Ui& ui);
    void DrawList(trg::Ui& ui);
    void DrawRow(trg::Ui& ui, const LevelPackage& package, const LevelScene& scene);
    void OpenReadme(trg::Ui& ui, const LevelPackage& package);
    void DrawDelete(trg::Ui& ui);
    std::string StatusText(const LevelPackage& package, const LevelScene& scene) const;

    std::filesystem::path user_dir_;
    CustomLevelsHooks hooks_;
    std::vector<LevelPackage> packages_;
    std::filesystem::path listed_data_dir_;
    int last_frame_ = -2;
    bool reload_ = true;
    trg::Task import_;
    std::shared_ptr<ImportResult> outcome_;
    std::string failure_, failure_detail_;
    std::string delete_id_;
    bool open_delete_ = false;
};

} // namespace jpt

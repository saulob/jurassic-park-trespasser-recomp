#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include <imgui.h>

#include "file_browser.h"
#include "trg/launcher.h"

namespace jpt {

class DiscBrowser {
public:
    void Open(const std::filesystem::path& program_folder);
    bool active() const { return active_; }
    std::optional<std::filesystem::path> Draw(ImFont* font, float font_size, const trg::Theme& theme);

private:
    void Go(const std::filesystem::path& folder);

    bool active_ = false;
    bool popup_open_ = false;
    bool show_all_ = false;
    bool focus_list_ = false;
    std::filesystem::path folder_;
    std::vector<Place> places_;
    FolderListing listing_;
};

}  // namespace jpt

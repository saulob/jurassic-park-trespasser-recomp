#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "file_browser.h"
#include "trg/launcher.h"

namespace jpt {

struct BrowsePurpose {
    const char* title;
    const char* extensions; // ".iso", ".zip;.7z"; the folder purpose lists folders only
    const char* none_here;
    const char* hint;
    bool folder = false;
};

extern const BrowsePurpose kBrowseDisc;
extern const BrowsePurpose kBrowseLevelArchive;
extern const BrowsePurpose kBrowseLevelFolder;
extern const BrowsePurpose kBrowseLinkFolder;
extern const BrowsePurpose kBrowseRelocateFolder;
extern const BrowsePurpose kBrowseInstallFolder;
extern const BrowsePurpose kBrowseDiscFolder;

class DiscBrowser {
public:
    void Open(const std::filesystem::path& program_folder, const BrowsePurpose& purpose = kBrowseDisc);
    const BrowsePurpose& purpose() const { return *purpose_; }
    bool active() const { return active_; }
    std::optional<std::filesystem::path> Draw(ImFont* font, float font_size, const trg::Theme& theme);

private:
    void Go(const std::filesystem::path& folder);

    std::string_view Filter() const;

    const BrowsePurpose* purpose_ = &kBrowseDisc;
    bool active_ = false;
    bool popup_open_ = false;
    bool show_all_ = false;
    bool focus_list_ = false;
    std::filesystem::path folder_;
    std::vector<Place> places_;
    FolderListing listing_;
};

}  // namespace jpt

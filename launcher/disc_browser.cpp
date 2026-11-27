#include "disc_browser.h"

#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include "localization/ui_text.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {

const BrowsePurpose kBrowseDisc = {"Select your Trespasser disc image (.iso)", ".iso", "No .iso files or folders here.",
                                   "Choose the .iso file. Controller: D-pad to move, A to choose, B to cancel."};
const BrowsePurpose kBrowseLevelArchive = {"Select a custom level (.zip, .7z or .rar)", ".zip;.7z;.rar", "No .zip, .7z or .rar files or folders here.",
                                           "Choose the archive. Controller: D-pad to move, A to choose, B to cancel."};
const BrowsePurpose kBrowseLevelFolder = {"Select the folder of a custom level", "/", "No folders here.",
                                          "Open the level's folder, then choose it. Controller: D-pad to move, A to choose, B to cancel.",
                                          true};
const BrowsePurpose kBrowseLinkFolder = {"Select a folder of custom levels to link", "/", "No folders here.",
                                         "Open the folder that holds the levels, then choose it. Controller: D-pad to move, A to choose, B to cancel.",
                                         true};
const BrowsePurpose kBrowseRelocateFolder = {"Select where the linked folder is now", "/", "No folders here.",
                                             "Open the folder in its new place, then choose it. Controller: D-pad to move, A to choose, B to cancel.",
                                             true};
const BrowsePurpose kBrowseInstallFolder = {"Select the folder of your Trespasser installation", "/", "No folders here.",
                                            "Open the game's folder, or a copy of the disc, then choose it. Controller: D-pad to move, A to choose, B to cancel.",
                                            true};
const BrowsePurpose kBrowseDiscFolder = {"Select the folder of the original disc", "/", "No folders here.",
                                         "Open the folder that holds a copy of the disc, then choose it. Controller: D-pad to move, A to choose, B to cancel.",
                                         true};

std::string_view DiscBrowser::Filter() const {
    return show_all_ && !purpose_->folder ? std::string_view() : std::string_view(purpose_->extensions);
}

void DiscBrowser::Open(const fs::path& program_folder, const BrowsePurpose& purpose) {
    purpose_ = &purpose;
    places_ = StandardPlaces(program_folder);
    std::error_code ec;
    show_all_ = false;
    active_ = true;
    popup_open_ = false;
    Go(!folder_.empty() && fs::is_directory(folder_, ec) ? folder_ : StartingFolder(places_, purpose_->extensions));
}

void DiscBrowser::Go(const fs::path& folder) {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(folder, ec);
    folder_ = ec ? folder : resolved;
    listing_ = ListFolder(folder_, Filter(), false);
    focus_list_ = true;
}

std::optional<fs::path> DiscBrowser::Draw(ImFont* font, float font_size, const trg::Theme& theme) {
    if (!active_) {
        return std::nullopt;
    }
    const char* title = Tr(purpose_->title);
    if (!popup_open_) {
        ImGui::OpenPopup(title);
        popup_open_ = true;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.88f, viewport->Size.y * 0.86f), ImGuiCond_Always);
    bool keep = true;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushFont(font, font_size);
    const std::pair<ImGuiCol, ImVec4> colors[] = {
        {ImGuiCol_WindowBg, theme.popup},
        {ImGuiCol_PopupBg, theme.popup},
        {ImGuiCol_TitleBg, theme.panel},
        {ImGuiCol_TitleBgActive, theme.panel},
        {ImGuiCol_ChildBg, theme.panel},
        {ImGuiCol_FrameBg, theme.frame},
        {ImGuiCol_FrameBgHovered, theme.frame_hot},
        {ImGuiCol_FrameBgActive, theme.frame_active},
        {ImGuiCol_Button, theme.frame},
        {ImGuiCol_ButtonHovered, theme.frame_hot},
        {ImGuiCol_ButtonActive, theme.frame_active},
        {ImGuiCol_Header, ImVec4(theme.accent.x, theme.accent.y, theme.accent.z, 0.35f)},
        {ImGuiCol_HeaderHovered, theme.frame_hot},
        {ImGuiCol_HeaderActive, theme.frame_active},
        {ImGuiCol_CheckMark, theme.accent},
        {ImGuiCol_NavCursor, theme.accent},
        {ImGuiCol_Text, theme.text},
        {ImGuiCol_TextDisabled, theme.dim},
    };
    for (const auto& [slot, color] : colors) {
        ImGui::PushStyleColor(slot, color);
    }
    const int pushed = static_cast<int>(std::size(colors));
    std::optional<fs::path> picked;
    if (!ImGui::BeginPopupModal(title, &keep, kFlags)) {
        active_ = false;
        ImGui::PopStyleColor(pushed);
        ImGui::PopFont();
        return std::nullopt;
    }

    const fs::path parent = folder_.parent_path();
    ImGui::BeginDisabled(parent.empty() || parent == folder_);
    if (ImGui::Button(Tr("Up"))) {
        Go(parent);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!purpose_->folder && ImGui::Checkbox(Tr("Show all files"), &show_all_)) {
        Go(folder_);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Utf8(folder_).c_str());

    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const float places_width = ImGui::GetContentRegionAvail().x * 0.24f;
    fs::path enter;
    if (ImGui::BeginChild("places", ImVec2(places_width, -footer), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < places_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(Tr(places_[i].label.c_str()), places_[i].path == folder_)) {
                enter = places_[i].path;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("files", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
        if (!listing_.readable) {
            ImGui::TextDisabled("%s", Tr("This folder cannot be read."));
        } else if (listing_.entries.empty()) {
            ImGui::TextDisabled("%s", Tr(show_all_ ? "This folder is empty." : purpose_->none_here));
        }
        const float right_edge = ImGui::GetContentRegionAvail().x;
        if (!parent.empty() && parent != folder_ && ImGui::Selectable(Tr("../  (parent folder)"))) {
            enter = parent;
        }
        for (size_t i = 0; i < listing_.entries.size(); ++i) {
            const FolderEntry& entry = listing_.entries[i];
            if (focus_list_) {
                ImGui::SetKeyboardFocusHere();
                focus_list_ = false;
            }
            ImGui::PushID(static_cast<int>(i));
            const std::string label = entry.directory ? entry.name + "/" : entry.name;
            if (ImGui::Selectable(label.c_str())) {
                if (entry.directory) {
                    enter = folder_ / PathFromUtf8(entry.name);
                } else {
                    picked = folder_ / PathFromUtf8(entry.name);
                }
            }
            if (!entry.directory) {
                const std::string size = FormatSize(entry.size);
                ImGui::SameLine(right_edge - ImGui::CalcTextSize(size.c_str()).x);
                ImGui::TextDisabled("%s", size.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (purpose_->folder) {
        if (ImGui::Button(Tr("Choose this folder"))) {
            picked = folder_;
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(Tr("Cancel"))) {
        keep = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr(purpose_->hint));

    if (!enter.empty()) {
        Go(enter);
    }
    if (picked || !keep) {
        ImGui::CloseCurrentPopup();
        active_ = false;
    }
    ImGui::EndPopup();
    ImGui::PopStyleColor(pushed);
    ImGui::PopFont();
    return picked;
}

}  // namespace jpt

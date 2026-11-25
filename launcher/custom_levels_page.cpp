#include "custom_levels_page.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <iterator>
#include <utility>

#include "launcher_log.h"
#include "localization/ui_text.h"
#include "page_margin.h"
#include "utf8.h"

namespace jpt {
namespace {

constexpr char kImportHelp[] =
    "Choose a level package as it was downloaded (ZIP or 7z), or a folder where one was already extracted. It is copied "
    "into your user data folder; the installed game versions are never changed and programs inside a package are left out.";
constexpr char kCompatibilityHelp[] =
    "Levels made for the original game can be played. Extra features of the ATX and Trespasser CE patches are not "
    "available, so a level that depends on them may not work as its author intended.";
constexpr char kMiddleDot[] = "   \xC2\xB7   ";

// Pop-ups are drawn outside the page panel, so they take the launcher's own colours.
class PopupStyle {
public:
    explicit PopupStyle(trg::Ui& ui) {
        const trg::Theme& theme = ui.theme();
        const std::pair<ImGuiCol, ImVec4> colors[] = {
            {ImGuiCol_PopupBg, theme.popup},           {ImGuiCol_ChildBg, theme.panel},
            {ImGuiCol_Button, theme.frame},            {ImGuiCol_ButtonHovered, theme.frame_hot},
            {ImGuiCol_ButtonActive, theme.frame_active}, {ImGuiCol_Text, theme.text},
        };
        for (const auto& [slot, color] : colors) ImGui::PushStyleColor(slot, color);
        count_ = int(std::size(colors));
    }
    ~PopupStyle() { ImGui::PopStyleColor(count_); }
    PopupStyle(const PopupStyle&) = delete;
    PopupStyle& operator=(const PopupStyle&) = delete;

private:
    int count_ = 0;
};

void Colored(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

} // namespace

CustomLevelsPage::CustomLevelsPage(std::filesystem::path user_dir, CustomLevelsHooks hooks)
    : user_dir_(std::move(user_dir)), hooks_(std::move(hooks)) {
    CleanLevelStaging(user_dir_);
}

CustomLevelsPage::~CustomLevelsPage() {
    if (import_.running()) import_.Cancel();
    import_.Finish();
}

void CustomLevelsPage::Refresh() {
    listed_data_dir_ = hooks_.data_dir();
    packages_ = ListLevelPackages(user_dir_, listed_data_dir_);
    reload_ = false;
    Log("custom levels: %zu package(s) in %s", packages_.size(), Utf8(CustomLevelsDir(user_dir_)).c_str());
}

void CustomLevelsPage::Picked(const std::string& path, bool folder) {
    if (path.empty() || import_.running()) return;
    failure_.clear();
    failure_detail_.clear();
    outcome_ = std::make_shared<ImportResult>();
    import_.Start("Reading the package...", [outcome = outcome_, source = PathFromUtf8(path), user = user_dir_, folder](trg::Task& task) {
        ImportHooks hooks;
        hooks.phase = [&task](const char* text) {
            task.SetLabel(text);
            task.Progress(0, 0);
        };
        hooks.progress = [&task](uint64_t done, uint64_t total) {
            task.Progress(static_cast<long long>(done), static_cast<long long>(total));
            return !task.cancelled();
        };
        *outcome = folder ? ImportLevelFolder(source, user, hooks) : ImportLevelArchive(source, user, hooks);
        return outcome->ok() ? std::string() : std::string(ImportStatusTitle(outcome->status));
    });
    Log("importing a custom level from %s", path.c_str());
}

void CustomLevelsPage::Poll(trg::Launcher& launcher) {
    if (import_.Finish() == trg::Task::State::kIdle) return;
    const std::shared_ptr<ImportResult> outcome = std::exchange(outcome_, nullptr);
    if (!outcome) return;
    reload_ = true;
    Log("custom level import: %s%s%s, %zu file(s), %zu left out", ImportStatusTitle(outcome->status), outcome->detail.empty() ? "" : ": ",
        outcome->detail.c_str(), outcome->files, outcome->skipped);
    if (outcome->ok()) {
        std::string text = Tr("Custom level installed");
        text += ": " + outcome->package + ".";
        if (outcome->skipped) text += " " + TrFormat("Files left out: %d.", int(outcome->skipped));
        launcher.SetStatus(text, 8.0);
        return;
    }
    if (outcome->status == ImportStatus::kCancelled) {
        launcher.SetStatus(Tr(ImportStatusTitle(outcome->status)), 6.0);
        return;
    }
    failure_ = ImportStatusTitle(outcome->status);
    failure_detail_ = outcome->detail;
    launcher.SetStatus(Tr(failure_), 8.0);
}

void CustomLevelsPage::Draw(trg::Ui& ui) {
    const int frame = ImGui::GetFrameCount();
    if (frame != last_frame_ + 1) reload_ = true;
    last_frame_ = frame;
    if ((reload_ && !import_.running()) || listed_data_dir_ != hooks_.data_dir()) Refresh();
    DrawImport(ui);
    ui.Heading("Installed levels");
    DrawList(ui);
    ui.Spacer(4);
    ui.Help(kCompatibilityHelp);
    DrawDelete(ui);
}

void CustomLevelsPage::DrawImport(trg::Ui& ui) {
    ui.EndRows();
    if (!ui.TaskProgress(import_)) {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float width = (ImGui::GetContentRegionAvail().x - spacing) / 2;
        if (ImGui::Button(Tr("Import ZIP / 7z"), ImVec2(width, 0))) hooks_.choose(false);
        ImGui::SameLine();
        if (ImGui::Button(Tr("Import Folder"), ImVec2(width, 0))) hooks_.choose(true);
    }
    ui.Help(kImportHelp);
    if (!failure_.empty()) {
        std::string text = Tr(failure_);
        if (!failure_detail_.empty()) text += " (" + failure_detail_ + ")";
        Colored(ui.theme().warn_dot, text);
    }
    ui.Spacer(4);
}

std::string CustomLevelsPage::StatusText(const LevelPackage& package, const LevelScene& scene) const {
    std::string text;
    if (!scene.valid) {
        text = Tr("Not a Trespasser scene");
    } else if (!scene.missing.empty()) {
        text = std::string(Tr("Missing files")) + ": ";
        for (size_t i = 0; i < scene.missing.size(); ++i) text += (i ? ", " : "") + scene.missing[i];
    } else {
        text = Tr("Files complete");
    }
    if (package.mentions_atx) text += std::string(kMiddleDot) + Tr("README mentions ATX");
    if (package.mentions_ce) text += std::string(kMiddleDot) + Tr("README mentions Trespasser CE");
    return text;
}

void CustomLevelsPage::DrawList(trg::Ui& ui) {
    if (packages_.empty()) {
        ui.Help("No custom levels yet. Import a package to see it here.");
        return;
    }
    for (size_t p = 0; p < packages_.size(); ++p) {
        const LevelPackage& package = packages_[p];
        ImGui::PushID(int(p));
        if (package.scenes.empty()) DrawRow(ui, package, LevelScene{});
        for (size_t s = 0; s < package.scenes.size(); ++s) {
            ImGui::PushID(int(s));
            DrawRow(ui, package, package.scenes[s]);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

void CustomLevelsPage::DrawRow(trg::Ui& ui, const LevelPackage& package, const LevelScene& scene) {
    const float scale = ui.scale(), spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool readme = !package.readme.empty();
    const bool complete = scene.valid && scene.missing.empty();
    ImGui::BeginGroup();
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted(package.id.c_str());
    ImGui::PopFont();
    const std::string details = (scene.file.empty() ? std::string(Tr("No scene")) : scene.file) + kMiddleDot + hooks_.version();
    Colored(ui.theme().dim, details);
    Colored(complete ? ui.theme().accent : ui.theme().warn_dot, StatusText(package, scene));

    const int buttons = readme ? 4 : 3;
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = std::min(128.0f * scale, (available - spacing * float(buttons - 1)) / float(buttons));
    ImGui::BeginDisabled(!scene.valid || !scene.loadable || !hooks_.game_ready() || import_.running());
    if (ui.AccentButton(Tr("Play"), ImVec2(width, 0))) {
        jpt_custom_level_request_t request{};
        std::snprintf(request.package, sizeof request.package, "%s", package.id.c_str());
        std::snprintf(request.scene, sizeof request.scene, "%s", scene.path.c_str());
        hooks_.play(request);
    }
    ImGui::EndDisabled();
    if (readme) {
        ImGui::SameLine();
        if (ImGui::Button(Tr("Readme"), ImVec2(width, 0))) OpenReadme(ui, package);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Open Folder"), ImVec2(width, 0))) hooks_.open_folder(package.dir);
    ImGui::SameLine();
    ImGui::BeginDisabled(import_.running());
    if (ImGui::Button(Tr("Delete"), ImVec2(width, 0))) {
        delete_id_ = package.id;
        open_delete_ = true;
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ui.Spacer(4);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(at, ImVec2(at.x + available, at.y), trg::Col(ui.theme().border_row), std::max(1.0f, scale));
    ui.Spacer(6);
}

void CustomLevelsPage::OpenReadme(trg::Ui& ui, const LevelPackage& package) {
    std::filesystem::path file;
    if (ResolveReadme(user_dir_, package, file)) {
        hooks_.open_file(file);
        return;
    }
    Log("custom level %s: readme %s is not an openable file of the package", package.id.c_str(), package.readme.c_str());
    ui.SetStatus(Tr("The readme could not be opened."), 8.0);
}

void CustomLevelsPage::DrawDelete(trg::Ui& ui) {
    constexpr char kPopup[] = "##level_delete";
    const PopupStyle style(ui);
    if (std::exchange(open_delete_, false)) ImGui::OpenPopup(kPopup);
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings))
        return;
    size_t scenes = 0;
    for (const LevelPackage& package : packages_)
        if (package.id == delete_id_) scenes = package.scenes.size();
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted((std::string(Tr("Delete this custom level?")) + "\n\n" + delete_id_).c_str());
    ImGui::PopFont();
    if (scenes > 1) ImGui::TextUnformatted(TrFormat("All %d levels of this package are removed.", int(scenes)).c_str());
    ImGui::TextUnformatted(Tr("Saves and the other packages are kept."));
    ImGui::Dummy(ImVec2(320.0f * ui.scale(), 6.0f * ui.scale()));
    const ImVec2 size(150.0f * ui.scale(), 0.0f);
    const bool confirmed = ui.AccentButton(Tr("Delete"), size);
    ImGui::SameLine();
    const bool cancelled = ImGui::Button(Tr("Cancel"), size) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (confirmed || cancelled) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (!confirmed) return;
    std::string error;
    const bool removed = DeleteLevelPackage(user_dir_, delete_id_, error);
    Log("custom level %s %s%s%s", delete_id_.c_str(), removed ? "removed" : "not removed", error.empty() ? "" : ": ", error.c_str());
    ui.SetStatus(removed ? std::string(Tr("Custom level removed. Saves were kept.")) : std::string(Tr("The custom level could not be removed: ")) + TrMessage(error), 8.0);
    reload_ = true;
}

} // namespace jpt

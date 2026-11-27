#include "custom_levels_page.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cfloat>
#include <cstdio>
#include <iterator>
#include <utility>

#include "launcher_log.h"
#include "loading_view.h"
#include "localization/ui_text.h"
#include "page_margin.h"
#include "utf8.h"

namespace jpt {
namespace {

constexpr char kImportHelp[] =
    "Choose a level package as it was downloaded (ZIP, 7z or RAR), or a folder where one was already extracted. It is checked first and "
    "copied into your user data folder only when this port supports all of it; the installed game versions are never changed and "
    "programs inside a package are left out.";
constexpr char kLinkHelp[] =
    "Link External Folder plays levels where they already are, one folder or a folder of several, without copying them. Their files "
    "are only read and never changed; Unlink forgets the folder and deletes nothing in it. Saves stay in your user data folder.";
constexpr char kCompatibilityHelp[] =
    "Levels made for the original game can be played. A package's own settings last only while it plays. A level that needs a file "
    "it lacks, or a feature of the ATX or Trespasser CE patches, is not installed.";

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

void CustomLevelsPage::LogListing(const Listing& listing) {
    const ListingStats& s = listing.stats;
    if (s.analysed || listing.packages.size() != logged_packages_)
        Log("custom levels: %zu package(s) in %s, %zu from their listing record (%.0f ms), %zu analysed (%.0f ms), game files %.0f ms, total %.0f ms",
            listing.packages.size(), Utf8(CustomLevelsDir(user_dir_)).c_str(), s.cached, s.cached_ms, s.analysed, s.analysed_ms, s.game_ms, listing.total_ms);
    logged_packages_ = listing.packages.size();
    for (const LevelPackage& package : listing.packages) {
        for (const std::string& line : package.log)
            if (logged_.insert(package.id + ": " + line).second) Log("custom level %s: %s", package.id.c_str(), line.c_str());
        for (const LevelScene& scene : package.scenes)
            if (scene.valid && logged_.insert(package.id + ": " + scene.path + ": " + LevelClassName(scene.level_class())).second)
                Log("custom level %s: %s %s", package.id.c_str(), scene.path.c_str(), LevelClassName(scene.level_class()));
    }
}

void CustomLevelsPage::Picked(const std::string& path, LevelPick pick) {
    if (path.empty() || import_.running()) return;
    notice_ = {};
    if (pick == LevelPick::kRelocate) {
        if (!relocate_id_.empty()) pending_relocate_ = {relocate_id_, path};
        return;
    }
    const ImportRequest request{PathFromUtf8(path), user_dir_, hooks_.data_dir(), pick == LevelPick::kFolder};
    if (pick == LevelPick::kLink) {
        link_outcome_ = std::make_shared<LinkResult>();
        import_.Start(kLinkPhases[0], [outcome = link_outcome_, request](trg::Task& task) {
            const auto publish = [&task](const std::string& phase, long long done, long long total) {
                task.SetLabel(phase);
                task.Progress(done, total);
            };
            *outcome = RunLink(request, publish, [&task] { return task.cancelled(); });
            return outcome->ok() || outcome->status == ImportStatus::kAlreadyLinked ? std::string() : std::string(ImportStatusTitle(outcome->status));
        });
        Log("linking custom levels in %s", path.c_str());
        return;
    }
    outcome_ = std::make_shared<ImportResult>();
    import_shown_done_ = std::make_shared<std::atomic<bool>>(false);
    import_.Start(kImportPhases[0], [outcome = outcome_, shown = import_shown_done_, request](trg::Task& task) {
        const auto publish = [&task](const std::string& phase, long long done, long long total) {
            task.SetLabel(phase);
            task.Progress(done, total);
        };
        *outcome = RunImport(request, publish, [&task] { return task.cancelled(); });
        // The bar is taken away as soon as the task ends: it ends once the page has drawn the 100%.
        while (outcome->ok() && !shown->load() && !task.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return outcome->ok() ? std::string() : std::string(ImportStatusTitle(outcome->status));
    });
    Log("importing a custom level from %s", path.c_str());
}

void CustomLevelsPage::Poll(trg::Launcher& launcher) {
    if (import_.running() && import_shown_done_ && ImGui::GetFrameCount() > last_frame_ + 1) import_shown_done_->store(true);
    if (import_.Finish() == trg::Task::State::kIdle) return;
    if (const std::shared_ptr<LinkResult> linked = std::exchange(link_outcome_, nullptr)) {
        listing_.Invalidate(false);
        for (const std::string& line : linked->log) Log("custom level link: %s", line.c_str());
        notice_ = LinkNotice(linked->status);
        if (linked->ok()) notice_.reason = TrFormat("%d package(s) linked. Their files stay where they are.", int(linked->linked.size()));
        launcher.SetStatus(Tr(notice_.title), 8.0);
        return;
    }
    const std::shared_ptr<ImportResult> outcome = std::exchange(outcome_, nullptr);
    if (!outcome) return;
    listing_.Invalidate(false);
    for (const std::string& line : outcome->log) Log("custom level import: %s", line.c_str());
    notice_ = ImportNotice(outcome->status);
    launcher.SetStatus(Tr(notice_.title), 8.0);
}

void CustomLevelsPage::Draw(trg::Ui& ui) {
    const int frame = ImGui::GetFrameCount();
    if (frame != last_frame_ + 1) {
        listing_.Hidden();
        if (last_frame_ >= 0) listing_.Invalidate(false);
    }
    last_frame_ = frame;
    const LevelListing::Frame now = listing_.Advance({user_dir_, hooks_.data_dir()});
    if (now.adopted) LogListing(listing_.list());
    if (pending_play_ && !listing_.busy()) {
        const auto [id, scene] = *std::exchange(pending_play_, std::nullopt);
        Play(id, scene);
    }
    RunPending(ui);
    if (now.view == LevelListing::View::kLoading) {
        if (import_shown_done_) import_shown_done_->store(true);
        DrawLoadingView(ui, "Loading custom levels...");
        return;
    }
    if (now.failed) {
        Log("custom levels could not be listed: %s", listing_.error().c_str());
        notice_ = {"The custom levels could not be listed", "", true};
    }
    DrawImport(ui);
    if (now.view == LevelListing::View::kFailed) return;
    const std::vector<LevelPackage>& packages = listing_.list().packages;
    ui.Heading(TrFormat("Installed levels (%d)", int(PlayableLevels(packages))).c_str());
    if (!packages.empty()) DrawSort(ui);
    DrawList(ui, packages);
    ui.Spacer(4);
    ui.Help(kCompatibilityHelp);
    DrawDelete(ui);
    DrawUnlink(ui);
}

void CustomLevelsPage::RunPending(trg::Ui& ui) {
    if (listing_.busy()) return;
    if (pending_rescan_) {
        const std::string id = *std::exchange(pending_rescan_, std::nullopt);
        const bool done = RescanLevelPackage(user_dir_, id);
        Log("custom level %s: rescan %s", id.c_str(), done ? "requested" : "not possible");
        listing_.Invalidate(false);
    }
    if (pending_relocate_) {
        const auto [id, folder] = *std::exchange(pending_relocate_, std::nullopt);
        std::string error;
        const bool moved = RelocateLevelPackage(user_dir_, id, PathFromUtf8(folder), error);
        Log("custom level %s %s %s%s%s", id.c_str(), moved ? "relocated to" : "not relocated to", folder.c_str(), error.empty() ? "" : ": ", error.c_str());
        ui.SetStatus(moved ? std::string(Tr("Folder relocated. Saves were kept.")) : std::string(Tr("The folder could not be relocated: ")) + TrMessage(error), 8.0);
        listing_.Invalidate(false);
    }
}

void CustomLevelsPage::DrawImport(trg::Ui& ui) {
    ui.EndRows();
    if (ui.TaskProgress(import_)) {
        if (import_.fraction() >= 1.0f && import_shown_done_) import_shown_done_->store(true);
    } else {
        const std::pair<const char*, LevelPick> kButtons[] = {
            {"Import ZIP / 7z / RAR", LevelPick::kArchive}, {"Import Folder", LevelPick::kFolder}, {"Link External Folder", LevelPick::kLink}};
        const float spacing = ImGui::GetStyle().ItemSpacing.x, available = ImGui::GetContentRegionAvail().x;
        float widest = 0;
        for (const auto& [label, pick] : kButtons) widest = std::max(widest, ImGui::CalcTextSize(Tr(label)).x + ImGui::GetStyle().FramePadding.x * 2);
        // One row when every label fits a third of the page, else one button a line (high DPI, longer translations).
        const bool row = widest <= (available - spacing * 2) / 3;
        const float width = row ? (available - spacing * 2) / 3 : available;
        for (size_t i = 0; i < std::size(kButtons); ++i) {
            if (i && row) ImGui::SameLine();
            if (ImGui::Button(Tr(kButtons[i].first), ImVec2(width, 0))) hooks_.choose(kButtons[i].second);
        }
    }
    DrawNotice(ui);
    ui.Help(kImportHelp);
    ui.Help(kLinkHelp);
    ui.Spacer(4);
}

void CustomLevelsPage::DrawNotice(trg::Ui& ui) {
    if (notice_.title.empty()) return;
    ui.Spacer(4);
    Colored(notice_.failed ? ui.theme().warn_dot : ui.theme().accent, Tr(notice_.title));
    if (!notice_.reason.empty()) Colored(ui.theme().text, Tr(notice_.reason));
    if (notice_.failed) {
        if (ImGui::Button(Tr("Bug Report"))) hooks_.open_bug_report("", "");
        ImGui::SameLine();
    }
    if (ImGui::Button(Tr("Close"))) notice_ = {};
    ui.Spacer(4);
}

void CustomLevelsPage::DrawSort(trg::Ui& ui) {
    static constexpr std::pair<LevelSort, const char*> kSorts[] = {
        {LevelSort::kNewest, "Newest first"}, {LevelSort::kOldest, "Oldest first"}, {LevelSort::kNameUp, "Name A to Z"}, {LevelSort::kNameDown, "Name Z to A"}};
    const LevelSort current = LevelSortOf(hooks_.sort());
    const char* label = kSorts[0].second;
    for (const auto& [sort, text] : kSorts)
        if (sort == current) label = text;
    ui.EndRows();
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 240.0f * ui.scale()));
    if (ImGui::BeginCombo("##level_sort", Tr(label))) {
        for (const auto& [sort, text] : kSorts)
            if (ImGui::Selectable(Tr(text), sort == current) && sort != current) hooks_.set_sort(LevelSortKey(sort));
        ImGui::EndCombo();
    }
    ui.Spacer(4);
}

void CustomLevelsPage::DrawList(trg::Ui& ui, const std::vector<LevelPackage>& packages) {
    if (packages.empty()) {
        ui.Help("No custom levels yet. Import a package to see it here.");
        return;
    }
    for (const LevelRow& row : SortedRows(packages, LevelSortOf(hooks_.sort()))) {
        ImGui::PushID(row.package->id.c_str());
        ImGui::PushID(row.scene ? row.scene->path.c_str() : "");
        DrawRow(ui, *row.package, row.scene);
        ImGui::PopID();
        ImGui::PopID();
    }
}

void CustomLevelsPage::Play(const std::string& id, const std::string& scene) {
    const PlayCheck check = CheckBeforePlay(user_dir_, hooks_.data_dir(), id, scene);
    for (const std::string& line : check.log) Log("custom level: %s", line.c_str());
    if (!check.ok) {
        notice_ = {"This custom level cannot be played", ImportStatusTitle(ImportStatus::kNotSupported), true};
        listing_.Invalidate(false);
        return;
    }
    jpt_custom_level_request_t request{};
    std::snprintf(request.package, sizeof request.package, "%s", id.c_str());
    std::snprintf(request.scene, sizeof request.scene, "%s", scene.c_str());
    std::snprintf(request.layout, sizeof request.layout, "%s", check.layout.c_str());
    hooks_.play(request);
}

// The reasons go to this session's log, which Bug Report then offers: never an older run of the level.
void CustomLevelsPage::ReportLevel(const LevelPackage& package, const LevelScene* scene) {
    for (const std::string& line : ReportLines(package, scene, hooks_.data_dir())) Log("%s", line.c_str());
    hooks_.open_bug_report("", "");
}

void CustomLevelsPage::DrawRow(trg::Ui& ui, const LevelPackage& package, const LevelScene* scene) {
    const float scale = ui.scale(), spacing = ImGui::GetStyle().ItemSpacing.x;
    const RowActions actions = package.available ? ActionsOf(scene, !package.readme.empty()) : RowActions{};
    ImGui::BeginGroup();
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted(package.id.c_str());
    ImGui::PopFont();
    DrawOrigin(ui, package);
    if (scene) Colored(ui.theme().dim, scene->file);
    const char* status = !package.available ? "Folder unavailable" : scene ? LevelStatusText(*scene) : "Not supported";
    Colored(actions.play ? ui.theme().accent : ui.theme().warn_dot, Tr(status));
    if (!package.available) Colored(ui.theme().dim, Tr("The folder was moved, renamed or is on a drive that is not connected. Relocate it or connect the drive."));
    if (scene)
        for (const std::string& next : LevelNotes(*scene)) Colored(ui.theme().dim, TrFormat("Leads to %s, which is not included: the level ends there", next.c_str()));

    const int buttons = (package.available ? 2 : 1) + (package.linked ? 1 : 0) + (actions.play ? 1 : 0) + (actions.readme ? 1 : 0) + (actions.report ? 1 : 0);
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = std::min(128.0f * scale, (available - spacing * float(buttons - 1)) / float(buttons));
    if (actions.play) {
        ImGui::BeginDisabled(!hooks_.game_ready() || import_.running() || pending_play_.has_value());
        if (ui.AccentButton(Tr("Play"), ImVec2(width, 0))) pending_play_ = {package.id, scene->path};
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (actions.readme) {
        if (ImGui::Button(Tr("Readme"), ImVec2(width, 0))) OpenReadme(ui, package);
        ImGui::SameLine();
    }
    if (actions.report) {
        if (ImGui::Button(Tr("Bug Report"), ImVec2(width, 0))) ReportLevel(package, scene);
        ImGui::SameLine();
    }
    if (package.available) {
        if (ImGui::Button(Tr("Open Folder"), ImVec2(width, 0))) hooks_.open_folder(package.dir);
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(import_.running());
    if (package.linked) {
        if (package.available && ImGui::Button(Tr("Rescan"), ImVec2(width, 0))) pending_rescan_ = package.id;
        if (!package.available && ImGui::Button(Tr("Relocate Folder"), ImVec2(width, 0))) {
            relocate_id_ = package.id;
            hooks_.choose(LevelPick::kRelocate);
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Unlink"), ImVec2(width, 0))) {
            unlink_id_ = package.id;
            open_unlink_ = true;
        }
    } else if (ImGui::Button(Tr("Delete"), ImVec2(width, 0))) {
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

void CustomLevelsPage::DrawOrigin(trg::Ui& ui, const LevelPackage& package) {
    if (package.linked) {
        const std::string mission = package.link.active_fm.empty() ? std::string() : " \xC2\xB7 " + package.link.active_fm;
        Colored(ui.theme().dim, std::string(Tr("External Folder")) + ": " + Utf8(package.link.root) + mission);
        return;
    }
    const bool archive = package.origin == "zip" || package.origin == "7z" || package.origin == "rar";
    if (archive || package.origin == "folder") Colored(ui.theme().dim, Tr(archive ? "Imported Archive" : "Imported Folder"));
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
    for (const LevelPackage& package : listing_.list().packages)
        if (package.id == delete_id_) scenes = package.rows().size();
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
    if (removed) listing_.Forget(delete_id_);
    listing_.Invalidate(false);
}

void CustomLevelsPage::DrawUnlink(trg::Ui& ui) {
    constexpr char kPopup[] = "##level_unlink";
    const PopupStyle style(ui);
    if (std::exchange(open_unlink_, false)) ImGui::OpenPopup(kPopup);
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted((std::string(Tr("Unlink this external folder?")) + "\n\n" + unlink_id_).c_str());
    ImGui::PopFont();
    ImGui::TextUnformatted(Tr("Nothing in the folder is deleted. Saves are kept."));
    ImGui::Dummy(ImVec2(320.0f * ui.scale(), 6.0f * ui.scale()));
    const ImVec2 size(150.0f * ui.scale(), 0.0f);
    const bool confirmed = ui.AccentButton(Tr("Unlink"), size);
    ImGui::SameLine();
    const bool cancelled = ImGui::Button(Tr("Cancel"), size) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (confirmed || cancelled) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (!confirmed) return;
    std::string error;
    const bool unlinked = UnlinkLevelPackage(user_dir_, unlink_id_, error);
    Log("custom level %s %s%s%s", unlink_id_.c_str(), unlinked ? "unlinked" : "not unlinked", error.empty() ? "" : ": ", error.c_str());
    ui.SetStatus(unlinked ? std::string(Tr("Folder unlinked. Its files and your saves were kept.")) : std::string(Tr("The folder could not be unlinked: ")) + TrMessage(error),
                 8.0);
    if (unlinked) listing_.Forget(unlink_id_);
    listing_.Invalidate(false);
}

} // namespace jpt

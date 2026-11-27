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
    "copied into your user data folder when at least one of its levels can be started; the installed game versions are never changed "
    "and programs inside a package are left out.";
constexpr char kLinkHelp[] =
    "Link External Folder plays levels where they already are, one folder or a folder of several, without copying them. Their files "
    "are only read and never changed; Unlink forgets the folder and deletes nothing in it. Saves stay in your user data folder.";
constexpr char kCompatibilityHelp[] =
    "Ready levels play as made. Limited Compatibility levels use features of the ATX or Trespasser CE patches that this port does not "
    "have yet: Play Anyway starts them after a warning. Invalid / Unsafe levels, with missing or damaged files, are never started. "
    "Bug Report sends a level's compatibility report when you choose to.";
constexpr char kWarningTitle[] = "Compatibility Warning";
constexpr char kWarningText[] = "This level uses features that are not fully supported yet.";
constexpr char kWarningRisk[] = "You may experience graphical issues, broken scripts, gameplay problems or crashes.";
constexpr char kWarningQuestion[] = "Would you like to continue?";
const ImVec4 kLimitedColor = ImVec4(1.0f, 0.80f, 0.25f, 1.0f);

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

ImVec4 CompatColor(const trg::Theme& theme, jpt_compat_t compat) {
    if (compat == JPT_COMPAT_READY) return theme.accent;
    return compat == JPT_COMPAT_LIMITED ? kLimitedColor : theme.warn_dot;
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
            if (scene.valid && logged_.insert(package.id + ": " + scene.path + ": " + LevelClassName(scene.level_class()) + jpt_compat_name(scene.compatibility())).second)
                Log("custom level %s: %s %s, compatibility=%s", package.id.c_str(), scene.path.c_str(), LevelClassName(scene.level_class()), jpt_compat_name(scene.compatibility()));
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
        if (linked->ok() && linked->limited)
            notice_.reason += std::string(" ") + TrFormat("%d level(s) have limited compatibility: they start only with Play Anyway.", int(linked->limited));
        launcher.SetStatus(Tr(notice_.title), 8.0);
        return;
    }
    const std::shared_ptr<ImportResult> outcome = std::exchange(outcome_, nullptr);
    if (!outcome) return;
    listing_.Invalidate(false);
    for (const std::string& line : outcome->log) Log("custom level import: %s", line.c_str());
    notice_ = ImportNotice(outcome->status);
    if (outcome->ok() && outcome->limited)
        notice_.reason = TrFormat("%d level(s) have limited compatibility: they start only with Play Anyway.", int(outcome->limited));
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
        Play(*std::exchange(pending_play_, std::nullopt));
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
    DrawWarning(ui);
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

// Checked again at each start: a consent covers the reasons it was given for, in this start alone.
void CustomLevelsPage::Play(const PendingPlay& play) {
    const PlayCheck check = CheckBeforePlay(user_dir_, hooks_.data_dir(), play.id, play.scene, play.consent ? &*play.consent : nullptr);
    for (const std::string& line : check.log) Log("custom level: %s", line.c_str());
    const bool ask = check.needs_consent || (check.ok && play.anyway && !play.consent && !check.keys.empty());
    if (ask) {
        const std::string file = play.scene.substr(play.scene.find_last_of('/') + 1);
        warning_ = CompatibilityWarning{play.id, play.scene, file, check.reasons, check.keys, play.consent.has_value()};
        open_warning_ = true;
        Log("custom level %s: Compatibility Warning shown for %s, %zu reason(s)", play.id.c_str(), play.scene.c_str(), check.reasons.size());
        return;
    }
    if (!check.ok) {
        notice_ = {"This custom level cannot be played", ImportStatusTitle(ImportStatus::kNotSupported), true};
        listing_.Invalidate(false);
        return;
    }
    const jpt_custom_level_request_t request = LaunchRequest(play.id, play.scene, check);
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
    const char* status = !package.available ? "Folder unavailable" : scene ? LevelStatusText(*scene) : "Invalid / Unsafe";
    Colored(CompatColor(ui.theme(), package.available && scene ? scene->compatibility() : JPT_COMPAT_INVALID), Tr(status));
    const bool continues_limited = package.available && actions.play && LeadsToLimited(package, *scene);
    const bool anyway = actions.play_anyway || continues_limited;
    if (continues_limited) Colored(ui.theme().dim, Tr("Leads to levels with limited compatibility: Play Anyway covers them."));
    if (package.available && scene && (anyway || !actions.play)) DrawReasons(ui, package, *scene);
    if (!package.available) Colored(ui.theme().dim, Tr("The folder was moved, renamed or is on a drive that is not connected. Relocate it or connect the drive."));
    if (scene)
        for (const std::string& next : LevelNotes(*scene)) Colored(ui.theme().dim, TrFormat("Leads to %s, which is not included: the level ends there", next.c_str()));

    const int buttons = (package.available ? 2 : 1) + (package.linked ? 1 : 0) + (actions.play ? 1 : 0) + (anyway ? 1 : 0) + (actions.readme ? 1 : 0) + (actions.report ? 1 : 0);
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = std::min(128.0f * scale, (available - spacing * float(buttons - 1)) / float(buttons));
    if (actions.play) {
        ImGui::BeginDisabled(!hooks_.game_ready() || import_.running() || pending_play_.has_value());
        if (ui.AccentButton(Tr("Play"), ImVec2(width, 0))) pending_play_ = PendingPlay{package.id, scene->path, false, std::nullopt};
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (anyway) {
        ImGui::BeginDisabled(!hooks_.game_ready() || import_.running() || pending_play_.has_value() || warning_.has_value());
        if (ImGui::Button(Tr("Play Anyway"), ImVec2(width, 0))) pending_play_ = PendingPlay{package.id, scene->path, true, std::nullopt};
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

// Why a level is not READY: what the runtime leaves out apart from what keeps it from loading at all.
void CustomLevelsPage::DrawReasons(trg::Ui& ui, const LevelPackage& package, const LevelScene& scene) {
    std::vector<std::pair<std::string, LevelReason>> reasons;
    const std::vector<const LevelScene*> scenes = scene.compatibility() == JPT_COMPAT_INVALID ? std::vector<const LevelScene*>{&scene} : PlayAnywayScenes(package, scene);
    for (const LevelScene* each : scenes)
        for (const LevelReason& reason : ReasonsOf(*each)) reasons.push_back({each == &scene ? std::string() : each->file, reason});
    if (reasons.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().dim);
    const bool open = ImGui::TreeNodeEx("##reasons", ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%d)", Tr("Compatibility details"), int(reasons.size()));
    ImGui::PopStyleColor();
    if (!open) return;
    for (const auto& [file, reason] : reasons) {
        const std::string where = file.empty() ? std::string() : file + ": ";
        Colored(CompatColor(ui.theme(), reason.compat), std::string(Tr(IssueKindLabel(reason.kind))) + " \xC2\xB7 " + where + reason.code + " " + reason.subject);
        if (!reason.detail.empty()) Colored(ui.theme().dim, TrMessage(reason.detail));
    }
    ImGui::TreePop();
}

void CustomLevelsPage::DrawWarning(trg::Ui& ui) {
    constexpr char kPopup[] = "##level_compatibility_warning";
    const PopupStyle style(ui);
    if (std::exchange(open_warning_, false)) ImGui::OpenPopup(kPopup);
    const float scale = ui.scale();
    const ImGuiStyle& style_now = ImGui::GetStyle();
    const ImGuiViewport* view = ImGui::GetMainViewport();
    const float label = std::max(ImGui::CalcTextSize(Tr("Cancel")).x, ImGui::CalcTextSize(Tr("Play Anyway")).x) + style_now.FramePadding.x * 2.0f;
    const WarningLayout layout = LayOutWarning(scale, view->WorkSize.x, view->WorkSize.y, label, style_now.WindowPadding.x, style_now.ItemSpacing.x);
    ImGui::SetNextWindowPos(view->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(layout.width, 0.0f), ImVec2(layout.width, view->WorkSize.y - 32.0f * scale));
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings))
        return;
    if (!warning_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    Colored(kLimitedColor, Tr(kWarningTitle));
    ImGui::PopFont();
    ImGui::TextUnformatted(warning_->file.c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr(kWarningText));
    ImGui::TextUnformatted(Tr(kWarningRisk));
    if (warning_->changed) Colored(ui.theme().warn_dot, Tr("The level changed since you last confirmed it. Check its reasons again."));
    ImGui::Spacing();
    if (ImGui::BeginChild("##warning_reasons", ImVec2(0.0f, layout.list_height), ImGuiChildFlags_Borders)) {
        ImGui::PushTextWrapPos(0.0f);
        for (const LevelReason& reason : warning_->reasons) {
            Colored(kLimitedColor, std::string(Tr(IssueKindLabel(reason.kind))) + ": " + reason.code + " " + reason.subject);
            if (!reason.detail.empty()) Colored(ui.theme().dim, TrMessage(reason.detail));
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr(kWarningQuestion));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));
    const ImVec2 size(layout.row ? layout.button : -FLT_MIN, 0.0f);
    const bool cancel = ImGui::Button(Tr("Cancel"), size);
    ImGui::SetItemDefaultFocus();
    if (layout.row) ImGui::SameLine();
    const bool play = ImGui::Button(Tr("Play Anyway"), size);
    const WarningChoice choice = ChooseWarning(cancel, play, ImGui::IsKeyPressed(ImGuiKey_Escape, false), ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
    if (choice != WarningChoice::kNone) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (choice == WarningChoice::kNone) return;
    const CompatibilityWarning shown = *std::exchange(warning_, std::nullopt);
    Log("custom level %s: Compatibility Warning for %s: %s", shown.id.c_str(), shown.scene.c_str(), choice == WarningChoice::kPlayAnyway ? "Play Anyway" : "cancelled");
    if (choice == WarningChoice::kPlayAnyway) pending_play_ = PendingPlay{shown.id, shown.scene, true, PlayConsent{shown.keys}};
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

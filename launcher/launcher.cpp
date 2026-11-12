#include "jpt_launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "trg/headers.h"
#include "trg/launcher.h"
#include "trg/platform.h"

#include "game_files.h"
#include "game_install.h"
#include "header_art.h"
#include "host.h"
#include "jpt_config.h"
#include "jpt_display.h"
#include "launcher_log.h"
#include "launcher_quit.h"
#include "localization/ui_text.h"
#include "options.h"
#include "page_icons.h"
#include "placement.h"
#include "play_gate.h"
#include "settings.h"
#include "system_info.h"
#include "utf8.h"

#ifndef JPT_PORT_VERSION
#define JPT_PORT_VERSION "unknown"
#endif
#define JPT_TRG_LAUNCHER_VERSION "1.1.0"

namespace jpt {
namespace {

const char* TranslateLauncherText(const char* english) { return Tr(english); }

const ImVec4 kAccent = ImVec4(0x8C / 255.0f, 0xC6 / 255.0f, 0x3F / 255.0f, 1.0f);
const ImVec4 kActionEdge = ImVec4(0x4F / 255.0f, 0x7A / 255.0f, 0x24 / 255.0f, 1.0f);
const ImVec4 kActionLink = ImVec4(0xC8 / 255.0f, 0xE6 / 255.0f, 0x9C / 255.0f, 1.0f);
const std::string kPortUrl = std::string("https://") + kPortRepository;
constexpr double kPlayFeedbackSeconds = 0.4;

constexpr char kShowLauncherHelp[] =
    "Off starts the game directly. Hold Shift while starting, or start the program with --launcher, to bring it back.";
constexpr char kWindowSizeHelp[] =
    "The game window's size in pixels in Windowed mode. Auto shows the picture at the monitor's scale. The picture "
    "keeps its shape: other window shapes get black bars.";
constexpr char kMonitorHelp[] = "The display the game opens on. Native render resolution follows it.";
constexpr char kAspectRatioHelp[] =
    "The shape of the game's 3D view. Wider shapes show more of the world to the sides (Hor+): the vertical view stays "
    "the original one and nothing is stretched. Menus stay 4:3, centred.";
constexpr char kPresentationHelp[] =
    "Controls how the rendered image fits a window or display with a different aspect ratio. This is not widescreen: "
    "see Aspect ratio.";
constexpr char kRendererHelp[] =
    "Direct3D 11 draws the game's 3D on the graphics card at the render resolution. Software is the game's own "
    "rasteriser at its original size. F5 switches while playing.";
constexpr char kRenderResolutionHelp[] =
    "The size the 3D world is really drawn at, with its own depth buffer and anti-aliasing; it follows the Aspect ratio "
    "on the Display page. Native uses the monitor's size. Original keeps the earlier compatibility size.";
constexpr char kAntiAliasingHelp[] =
    "Multisample anti-aliasing of the 3D world. Only the levels this graphics card supports are listed.";
constexpr char kTextureFilteringHelp[] =
    "Sharper, steadier textures at a distance and on the ground seen at an angle, with mipmaps generated for the 3D "
    "world. Original uses the game's own filtering without them; Bilinear smooths every texture.";
constexpr char kVSyncHelp[] = "On prevents tearing. Off allows it, for the lowest latency and G-Sync/FreeSync.";
constexpr char kShowFpsTitleHelp[] =
    "Shows the renderer, the frame rate and the game's screen size in the window title.";

trg::Theme LauncherTheme() {
    trg::Theme theme = trg::Theme::Midnight().WithAccent(kAccent).WithAction(kAccent, kActionEdge, kActionLink);
    // Warnings stay red-orange so they are never mistaken for the accent.
    theme.warn = ImVec4(0.47f, 0.18f, 0.10f, 1.0f);
    theme.warn_dot = ImVec4(1.0f, 0.45f, 0.30f, 1.0f);
    return theme;
}

trg::HeaderPainter LauncherHeader(std::function<ImTextureID(int, int)> art, float art_aspect) {
    const trg::HeaderPainter gradient = trg::headers::Gradient(
        IM_COL32(12, 18, 26, 255), IM_COL32(44, 52, 18, 255), IM_COL32(10, 34, 26, 255), IM_COL32(8, 10, 16, 255));
    return [gradient, art = std::move(art), art_aspect](const trg::HeaderContext& c) {
        gradient(c);
        const auto fade = [&c](float share) {
            const ImU32 clear = trg::Col(c.theme.bg, 0.0f), solid = trg::Col(c.theme.bg);
            c.draw->AddRectFilledMultiColor(ImVec2(c.min.x, c.max.y - (c.max.y - c.min.y) * share), c.max, clear, clear,
                                            solid, solid);
        };
        const HeaderArtRect rect = PlaceHeaderArt(c.min.y, c.max.x, c.max.y, art_aspect);
        const float framebuffer = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
        const ImTextureID texture = art(static_cast<int>((rect.max_x - rect.min_x) * framebuffer + 0.5f),
                                        static_cast<int>((rect.max_y - rect.min_y) * framebuffer + 0.5f));
        if (texture != ImTextureID{}) {
            c.draw->PushClipRect(c.min, c.max, false);
            for (const HeaderArtStrip& strip : SplitHeaderArt(rect)) {
                c.draw->AddImage(ImTextureRef(texture), ImVec2(strip.min_x, rect.min_y), ImVec2(strip.max_x, rect.max_y),
                                 ImVec2(strip.min_u, 0.0f), ImVec2(strip.max_u, 1.0f));
            }
            c.draw->PopClipRect();
        }
        fade(kHeaderFade);
    };
}

struct Image {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

// assets/launcher next to the executable (copied there by the build).
bool LoadLauncherImage(const char* file, Image& image) {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return false;
    }
    const std::filesystem::path path = PathFromUtf8(base) / "assets" / "launcher" / file;
    SDL_Surface* loaded = SDL_LoadPNG(Utf8(path).c_str());
    if (!loaded) {
        Log("launcher image unavailable: %s (%s)", Utf8(path).c_str(), SDL_GetError());
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!rgba) {
        return false;
    }
    image.width = rgba->w;
    image.height = rgba->h;
    image.rgba.resize(size_t(rgba->w) * size_t(rgba->h) * 4);
    for (int y = 0; y < rgba->h; ++y) {
        std::memcpy(image.rgba.data() + size_t(y) * size_t(rgba->w) * 4,
                    static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch), size_t(rgba->w) * 4);
    }
    SDL_DestroySurface(rgba);
    return true;
}

ImTextureID LoadSponsorIcon(LauncherHost& host) {
    Image image;
    if (!LoadLauncherImage("sponsor-heart.png", image)) {
        return ImTextureID{};
    }
    // Transparent texels are white too, so linear filtering does not darken the heart's edge.
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        if (image.rgba[i + 3] == 0) {
            image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = 255;
        }
    }
    return host.CreateTexture(image.rgba.data(), image.width, image.height);
}

std::string GameFilesHelp() {
    return Tr("Select your own disc image (.iso). Supported: Trespasser UK 1.0.116. It is read directly: nothing is "
              "mounted and no setup program runs. The game is copied into the game folder next to the program (about "
              "600 MB) and checked before it replaces anything. No game files are included. Reinstalling keeps your "
              "saves and settings.");
}

std::string StatusDetail(const GameCheck& check) {
    switch (check.state) {
    case GameState::kReady:
        return std::string(check.profile->executable) + "  \xC2\xB7  SHA-256 " + check.executable_sha256.substr(0, 16) +
               "\xE2\x80\xA6";
    case GameState::kMissing:
        return Tr("Install the game from your Trespasser disc image.");
    default:
        return check.detail;
    }
}

enum class Outcome { kRunning, kPlay, kQuit };

class LauncherApp {
public:
    LauncherApp(const jpt_launcher_config_t& config, LauncherSettings& settings, GameCheck check)
        : config_(config),
          settings_(settings),
          game_dir_(PathFromUtf8(config.game_dir)),
          user_dir_(PathFromUtf8(config.user_dir)),
          registry_(user_dir_ / JPT_REGISTRY_FILE),
          check_(std::move(check)) {}

    // Runs the launcher window until Play or Quit. False when no window could be opened.
    bool Run(Outcome& outcome) {
        HostOptions options;
        options.title = kGameTitle;
        const WindowPlacement saved = ReadPlacement(settings_);
        if (PlacementIsSet(saved)) {
            options.restore = saved;
        }
        if (!host_.Open(options)) {
            return false;
        }
        monitors_ = ListMonitors();
        gpu_ = QueryGpu();
        Log("graphics card \"%s\", MSAA %s%s%s, %zu monitor(s)", gpu_.name.empty() ? "unknown" : gpu_.name.c_str(),
            gpu_.msaa_mask & 4 ? "2x " : "", gpu_.msaa_mask & 16 ? "4x " : "", gpu_.msaa_mask & 256 ? "8x" : "",
            monitors_.size());
        ReadGameplay();
        placement_ = PlacementTracker(host_.Restored());
        fonts_ = trg::LoadSystemFonts(trg::layout::kTextBody);
        if (LoadLauncherImage("header-art.png", header_art_image_)) {
            header_art_aspect_ = float(header_art_image_.width) / float(header_art_image_.height);
        }
        sponsor_icon_ = LoadSponsorIcon(host_);
        trg::SetTranslator(&TranslateLauncherText);
        launcher_ = std::make_unique<trg::Launcher>(MakeConfig());

        outcome = Outcome::kRunning;
        host_.BeginFrame();
        outcome = Frame();
        host_.EndFrame(LauncherTheme().bg);
        host_.Show();
        Log("shown");
        while (outcome == Outcome::kRunning) {
            if (!host_.PumpEvents()) {
                outcome = Outcome::kQuit;
                break;
            }
            if (host_.Minimized()) {
                SDL_Delay(30);
                continue;
            }
            host_.BeginFrame();
            outcome = Frame();
            host_.EndFrame(LauncherTheme().bg);
        }
        if (install_.running()) {
            install_.Cancel();
            install_.Finish();
        }
        SavePlacement(placement_.Flush());
        launcher_.reset();
        host_.Close();
        Log(outcome == Outcome::kPlay ? "closed, starting the game" : "closed");
        return true;
    }

private:
    bool Ready() const { return check_.state == GameState::kReady; }

    Outcome Frame() {
        TrackPlacement();
        PollInstall();
        if (play_.starting() && ImGui::GetTime() - play_started_ >= kPlayFeedbackSeconds) {
            return Outcome::kPlay;
        }
        ImGuiIO& io = ImGui::GetIO();
        if (play_.starting()) {
            io.ClearInputKeys();
            io.ClearInputMouse();
        }
        ImGui::BeginDisabled(play_.starting());
        const trg::Result result = launcher_->Frame();
        ImGui::EndDisabled();
        if (play_.starting()) {
            DrawStartingMessage();
        }
        switch (result) {
        case trg::Result::kPlay:
            RequestPlay();
            break;
        case trg::Result::kQuit:
            if (!play_.starting() && LauncherQuitAllowed(ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
                return Outcome::kQuit;
            }
            break;
        case trg::Result::kNone:
            break;
        }
        return Outcome::kRunning;
    }

    void Recheck() {
        check_ = CheckGameFolder(game_dir_);
        Log("game files: %s%s%s", GameStateTitle(check_.state), check_.detail.empty() ? "" : ": ", check_.detail.c_str());
    }

    // PLAY saves the settings once here, then starts the transition. A failed save keeps the launcher open.
    void RequestPlay() {
        if (play_.starting()) {
            return;
        }
        Recheck();
        switch (play_.Request(install_.running(), Ready(), [this] { return launcher_->Save(); })) {
        case PlayRequest::kStarting:
            play_started_ = ImGui::GetTime();
            launcher_->SetStatus("", 0.0);
            Log("settings saved to %s, starting the game", Utf8(settings_.file()).c_str());
            break;
        case PlayRequest::kFilesNotReady:
            launcher_->SetStatus(std::string(GameStateTitle(check_.state)) + Tr(". Install the game files first."), 6.0);
            break;
        case PlayRequest::kBusy:
            launcher_->SetStatus("Wait for the installation to finish.", 6.0);
            break;
        case PlayRequest::kSaveFailed:
            Log("could not save the settings");
            break;
        }
    }

    void TrackPlacement() {
        const auto state = host_.State();
        if (!state) {
            return;
        }
        placement_.Observe(*state, host_.Fullscreen(), ImGui::GetTime());
        SavePlacement(placement_.TakeSettled(ImGui::GetTime()));
    }

    void SavePlacement(const std::optional<WindowPlacement>& placement) {
        if (placement && !settings_.SaveNow(PlacementValues(*placement))) {
            Log("could not save the launcher window placement");
        }
    }

    void DrawStartingMessage() {
        const char* text = Tr("Settings saved. Starting game...");
        ImGui::PushFont(fonts_.semibold, fonts_.size * 1.5f);
        const float size = ImGui::GetFontSize();
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 pos(viewport->Pos.x + (viewport->Size.x - text_size.x) * 0.5f,
                         viewport->Pos.y + (viewport->Size.y - text_size.y) * 0.5f);
        ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), size, pos, ImGui::GetColorU32(kAccent), text);
        ImGui::PopFont();
    }

    trg::LauncherConfig MakeConfig() {
        trg::LauncherConfig c;
        c.settings = &settings_;
        c.save_on_play = false;
        c.theme = LauncherTheme();
        c.hints = "Enter  Play     Alt+F4  Quit     Ctrl+S  Save";
        c.fonts = fonts_;
        c.branding.title = "JURASSIC PARK: TRESPASSER";
        c.branding.fade_into_page = false;
        c.branding.subtitle_color = kAccent;
        c.branding.subtitle_segments = {
            {"RECOMPILED   \xC2\xB7   ", ""},
            {"v" JPT_PORT_VERSION "   \xC2\xB7   ", ""},
            {"BY SAULOB", kAuthorUrl},
        };
        c.branding.background = LauncherHeader([this](int width, int height) { return HeaderArtTexture(width, height); },
                                               header_art_aspect_);
        c.sponsor = {"Sponsor on GitHub", kSponsorUrl, sponsor_icon_};
        c.note = "Play applies the changes. Save keeps them without starting.";
        for (const PageId page : kPageOrder) {
            c.pages.push_back(MakePage(page));
            c.pages.back().icon = [this, page](int pixels) { return PageIconTexture(page, pixels); };
        }
        c.can_play = [this] {
            if (install_.running()) {
                return trg::PlayCheck{false, "Wait for the installation to finish.", 0};
            }
            if (!Ready()) {
                return trg::PlayCheck{false, "The game files are not ready: see the Play page.", 0};
            }
            return trg::PlayCheck{};
        };
        c.on_save = [this] { return SaveGameRegistry(); };
        c.on_reset = [this] {
            gore_ = 1;
            invert_ = false;
            gore_dirty_ = invert_dirty_ = true;
        };
        return c;
    }

    ImTextureID HeaderArtTexture(int width, int height) {
        if (width != header_art_width_ || height != header_art_height_) {
            header_art_width_ = width;
            header_art_height_ = height;
            host_.DestroyTexture(header_art_);
            const std::vector<uint8_t> rgba = ScaleArtRgba(header_art_image_.rgba.data(), header_art_image_.width,
                                                           header_art_image_.height, width, height);
            header_art_ = rgba.empty() ? ImTextureID{} : host_.CreateTexture(rgba.data(), width, height);
        }
        return header_art_;
    }

    ImTextureID PageIconTexture(PageId page, int pixels) {
        const auto [icon, is_new] = page_icons_.try_emplace({page, pixels});
        if (is_new) {
            const std::vector<uint8_t> rgba = RenderPageIcon(page, pixels);
            if (!rgba.empty()) {
                icon->second = host_.CreateTexture(rgba.data(), pixels, pixels);
            }
            if (icon->second == ImTextureID{}) {
                Log("sidebar icon unavailable: page %d at %d px", static_cast<int>(page), pixels);
            }
        }
        return icon->second;
    }

    trg::Page MakePage(PageId page) {
        const std::string name(PageName(page));
        switch (page) {
        case PageId::kPlay:
            return {name, "Install the game from your disc, and start it.", [this](trg::Ui& ui) { PagePlay(ui); },
                    [this] { return !Ready(); }, nullptr};
        case PageId::kDisplay:
            return {name, "Window, monitor, aspect ratio and presentation.", [this](trg::Ui& ui) { PageDisplay(ui); },
                    nullptr, nullptr};
        case PageId::kGraphics:
            return {name, "Renderer, resolution, anti-aliasing and texture filtering.",
                    [this](trg::Ui& ui) { PageGraphics(ui); }, nullptr, nullptr};
        case PageId::kGameplay:
            return {name, "The game's own options and frame rate readouts.", [this](trg::Ui& ui) { PageGameplay(ui); },
                    nullptr, nullptr};
        case PageId::kControls:
            return {name, "Keyboard and mouse.", [this](trg::Ui& ui) { PageControls(ui); }, nullptr, nullptr};
        case PageId::kCheats:
            return {name, "Coming later.", [this](trg::Ui& ui) { PageComingLater(ui, "Cheats"); }, nullptr, nullptr};
        case PageId::kBugReport:
            return {name, "Coming later.", [this](trg::Ui& ui) { PageComingLater(ui, "Bug reports"); }, nullptr,
                    nullptr};
        case PageId::kAbout:
            return {name, "This port, and where your files and settings live.", [this](trg::Ui& ui) { PageAbout(ui); },
                    nullptr, nullptr};
        }
        return {};
    }

    // ---- Play

    void PagePlay(trg::Ui& ui) {
        const bool ready = Ready();
        const std::string title = ready ? std::string(GameStateTitle(check_.state)) + " \xC2\xB7 " + check_.profile->label
                                        : GameStateTitle(check_.state);
        const std::string detail = StatusDetail(check_);
        ui.StatusCard(ready ? trg::Status::kReady : trg::Status::kAttention, title.c_str(), detail.c_str());
        if (!install_failure_.empty()) {
            ui.Spacer(4);
            ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
            ImGui::TextUnformatted(Tr(install_failure_.c_str()));
            ImGui::PopStyleColor();
            ui.Help(install_failure_text_.c_str());
            ui.Spacer(4);
        }
        ui.Row("Game files");
        if (!ui.TaskProgress(install_)) {
            const bool clicked = ready ? ImGui::Button(Tr("Reinstall game..."), ImVec2(-FLT_MIN, 0))
                                       : ui.AccentButton("Install game...");
            if (clicked) {
                BrowseAndInstall();
            }
        }
        ui.EndRows();
        ui.Help(GameFilesHelp().c_str());
        ui.Toggle("Show this launcher", kShowLauncherHelp, JPT_KEY_SHOW_LAUNCHER, true, "Off", "On");
        ui.EndRows();
    }

    void BrowseAndInstall() {
        if (install_.running()) {
            return;
        }
        StartInstall(trg::BrowseForFile(Tr("Select your Trespasser disc image"), {{Tr("Disc images (*.iso)"), "*.iso"}}));
    }

    void StartInstall(const std::string& picked) {
        if (picked.empty() || install_.running() || play_.starting()) {
            return;
        }
        install_failure_.clear();
        install_failure_text_.clear();
        outcome_ = std::make_shared<InstallResult>();
        const bool started = install_.Start(
            "Reading the disc image...",
            [outcome = outcome_, iso = PathFromUtf8(picked), game = game_dir_, user = user_dir_](trg::Task& task) {
                InstallHooks hooks;
                hooks.phase = [&task](const char* text) {
                    task.SetLabel(text);
                    task.Progress(0, 0);
                };
                hooks.progress = [&task](uint64_t done, uint64_t total) {
                    task.Progress(static_cast<long long>(done), static_cast<long long>(total));
                    return !task.cancelled();
                };
                hooks.log = [](const std::string& line) { Log("%s", line.c_str()); };
                *outcome = InstallFromIso(iso, game, user, hooks);
                return outcome->ok() ? std::string() : std::string(InstallStatusTitle(outcome->status));
            });
        if (started) {
            Log("installing the game from %s into %s", picked.c_str(), Utf8(game_dir_).c_str());
        }
    }

    // Runs every frame so a finished install is picked up whichever page is open.
    void PollInstall() {
        if (install_.Finish() == trg::Task::State::kIdle) {
            return;
        }
        const std::shared_ptr<InstallResult> outcome = std::move(outcome_);
        if (!outcome) {
            return;
        }
        Recheck();
        if (outcome->ok()) {
            Log("game installed: %zu files, %llu bytes, %s", outcome->files,
                static_cast<unsigned long long>(outcome->bytes), outcome->detail.c_str());
            launcher_->SetStatus(Tr("Game installed. Ready to play."), 6.0);
            return;
        }
        if (outcome->status == InstallStatus::kCancelled) {
            Log("game install cancelled");
            launcher_->SetStatus(Tr("Installation cancelled. Nothing was changed."), 6.0);
            return;
        }
        Log("game install failed: %s (%s)", InstallStatusTitle(outcome->status), outcome->detail.c_str());
        install_failure_ = InstallStatusTitle(outcome->status);
        install_failure_text_ = outcome->detail + Tr(". The current installation was not changed.");
        launcher_->SetStatus(Tr("Install failed: ") + install_failure_, 8.0);
    }

    // ---- Display

    const jpt_monitor_t* SelectedMonitor() const {
        if (monitors_.empty()) {
            return nullptr;
        }
        return &monitors_[size_t(jpt_monitor_index(settings_.GetInt(JPT_KEY_MONITOR, 1), int(monitors_.size())))];
    }

    void PageDisplay(trg::Ui& ui) {
        ui.Choice("Display mode", "Borderless Fullscreen fills the monitor. F11 switches while playing.",
                  JPT_KEY_DISPLAY_MODE, "windowed", {{"windowed", "Windowed"}, {"borderless", "Borderless Fullscreen"}},
                  2);
        ui.Row("Window size", kWindowSizeHelp);
        WindowSizeCombo();
        ui.Row("Monitor", kMonitorHelp);
        MonitorCombo();
        std::vector<trg::Option> aspects;
        for (const jpt_aspect_t& aspect : jpt_aspects) {
            aspects.push_back({aspect.key, Tr(aspect.label)});
        }
        ui.Combo("Aspect ratio", kAspectRatioHelp, JPT_KEY_ASPECT_RATIO, "16:9", aspects);
        ui.Choice("Presentation", kPresentationHelp, JPT_KEY_PRESENTATION, "letterbox",
                  {{"letterbox", "Letterbox"}, {"stretch", "Stretch"}}, 2);
        ui.EndRows();
    }

    void WindowSizeCombo() {
        const std::string current = settings_.Get(JPT_KEY_WINDOW_SIZE);
        const jpt_window_size_t* selected = &jpt_window_sizes[0];
        for (const jpt_window_size_t& size : jpt_window_sizes) {
            if (current == size.key) {
                selected = &size;
            }
        }
        const jpt_monitor_t* monitor = SelectedMonitor();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##window_size", WindowSizeLabel(*selected).c_str())) {
            for (const jpt_window_size_t& size : jpt_window_sizes) {
                const bool is_selected = &size == selected;
                if (!is_selected && monitor && !WindowSizeFits(size, *monitor)) {
                    continue;
                }
                if (ImGui::Selectable(WindowSizeLabel(size).c_str(), is_selected) && !is_selected) {
                    settings_.Set(JPT_KEY_WINDOW_SIZE, size.key);
                }
            }
            ImGui::EndCombo();
        }
    }

    void MonitorCombo() {
        const int current = settings_.GetInt(JPT_KEY_MONITOR, 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##monitor", MonitorLabel(current, monitors_).c_str())) {
            for (int i = 1; i <= static_cast<int>(monitors_.size()); ++i) {
                if (ImGui::Selectable(MonitorLabel(i, monitors_).c_str(), i == current) && i != current) {
                    settings_.SetInt(JPT_KEY_MONITOR, i);
                }
            }
            ImGui::EndCombo();
        }
    }

    // ---- Graphics

    jpt_size_t MonitorSize() const {
        const jpt_monitor_t* monitor = SelectedMonitor();
        if (!monitor) {
            return {0, 0};
        }
        return {monitor->bounds.right - monitor->bounds.left, monitor->bounds.bottom - monitor->bounds.top};
    }

    void PageGraphics(trg::Ui& ui) {
        ui.Combo("Renderer", kRendererHelp, JPT_KEY_RENDERER, "d3d11",
                 {{"d3d11", "Direct3D 11"}, {"software", "Software"}});
        const int aspect = jpt_aspect_index(settings_.Get(JPT_KEY_ASPECT_RATIO).c_str());
        const jpt_size_t monitor = MonitorSize();
        std::vector<trg::Option> resolutions;
        for (int preset = 0; preset < JPT_RENDER_PRESET_COUNT; ++preset) {
            const jpt_size_t box = jpt_render_box(preset, aspect, monitor.width, monitor.height);
            resolutions.push_back({jpt_render_presets[preset].key, ResolutionLabel(preset, box)});
        }
        ui.Combo("Render resolution", kRenderResolutionHelp, JPT_KEY_RENDER_RESOLUTION, "native", resolutions);
        const int preset = jpt_render_preset_index(settings_.Get(JPT_KEY_RENDER_RESOLUTION).c_str());
        const jpt_size_t box = jpt_render_box(preset < 0 ? JPT_RENDER_NATIVE : preset, aspect, monitor.width,
                                              monitor.height);
        ui.Info("Render size", RenderSizeText(box, aspect).c_str());
        AntiAliasingRow(ui);
        ui.Combo("Texture filtering", kTextureFilteringHelp, JPT_KEY_TEXTURE_FILTER, "anisotropic16",
                 {{"original", "Original"},
                  {"bilinear", "Bilinear"},
                  {"anisotropic4", "Anisotropic 4x"},
                  {"anisotropic8", "Anisotropic 8x"},
                  {"anisotropic16", "Anisotropic 16x"}});
        ui.Toggle("VSync", kVSyncHelp, JPT_KEY_VSYNC, true, "Off", "On");
        ui.Info("Graphics card", gpu_.name.empty() ? Tr("Not detected") : gpu_.name.c_str());
        ui.EndRows();
    }

    void AntiAliasingRow(trg::Ui& ui) {
        std::vector<trg::Option> options{{"0", "Off"}};
        for (unsigned samples = 2; samples <= 8; samples *= 2) {
            if (gpu_.msaa_mask & (1u << samples)) {
                options.push_back({std::to_string(samples), std::to_string(samples) + "x MSAA"});
            }
        }
        const std::string current = settings_.Get(JPT_KEY_MSAA);
        const bool listed = current == "1" || std::any_of(options.begin(), options.end(),
                                                          [&current](const trg::Option& o) { return o.value == current; });
        std::string help = kAntiAliasingHelp;
        if (!listed) {
            help += Tr(" The saved ") + current + Tr("x is not supported here: choose another level.");
        }
        ui.Choice("Anti-aliasing", help.c_str(), JPT_KEY_MSAA, "4", options, 4);
    }

    // ---- Gameplay

    void ReadGameplay() {
        unsigned long value = 0;
        // The game reads Gore with default 1 and clamps it to 0..2 (0x0040BA01).
        gore_ = 1;
        if (registry_.GetDword("Gore", value)) {
            const long level = static_cast<long>(value);
            gore_ = level < 0 ? 0 : level > 2 ? 2 : static_cast<int>(level);
        }
        invert_ = registry_.GetDword("InvertMouse", value) && value != 0;
    }

    bool SaveGameRegistry() {
        if (gore_dirty_ && registry_.SetDword("Gore", static_cast<unsigned long>(gore_))) {
            gore_dirty_ = false;
        }
        if (invert_dirty_ && registry_.SetDword("InvertMouse", invert_ ? 1 : 0)) {
            invert_dirty_ = false;
        }
        return !gore_dirty_ && !invert_dirty_;
    }

    void PageGameplay(trg::Ui& ui) {
        ui.Row("Gore", "The game's own gore level, 0 to 2 (1 is its default); its options menu changes it too.");
        const int level = ui.Segmented("##gore", {"0", "1", "2"}, gore_);
        if (level >= 0 && level != gore_) {
            gore_ = level;
            gore_dirty_ = true;
        }
        ui.Row("Invert mouse", "Mouse look up and down inverted, the game's own option.");
        const int invert = ui.Segmented("##invert_mouse", {"Off", "On"}, invert_ ? 1 : 0);
        if (invert >= 0 && (invert == 1) != invert_) {
            invert_ = invert == 1;
            invert_dirty_ = true;
        }
        ui.Toggle("Frame rate in title", kShowFpsTitleHelp, JPT_KEY_SHOW_FPS, true, "Off", "On");
        ui.EndRows();
    }

    // ---- Controls

    void PageControls(trg::Ui& ui) {
        ui.Info("Keyboard and mouse", "The game's own bindings, set in its Options menu.");
        if (ui.Section("Port hotkeys", true)) {
            ui.Help("Rebinding the hotkeys is not available yet.");
            ui.Info("Hint", "F1");
            ui.Info("Renderer: Direct3D 11 / Software", "F5");
            ui.Info("Windowed / Borderless Fullscreen", "F11");
            ui.Info("The game's original cheat input", "Ctrl+F11");
            ui.Info("Controls", "F12");
            ui.Info("Menu", "Esc");
            ui.Info("Exit", "Alt+F4");
            ui.EndRows();
        }
    }

    void PageComingLater(trg::Ui& ui, const char* what) {
        ui.Paragraph((std::string(Tr(what)) + Tr(" are coming later. Nothing on this page is active, and nothing is sent "
                                                "anywhere.")).c_str());
    }

    // ---- About

    void PageAbout(trg::Ui& ui) {
        ui.SponsorCard(
            "This open source PC port is maintained and developed in my free time. If you enjoy the project and would "
            "like to support its continued development, you can sponsor my work on GitHub.",
            "Support is completely optional and helps support ongoing improvements and future projects.");
        ui.Paragraph("Jurassic Park: Trespasser Recompiled. PC port powered by static recompilation: the 1998 game "
                     "runs as native code on modern Windows, with a Direct3D 11 renderer. No game files are included; "
                     "the game runs from the player's own disc. Port by saulob, recompiled with pcrecomp by sp00nz. "
                     "Original game by DreamWorks Interactive.");
        ui.Spacer(4);
        std::error_code ec;
        if (std::filesystem::is_directory(game_dir_, ec)) {
            ui.FolderRow("Game files", "Where the installed game is read from.", "Open game folder", Utf8(game_dir_));
        }
        std::filesystem::create_directories(user_dir_, ec);
        ui.FolderRow("Saves and settings", "Settings, saves and the game's own options.", "Open user folder",
                     Utf8(user_dir_));
        if (ui.ButtonRow("Settings file", "Launcher choices stored as plain text.", "Show settings file") &&
            launcher_->Save()) {
            trg::OpenInFileManager(Utf8(std::filesystem::exists(settings_.file(), ec) ? settings_.file() : user_dir_));
        }
        if (ui.ConfirmRow("Reset settings", "Put the launcher and display settings back to their defaults.",
                          "Reset all settings", "Put every setting back to its default?", "Reset")) {
            settings_.ResetToDefaults();
            launcher_->SetStatus("Settings reset to defaults. Save or press Play to keep them.", 6.0);
            Log("settings reset to defaults (pending)");
        }
        ui.Heading("Diagnostics");
        if (ui.ButtonRow("Logs folder", "Open the folder containing session logs", "Open logs folder")) {
            OpenLogsFolder();
        }
        const std::string renderer = settings_.Get(JPT_KEY_RENDERER) == "software" ? "Software" : "Direct3D 11";
        ui.InfoGrid({{"Port version", JPT_PORT_VERSION},
                     {"Game", Ready() ? check_.profile->label : GameStateTitle(check_.state)},
                     {"Renderer", renderer},
                     {"Platform", "Windows x86"}});
        char libraries[96];
        std::snprintf(libraries, sizeof(libraries), "SDL %d.%d.%d  \xC2\xB7  Dear ImGui %s  \xC2\xB7  TRG Launcher %s",
                      SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                      SDL_VERSIONNUM_MICRO(SDL_GetVersion()), IMGUI_VERSION, JPT_TRG_LAUNCHER_VERSION);
        ui.Info("Libraries", libraries);
        ui.Link(kPortRepository, kPortUrl);
    }

    void OpenLogsFolder() {
        if (!config_.logs_dir || !*config_.logs_dir) {
            Log("could not open the logs folder: no logs directory is configured");
            launcher_->SetStatus("Could not open the logs folder.", 6.0);
            return;
        }
        std::error_code ec;
        const std::filesystem::path folder = PathFromUtf8(config_.logs_dir);
        std::filesystem::create_directories(folder, ec);
        trg::OpenInFileManager(Utf8(folder));
    }

    const jpt_launcher_config_t& config_;
    LauncherSettings& settings_;
    std::filesystem::path game_dir_, user_dir_;
    GameRegistry registry_;
    GameCheck check_;
    LauncherHost host_;
    Image header_art_image_;
    float header_art_aspect_ = kHeaderArtAspect;
    ImTextureID header_art_{};
    int header_art_width_ = 0;
    int header_art_height_ = 0;
    ImTextureID sponsor_icon_{};
    std::map<std::pair<PageId, int>, ImTextureID> page_icons_;
    std::unique_ptr<trg::Launcher> launcher_;
    trg::Fonts fonts_;
    PlacementTracker placement_;
    trg::Task install_;
    std::shared_ptr<InstallResult> outcome_;
    std::string install_failure_, install_failure_text_;
    std::vector<jpt_monitor_t> monitors_;
    GpuInfo gpu_;
    PlayGate play_;
    double play_started_ = 0.0;
    int gore_ = 1;
    bool invert_ = false, gore_dirty_ = false, invert_dirty_ = false;
};

} // namespace
} // namespace jpt

extern "C" jpt_launcher_result_t jpt_launcher_run(const jpt_launcher_config_t* config) {
    using namespace jpt;
    SetLauncherLog(config->log);
    LauncherSettings settings(PathFromUtf8(config->settings_file));
    GameCheck check = CheckGameFolder(PathFromUtf8(config->game_dir));
    Log("game files: %s%s%s", GameStateTitle(check.state), check.detail.empty() ? "" : ": ", check.detail.c_str());
    const bool ready = check.state == GameState::kReady;
    const bool show_setting = settings.GetBool(JPT_KEY_SHOW_LAUNCHER, true);
    const bool shift = trg::ShiftHeld() || config->force_show != 0;
    if (!LauncherWanted(ready, show_setting, shift, config->skip_once != 0)) {
        Log("skipped (show_launcher %s%s)", show_setting ? "on" : "off", config->skip_once ? ", skip requested" : "");
        return JPT_LAUNCHER_PLAY;
    }
    Log("settings file %s", Utf8(settings.file()).c_str());
    Log("opening: %s%s%s", ready ? "game ready" : "game files not ready", show_setting ? ", show_launcher on" : "",
        shift ? ", Shift held or --launcher" : "");
    LauncherApp app(*config, settings, std::move(check));
    Outcome outcome = Outcome::kRunning;
    if (!app.Run(outcome)) {
        Log("could not open its window; %s", ready ? "starting the game" : "game files are not ready");
        return ready ? JPT_LAUNCHER_PLAY : JPT_LAUNCHER_QUIT;
    }
    return outcome == Outcome::kPlay ? JPT_LAUNCHER_PLAY : JPT_LAUNCHER_QUIT;
}

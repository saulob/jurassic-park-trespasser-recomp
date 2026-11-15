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
#include "jpt_gamepad.h"
#include "jpt_l10n.h"
#include "jpt_prompt_text.h"
#include "launcher_log.h"
#include "launcher_quit.h"
#include "localization/ui_text.h"
#include "options.h"
#include "page_icons.h"
#include "placement.h"
#include "play_gate.h"
#include "settings.h"
#include "system_info.h"
#include "user_data.h"
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
    "rasteriser at its original size.";
constexpr char kGraphicsCardHelp[] =
    "Selects which graphics card renders the game. If the selected card is no longer available, the first available "
    "card is used.";
constexpr char kRenderResolutionHelp[] =
    "The size the 3D world is really drawn at, with its own depth buffer and anti-aliasing; it follows the Aspect ratio "
    "on the Display page. Native uses the monitor's size. Original keeps the earlier compatibility size.";
constexpr char kAntiAliasingHelp[] =
    "Multisample anti-aliasing of the 3D world. Only the levels this graphics card supports are listed.";
constexpr char kTextureFilteringHelp[] =
    "Sharper, steadier textures at a distance and on the ground seen at an angle, with mipmaps generated for the 3D "
    "world. Original uses the game's own filtering without them; Bilinear smooths every texture.";
constexpr char kAmbientOcclusionHelp[] =
    "Soft shadows where the 3D world's surfaces meet: trees and rocks on the ground, corners and walls (Intel ASSAO). "
    "Higher levels look smoother and cost more. Direct3D 11 only; F5's Debug Menu changes it while playing.";
constexpr char kVSyncHelp[] = "On prevents tearing. Off allows it, for the lowest latency and G-Sync/FreeSync.";
constexpr char kShowFpsTitleHelp[] = "Shows the frame rate in the window title.";
constexpr char kShowResolutionTitleHelp[] = "Shows the game's render resolution in the window title.";
constexpr char kSkipIntroHelp[] =
    "Skips the opening video before the main menu and the one after New Game. The credits and the ending still play.";
constexpr char kInvertMouseHelp[] = "Mouse look up and down inverted, the game's own option.";
constexpr char kKeyboardHelp[] =
    "The game's own bindings as it loads them, changed in its Controls screen (Options > Controls). They keep "
    "working while a gamepad is connected.";
constexpr char kGamepadHelp[] =
    "Xbox, PlayStation, Switch Pro, Steam Deck and other controllers SDL3 recognises work without setup, at the same "
    "time as the keyboard and mouse. The tutorial's instructions show the buttons of the device you used last. "
    "Controllers can be connected or disconnected at any time.";
constexpr char kGlyphsHelp[] =
    "The button pictures the game shows. Automatic follows the controller in use; the others always show that style.";
constexpr char kLookHelp[] =
    "How fast the right stick turns the view, in percent of the default speed. The same stick moves Anne's hand while "
    "Move Hand is held, a little slower.";
constexpr char kInvertLookHelp[] = "Right stick up looks down. The game's Invert mouse setting stays for the mouse.";
constexpr char kStickDeadzoneHelp[] =
    "How far a stick moves before it counts. Raise it if the view or Anne drifts with the stick at rest.";
constexpr char kTriggerDeadzoneHelp[] = "How far a trigger is pressed before it counts.";
constexpr char kGuideHelp[] =
    "Hold Move Hand to raise Anne's hand; the right stick then moves it instead of the view, as the mouse does with "
    "the left button. Rotate Wrist and Rotate Arm work with Move Hand held. In a level, View (F2 on the keyboard) shows this list.";
constexpr char kLanguageHelp[] = "Changes the game text, menus and subtitles. Voices, effects and music remain in English.";
constexpr char kBindingHelp[] =
    "Choosing a button another action uses gives that action this one's button. Start always opens the pause menu, "
    "and View/Back is kept free.";

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
        return TrMessage(check.detail);
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
          saves_dir_(PathFromUtf8(config.saves_dir)),
          registry_(saves_dir_ / JPT_REGISTRY_FILE),
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
        ListGraphicsCards();
        Log("%zu monitor(s)", monitors_.size());
        ReadGameRegistry();
        SetLauncherLanguage(settings_.Get(JPT_KEY_LANGUAGE));
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
        SetLauncherLanguage(settings_.Get(JPT_KEY_LANGUAGE));
        NormalizeGraphicsCard();
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
            launcher_->SetStatus(std::string(Tr(GameStateTitle(check_.state))) + Tr(". Install the game files first."), 6.0);
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
            invert_ = false;
            invert_dirty_ = true;
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
            return {name, "Renderer, graphics card, resolution, anti-aliasing and texture filtering.",
                    [this](trg::Ui& ui) { PageGraphics(ui); }, nullptr, nullptr};
        case PageId::kGameplay:
            return {name, "Language, Skip Intro and what the window title shows.",
                    [this](trg::Ui& ui) { PageGameplay(ui); }, nullptr, nullptr};
        case PageId::kControls:
            return {name, "Keyboard, mouse and gamepad.", [this](trg::Ui& ui) { PageControls(ui); }, nullptr, nullptr};
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
        const std::string title = ready ? std::string(Tr(GameStateTitle(check_.state))) + " \xC2\xB7 " + check_.profile->label
                                        : std::string(Tr(GameStateTitle(check_.state)));
        const std::string detail = StatusDetail(check_);
        ui.StatusCard(ready ? trg::Status::kReady : trg::Status::kAttention, title.c_str(), detail.c_str());
        if (!install_failure_.empty()) {
            ui.Spacer(4);
            ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
            ImGui::TextUnformatted(Tr(install_failure_.c_str()));
            ImGui::PopStyleColor();
            ui.Help((TrMessage(install_failure_text_) + Tr(". The current installation was not changed.")).c_str());
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
            [outcome = outcome_, iso = PathFromUtf8(picked), game = game_dir_, user = saves_dir_](trg::Task& task) {
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
        install_failure_text_ = outcome->detail;
        launcher_->SetStatus(Tr("Install failed: ") + Tr(install_failure_), 8.0);
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

    void ListGraphicsCards() {
        gpus_ = ListGpus();
        gpu_names_ = GpuChoices(gpus_);
        gpu_msaa_.assign(gpus_.size(), 0);
        for (size_t i = 0; i < gpus_.size(); ++i) {
            if (!gpus_[i].name[0]) {
                continue;
            }
            gpu_msaa_[i] = GpuMsaaMask(static_cast<int>(i));
            Log("graphics card %zu \"%s\", MSAA %s%s%s", i, gpus_[i].label, gpu_msaa_[i] & 4 ? "2x " : "",
                gpu_msaa_[i] & 16 ? "4x " : "", gpu_msaa_[i] & 256 ? "8x" : "");
        }
        if (gpu_names_.empty()) {
            Log("no graphics card found");
        }
    }

    // The adapter the game will use for the saved choice (the renderer resolves it the same way).
    int ChosenGpu() const {
        return jpt_gpu_resolve(gpus_.data(), static_cast<int>(gpus_.size()), settings_.Get(JPT_KEY_GRAPHICS_CARD).c_str());
    }

    void NormalizeGraphicsCard() {
        const int found = ChosenGpu();
        if (found >= 0 && settings_.Get(JPT_KEY_GRAPHICS_CARD) != gpus_[size_t(found)].label) {
            settings_.Set(JPT_KEY_GRAPHICS_CARD, gpus_[size_t(found)].label);
        }
    }

    void GraphicsCardCombo() {
        if (gpu_names_.empty()) {
            ImGui::TextUnformatted(Tr("No graphics card found"));
            return;
        }
        const std::string current = settings_.Get(JPT_KEY_GRAPHICS_CARD);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##graphics_card", current.c_str())) {
            for (const std::string& name : gpu_names_) {
                if (ImGui::Selectable(name.c_str(), name == current) && name != current) {
                    settings_.Set(JPT_KEY_GRAPHICS_CARD, name);
                }
            }
            ImGui::EndCombo();
        }
    }

    void PageGraphics(trg::Ui& ui) {
        ui.Combo("Renderer", kRendererHelp, JPT_KEY_RENDERER, "d3d11",
                 {{"d3d11", "Direct3D 11"}, {"software", "Software"}});
        ui.Row("Graphics card", kGraphicsCardHelp);
        GraphicsCardCombo();
        const int aspect = jpt_aspect_index(settings_.Get(JPT_KEY_ASPECT_RATIO).c_str());
        const jpt_size_t monitor = MonitorSize();
        std::vector<trg::Option> resolutions;
        for (int preset = 0; preset < JPT_RENDER_PRESET_COUNT; ++preset) {
            const jpt_size_t box = jpt_render_box(preset, aspect, monitor.width, monitor.height);
            resolutions.push_back({jpt_render_presets[preset].key, ResolutionLabel(preset, box)});
        }
        ui.Combo("Render resolution", kRenderResolutionHelp, JPT_KEY_RENDER_RESOLUTION, "native", resolutions);
        AntiAliasingRow(ui);
        ui.Combo("Ambient occlusion", kAmbientOcclusionHelp, JPT_KEY_AMBIENT_OCCLUSION, "off",
                 {{"off", "Off"}, {"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"ultra", "Ultra"}});
        ui.Combo("Texture filtering", kTextureFilteringHelp, JPT_KEY_TEXTURE_FILTER, "anisotropic16",
                 {{"original", "Original"},
                  {"bilinear", "Bilinear"},
                  {"anisotropic4", "Anisotropic 4x"},
                  {"anisotropic8", "Anisotropic 8x"},
                  {"anisotropic16", "Anisotropic 16x"}});
        ui.Toggle("VSync", kVSyncHelp, JPT_KEY_VSYNC, true, "Off", "On");
        ui.EndRows();
    }

    void AntiAliasingRow(trg::Ui& ui) {
        const int gpu = ChosenGpu();
        const unsigned msaa = gpu >= 0 ? gpu_msaa_[size_t(gpu)] : 0;
        std::vector<trg::Option> options{{"0", "Off"}};
        for (unsigned samples = 2; samples <= 8; samples *= 2) {
            if (msaa & (1u << samples)) {
                options.push_back({std::to_string(samples), std::to_string(samples) + "x MSAA"});
            }
        }
        const std::string current = settings_.Get(JPT_KEY_MSAA);
        const bool listed = current == "1" || std::any_of(options.begin(), options.end(),
                                                          [&current](const trg::Option& o) { return o.value == current; });
        std::string help = Tr(kAntiAliasingHelp);
        if (!listed) {
            help += " " + TrFormat("The saved %sx is not supported here: choose another level.", current.c_str());
        }
        ui.Choice("Anti-aliasing", help.c_str(), JPT_KEY_MSAA, "4", options, 4);
    }

    // ---- Gameplay

    void PageGameplay(trg::Ui& ui) {
        std::vector<trg::Option> languages;
        for (const jpt_l10n_language_t& language : jpt_l10n_languages) {
            languages.push_back({language.code, language.name});
        }
        ui.Combo("Language", kLanguageHelp, JPT_KEY_LANGUAGE, JPT_L10N_ENGLISH, languages);
        ui.Toggle("Skip Intro", kSkipIntroHelp, JPT_KEY_SKIP_INTRO, false, "Off", "On");
        ui.Toggle("Frame rate in title", kShowFpsTitleHelp, JPT_KEY_SHOW_FPS, true, "Off", "On");
        ui.Toggle("Render resolution in title", kShowResolutionTitleHelp, JPT_KEY_SHOW_RESOLUTION_TITLE, false, "Off", "On");
        ui.EndRows();
    }

    // ---- Controls

    void ReadGameRegistry() {
        unsigned long value = 0;
        invert_ = registry_.GetDword("InvertMouse", value) && value != 0;
        std::vector<uint8_t> keymap;
        if (!registry_.GetBinary("KeyMap", keymap) || !jpt_keymap_parse(keymap.data(), keymap.size(), keymap_)) {
            jpt_keymap_default(keymap_);
        }
    }

    bool SaveGameRegistry() {
        if (invert_dirty_ && registry_.SetDword("InvertMouse", invert_ ? 1 : 0)) {
            invert_dirty_ = false;
        }
        return !invert_dirty_;
    }

    void InvertMouseRow(trg::Ui& ui) {
        ui.Row("Invert mouse", kInvertMouseHelp);
        const int invert = ui.Segmented("##invert_mouse", {"Off", "On"}, invert_ ? 1 : 0);
        if (invert >= 0 && (invert == 1) != invert_) {
            invert_ = invert == 1;
            invert_dirty_ = true;
        }
    }

    void PageControls(trg::Ui& ui) {
        const std::vector<ConnectedGamepad> pads = host_.Gamepads();
        const jpt_gamepad_settings_t pad = GamepadSettings();
        const jpt_pad_family_t family = ShownFamily(pads);
        if (ui.Section("Gamepad", true)) {
            ui.Help(kGamepadHelp);
            ui.Info("Connected controller", pads.empty() ? "None" : pads.front().name.c_str());
            const std::string status = pads.empty()       ? std::string(Tr("Not connected"))
                                       : pads.size() == 1 ? std::string(Tr("Connected"))
                                                          : TrFormat("Connected (%d controllers)", int(pads.size()));
            ui.Info("Status", status.c_str());
            GamepadSettingsRows(ui);
            ui.EndRows();
        }
        if (ui.Section("Gamepad controls", true)) {
            const std::string help = TrFormat("With %s buttons.", jpt_pad_family_name(family)) + " " + Tr(kGuideHelp);
            ui.Help(help.c_str());
            GuideRows(ui, 1, family, pad);
            const std::string menus = TrFormat("D-pad or left stick, %s select, %s back, right stick pointer",
                                               ControlName(JPT_PAD_SOUTH).c_str(), ControlName(JPT_PAD_EAST).c_str());
            ui.Info("Menus", menus.c_str());
            ui.EndRows();
        }
        if (ui.Section("Gamepad bindings", false)) {
            ui.Help(kBindingHelp);
            for (int action = 0; action < JPT_ACT_COUNT; ++action) {
                BindingRow(ui, action);
            }
            if (ui.ConfirmRow("Gamepad defaults", "Puts the gamepad buttons and settings back to their defaults.",
                              "Reset to defaults", "Put every gamepad setting back to its default?", "Reset")) {
                for (int i = 0; i < JPT_GAMEPAD_SETTING_COUNT; ++i) {
                    settings_.Set(jpt_gamepad_setting_key(i), jpt_gamepad_setting_default(i));
                }
                ui.SetStatus("Gamepad settings reset to defaults.");
            }
            ui.EndRows();
        }
        if (ui.Section("Keyboard & Mouse", true)) {
            InvertMouseRow(ui);
            ui.Help(kKeyboardHelp);
            GuideRows(ui, 0, family, pad);
            ui.EndRows();
        }
        if (ui.Section("Port hotkeys", true)) {
            ui.Help("Rebinding the hotkeys is not available yet.");
            ui.Info("Hint", "F1");
            ui.Info("Controls", "F2");
            ui.Info("Windowed / Borderless Fullscreen", "F11");
            ui.Info("The game's original cheat input", "Ctrl+F11");
            ui.Info("Menu", "Esc");
            ui.Info("Exit", "Alt+F4");
            ui.EndRows();
        }
    }

    void GamepadSettingsRows(trg::Ui& ui) {
        std::vector<trg::Option> styles;
        for (int i = 0; i < JPT_GLYPHS_COUNT; ++i) {
            styles.push_back({jpt_glyph_style_keys[i], jpt_glyph_style_labels[i]});
        }
        ui.Combo("Controller glyphs", kGlyphsHelp, JPT_KEY_GAMEPAD_GLYPHS, "automatic", styles);
        ui.SliderInt("Look sensitivity X", kLookHelp, JPT_KEY_GAMEPAD_LOOK_X, 100, JPT_PAD_LOOK_MIN, JPT_PAD_LOOK_MAX,
                     "%d%%", 5);
        ui.SliderInt("Look sensitivity Y", kLookHelp, JPT_KEY_GAMEPAD_LOOK_Y, 100, JPT_PAD_LOOK_MIN, JPT_PAD_LOOK_MAX,
                     "%d%%", 5);
        ui.Toggle("Invert look Y", kInvertLookHelp, JPT_KEY_GAMEPAD_INVERT_Y, false, "Off", "On");
        ui.SliderInt("Stick deadzone", kStickDeadzoneHelp, JPT_KEY_GAMEPAD_STICK_DEADZONE, 15, JPT_PAD_STICK_DEADZONE_MIN,
                     JPT_PAD_STICK_DEADZONE_MAX, "%d%%");
        ui.SliderInt("Trigger deadzone", kTriggerDeadzoneHelp, JPT_KEY_GAMEPAD_TRIGGER_DEADZONE, 12,
                     JPT_PAD_TRIGGER_DEADZONE_MIN, JPT_PAD_TRIGGER_DEADZONE_MAX, "%d%%");
    }

    void GuideRows(trg::Ui& ui, int gamepad, jpt_pad_family_t family, const jpt_gamepad_settings_t& pad) const {
        jpt_prompt_context_t context{};
        context.gamepad = gamepad;
        context.family = family;
        context.settings = &pad;
        context.keymap = keymap_;
        context.keymap_count = JPT_KEYMAP_COUNT;
        jpt_guide_row_t rows[JPT_GUIDE_MAX_ROWS];
        const int count = jpt_controls_guide(&context, rows, JPT_GUIDE_MAX_ROWS);
        for (int i = 0; i < count; ++i) {
            ui.Info(rows[i].action, TrMessage(rows[i].binding).c_str());
        }
    }

    jpt_gamepad_settings_t GamepadSettings() const {
        jpt_gamepad_settings_t pad;
        jpt_gamepad_defaults(&pad);
        for (int i = 0; i < JPT_GAMEPAD_SETTING_COUNT; ++i) {
            const char* key = jpt_gamepad_setting_key(i);
            jpt_gamepad_apply(&pad, key, settings_.Get(key).c_str());
        }
        return pad;
    }

    int GlyphStyle() const {
        const std::string saved = settings_.Get(JPT_KEY_GAMEPAD_GLYPHS);
        for (int i = 0; i < JPT_GLYPHS_COUNT; ++i) {
            if (saved == jpt_glyph_style_keys[i]) {
                return i;
            }
        }
        return JPT_GLYPHS_AUTOMATIC;
    }

    // The chosen style, or with Automatic the first connected controller's (Xbox when none is connected).
    jpt_pad_family_t ShownFamily(const std::vector<ConnectedGamepad>& pads) const {
        jpt_pad_family_t connected = JPT_FAMILY_XBOX;
        if (!pads.empty()) {
            const ConnectedGamepad& first = pads.front();
            const jpt_pad_type_t type = first.type > 0 && first.type < JPT_PAD_TYPE_COUNT
                                            ? static_cast<jpt_pad_type_t>(first.type)
                                            : JPT_PAD_TYPE_UNKNOWN;
            connected = jpt_pad_family_of(type, first.vendor, first.product, first.name.c_str());
        }
        return jpt_glyph_family(static_cast<jpt_glyph_style_t>(GlyphStyle()), !pads.empty(), connected);
    }

    // Automatic names a button in both common layouts: the controller in use decides the glyphs.
    std::string ControlName(int control) const {
        const int style = GlyphStyle();
        if (style != JPT_GLYPHS_AUTOMATIC) {
            return Tr(jpt_pad_control_name(jpt_glyph_family(static_cast<jpt_glyph_style_t>(style), 1, JPT_FAMILY_XBOX),
                                           control));
        }
        const std::string xbox = Tr(jpt_pad_control_name(JPT_FAMILY_XBOX, control));
        const std::string playstation = Tr(jpt_pad_control_name(JPT_FAMILY_PLAYSTATION, control));
        return xbox == playstation ? xbox : xbox + " / " + playstation;
    }

    void BindingRow(trg::Ui& ui, int action) {
        const char* key = jpt_pad_actions[action].key;
        const std::string before = settings_.Get(key);
        std::vector<trg::Option> options{{"none", "Not bound"}};
        for (int control = 0; control < JPT_PAD_CONTROL_COUNT; ++control) {
            if (jpt_pad_bindable(control)) {
                options.push_back({jpt_pad_control_key(control), ControlName(control)});
            }
        }
        if (!ui.Combo(jpt_pad_actions[action].name, nullptr, key, jpt_default_value(key), options)) {
            return;
        }
        const std::string chosen = settings_.Get(key);
        if (chosen == "none") {
            return;
        }
        for (int other = 0; other < JPT_ACT_COUNT; ++other) {
            if (other != action && settings_.Get(jpt_pad_actions[other].key) == chosen) {
                settings_.Set(jpt_pad_actions[other].key, before);
            }
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
        ui.FolderRow("User data", UserDataFolderHelp(static_cast<UserDataOutcome>(config_.user_data_outcome)),
                     "Open user data folder", Utf8(user_dir_));
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
        ui.Heading("Translation credits");
        for (const jpt_l10n_credit_t& credit : jpt_l10n_credits) {
            std::string by = std::string(credit.author) + " (" + Tr(credit.source) + ")";
            if (credit.converter) {
                by += "; " + TrFormat("GameStrings conversion by %s", credit.converter);
            }
            ui.Info((std::string(Tr(jpt_l10n_language(credit.language)->name)) + " (" + Tr(credit.content) + ")").c_str(),
                    by.c_str());
        }
        ui.Info("Source preservation", JPT_L10N_PRESERVATION);
        ui.Info("PC port completion", (std::string(JPT_L10N_PORT_COMPLETION) + ": " + Tr(JPT_L10N_PORT_CONTENT)).c_str());
        ui.EndRows();
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
    std::filesystem::path game_dir_, user_dir_, saves_dir_;
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
    std::vector<jpt_gpu_t> gpus_;
    std::vector<std::string> gpu_names_;
    std::vector<unsigned> gpu_msaa_;
    PlayGate play_;
    double play_started_ = 0.0;
    bool invert_ = false, invert_dirty_ = false;
    jpt_key_binding_t keymap_[JPT_KEYMAP_COUNT] = {};
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

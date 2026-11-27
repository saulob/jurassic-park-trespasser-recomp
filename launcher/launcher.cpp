#include "jpt_launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "trg/headers.h"
#include "trg/launcher.h"
#include "trg/platform.h"

#include "custom_levels_page.h"
#include "disc_browser.h"
#include "file_browser.h"
#include "file_picker.h"
#include "game_files.h"
#include "game_install.h"
#include "graphics_cards.h"
#include "header_art.h"
#include "host.h"
#include "incident_page.h"
#include "jpt_config.h"
#include "jpt_display.h"
#include "jpt_gamepad.h"
#include "jpt_l10n.h"
#include "jpt_port.h"
#include "jpt_prompt_text.h"
#include "launcher_log.h"
#include "launcher_quit.h"
#include "launcher_renderer.h"
#include "localization/ui_text.h"
#include "options.h"
#include "page_icons.h"
#include "placement.h"
#include "process_probe.h"
#include "play_gate.h"
#include "portable.h"
#include "settings.h"
#include "system_info.h"
#include "update_config.h"
#include "updater/update_ui.h"
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

#ifdef _WIN32
constexpr char kShowLauncherHelp[] =
    "Off starts the game directly. Hold Shift while starting, or start the program with --launcher, to bring it back.";
#else
constexpr char kShowLauncherHelp[] =
    "Off starts the game directly. Start the program with --launcher to bring it back.";
#endif
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
#ifdef _WIN32
constexpr char kRendererHelp[] =
    "Direct3D 11 draws the game's 3D on the graphics card at the render resolution. Software is the game's own "
    "rasteriser at its original size.";
#else
constexpr char kRendererHelp[] =
    "Vulkan draws the game's 3D on the graphics card at the render resolution. Software is the game's own "
    "rasteriser at its original size.";
#endif
constexpr char kGraphicsCardHelp[] =
    "Selects which graphics card renders the game. If the selected card is no longer available, the first available "
    "card is used.";
constexpr char kRenderResolutionHelp[] =
    "The size the 3D world is really drawn at, with its own depth buffer and anti-aliasing; it follows the Aspect ratio "
    "on the Display page. Native uses the monitor's size. Original keeps the earlier compatibility size.";
constexpr char kFieldOfViewHelp[] =
    "How much of the world the 3D view shows, in degrees across a 4:3 picture, as Trespasser CE's FOV. 75 is the "
    "original view. Wider aspect ratios add view to the sides (Hor+) without stretching; menus, HUD, subtitles and "
    "videos stay the same.";
constexpr char kAntiAliasingHelp[] =
    "Multisample anti-aliasing of the 3D world. Only the levels this graphics card supports are listed.";
constexpr char kTextureFilteringHelp[] =
    "Sharper, steadier textures at a distance and on the ground seen at an angle, with mipmaps generated for the 3D "
    "world. Original uses the game's own filtering without them; Bilinear smooths every texture.";
#ifdef _WIN32
constexpr char kAmbientOcclusionHelp[] =
    "Soft shadows where the 3D world's surfaces meet: trees and rocks on the ground, corners and walls (Intel ASSAO). "
    "Higher levels look smoother and cost more. Direct3D 11 only.";
#else
constexpr char kAmbientOcclusionHelp[] =
    "Soft shadows where the 3D world's surfaces meet: trees and rocks on the ground, corners and walls (Intel ASSAO). "
    "Higher levels look smoother and cost more. Vulkan only.";
#endif
constexpr char kVSyncHelp[] = "On prevents tearing. Off allows it, for the lowest latency and G-Sync/FreeSync.";
constexpr char kShowFpsTitleHelp[] = "Shows the frame rate in the window title.";
constexpr char kShowFrameCounterHelp[] =
    "Shows the frame rate in the top right corner of the game. F6 shows or hides it while playing.";
constexpr char kShowResolutionTitleHelp[] = "Shows the game's render resolution in the window title.";
constexpr char kMinimapHelp[] =
    "Shows a circular map of the current level in the bottom-left corner, centred on Anne, with the direction she "
    "faces. North is always up. F5, or L3 + R3 on a gamepad, shows or hides it while playing.";
constexpr char kCompassHelp[] =
    "Shows the compass strip at the top of the screen, with the cardinal directions. Off hides only the strip: the "
    "minimap and the objective waypoints stay as set.";
constexpr char kWaypointsHelp[] =
    "Shows the current objective on the screen, on the compass and on the minimap: the diamond, the arrow, the "
    "distance and the route markers. Off hides only those; objectives and F4 Show Objective still work.";
constexpr char kSkipIntroHelp[] =
    "Skips the opening video before the main menu and the one after New Game. The credits and the ending still play.";
constexpr char kVideoSubtitlesHelp[] =
    "Shows subtitles in the game's language over the intro, the New Game video and the ending, where people speak.";
constexpr char kMaxObjectDetailHelp[] =
    "Every object keeps its most detailed model at any distance, as Trespasser CE's ForceMaxObjectDetail. Off lets the "
    "game swap distant objects for simpler models, as the original did.";
constexpr char kWaterRefractionHelp[] =
    "Distorts the scene behind water using the game's wave data, based on the refractive water effect from Trespasser "
    "CE. Off draws water as the original.";
constexpr char kEnhancedSkyHelp[] =
    "Fades the sky into the level's fog colour towards the horizon by view direction, with the level's own sky fog "
    "band, as Trespasser CE's enhanced sky. Off keeps the original sky fog.";
constexpr char kFogHelp[] =
    "The original game's distance fog over objects, terrain and water, in every renderer. Off draws them without it; "
    "the sky keeps its own haze, and the draw distance does not change.";
constexpr char kShadowsHelp[] =
    "The original game's shadows of objects and dinosaurs on the ground, in every renderer. Off draws the ground without "
    "them. Ambient occlusion is a separate setting.";
constexpr char kActiveAiHelp[] =
    "Controls how many AI creatures can be active at once. Original uses the retail limit of 3; Enhanced uses 6, as "
    "supported by Trespasser CE. Applies the next time the game starts.";
constexpr char kTrespasserCeHelp[] = "Gameplay enhancements reimplemented natively from Trespasser CE.";
constexpr char kOfficialPatchHelp[] =
    "Applies gameplay and level fixes from DreamWorks Interactive's official Trespasser v1.1 build 117 patch";
constexpr char kDualStowHelp[] =
    "Allows one item on the belt and one on the back while keeping the hand free. Hold Rotate Arm or Rotate Wrist "
    "while pressing Stow/Retrieve to stow without taking the other item out, or to take the item from the back. Off is "
    "the original behaviour.";
constexpr char kInvertMouseHelp[] = "Mouse look up and down inverted, the game's own option.";
constexpr char kMouseSensitivityHelp[] =
    "Controls how fast the mouse turns the view and moves Anne's hand on both axes. 1.00 is the original speed.";
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
constexpr char kHotkeysHelp[] =
    "Quick Save and Quick Load can be changed: choosing the other one's key swaps them. The other hotkeys are fixed.";
constexpr const char* kHotkeyHelp[JPT_HOTKEY_COUNT] = {
    "Saves the game at once in its own Quick Save slot, without touching your other saves. Works while playing a "
    "level.",
    "Loads the Quick Save. Nothing happens when there is none yet."};
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
    return Tr("Select your own disc image (.iso), or the folder of a game you already have: a copy of the disc or an "
              "installation. English 1.0.116 (UK or US disc), Russian (Fargus), Spanish or Brazilian Portuguese. The "
              "version is recognised from its files, never from a file or folder name. Sources are only read: nothing is "
              "mounted and no setup program or executable runs. Each version is copied into its own folder in the game "
              "folder (about 600 MB) and every file is checked before it can be played. No game files are included. "
              "Importing a version that is already installed copies nothing.");
}

// What an installation lacks, by name; the disc's folder usually has it.
std::string MissingFilesHelp(const std::vector<std::string>& missing) {
    std::string names;
    for (size_t i = 0; i < missing.size() && i < 8; ++i) names += (i ? ", " : "") + missing[i];
    if (missing.size() > 8) names += ", ...";
    return TrFormat("Missing or different from the retail files (%d): %s. Add the folder of the original disc to complete them.", int(missing.size()),
                    names.c_str());
}

LevelPick PickOf(const BrowsePurpose& purpose) {
    if (&purpose == &kBrowseLevelArchive) return LevelPick::kArchive;
    if (&purpose == &kBrowseLinkFolder) return LevelPick::kLink;
    if (&purpose == &kBrowseRelocateFolder) return LevelPick::kRelocate;
    return LevelPick::kFolder;
}

const BrowsePurpose& PurposeOf(LevelPick pick) {
    switch (pick) {
    case LevelPick::kArchive: return kBrowseLevelArchive;
    case LevelPick::kLink: return kBrowseLinkFolder;
    case LevelPick::kRelocate: return kBrowseRelocateFolder;
    case LevelPick::kFolder: break;
    }
    return kBrowseLevelFolder;
}

constexpr char kGameVersionHelp[] =
    "The installed version Play starts. Each version keeps its own saves; the launcher language is chosen in Gameplay.";
constexpr char kRemoveVersionHelp[] =
    "Deletes this version's copied game files. Your disc image, your saves and the other versions are not touched.";

bool Playable(const InstalledVersion* v) {
    return v && v->profile && v->check.state == GameState::kReady && jpt_game_version_playable(v->profile->version);
}

std::string VersionName(const InstalledVersion& v) {
    std::string name = Tr(v.profile->label());
    if (v.check.state != GameState::kReady) name += std::string(" (") + Tr(GameStateTitle(v.check.state)) + ")";
    else if (!jpt_game_version_playable(v.profile->version)) name += Tr(" (not playable yet)");
    return name;
}

std::string StatusDetail(const InstalledVersion* v, const GameCheck& check) {
    if (v && v->profile && check.state == GameState::kReady) {
        const jpt_game_version_t& g = *v->profile->version;
        if (!jpt_game_version_playable(&g)) return TrMessage(g.unplayable);
        return TrFormat("Game language: %s", Tr(g.language)) + "  \xC2\xB7  " + TrMessage(g.release);
    }
    if (check.state == GameState::kMissing) return Tr("Import the game from your Trespasser disc image.");
    return TrMessage(check.detail);
}

enum class Outcome { kRunning, kPlay, kQuit };

std::string RequestedRenderer(const jpt_launcher_config_t& config) {
    return config.launcher_renderer && *config.launcher_renderer ? config.launcher_renderer : std::string(kLauncherRendererAuto);
}

bool SoftwareRecovery(const jpt_launcher_config_t& config) {
    return RequestedRenderer(config) == kLauncherRendererSoftware;
}

class LauncherApp {
public:
    LauncherApp(const jpt_launcher_config_t& config, LauncherSettings& settings)
        : config_(config),
          settings_(settings),
          game_root_(PathFromUtf8(config.game_dir)),
          user_dir_(PathFromUtf8(config.user_dir)),
          registry_(PathFromUtf8(config.saves_dir) / JPT_REGISTRY_FILE) {
        Recheck();
    }

    const std::string& failure_text() const { return failure_text_; }

    // Runs the launcher window until Play or Quit. False when no window could be opened; failure_text says why.
    bool Run(Outcome& outcome) {
        SetLauncherLanguage(EffectiveLanguage());
        HostOptions options;
        options.renderer = RequestedRenderer(config_);
        options.title = kGameTitle;
        const WindowPlacement saved = ReadPlacement(settings_);
        if (PlacementIsSet(saved)) {
            options.restore = saved;
        }
        if (!host_.Open(options)) {
            failure_text_ = host_.FailureText();
            return false;
        }
        monitors_ = ListMonitors();
        DetectGraphics();
        Log("%zu monitor(s)", monitors_.size());
        ReadGameRegistry();
        if (LoadLauncherImage("header-art.png", header_art_image_)) {
            header_art_aspect_ = float(header_art_image_.width) / float(header_art_image_.height);
        }
        trg::SetTranslator(&TranslateLauncherText);
        incident_page_ = std::make_unique<IncidentPage>(IncidentPlaces());
        incident_page_->CheckReports();
        custom_levels_ = std::make_unique<CustomLevelsPage>(user_dir_, MakeCustomLevelsHooks());
        updater_ = std::make_unique<trg::update::UpdateController>(MakeUpdateConfig(InstallDir()));
        updater_->StartCheck();

        if (!DrawFirstFrame(outcome) && !RetryWithSoftware(options, outcome)) {
            StopUpdater();
            return false;
        }
        host_.Show();
        Log("shown");
        while (outcome == Outcome::kRunning) {
            if (!host_.WaitForFrame()) {
                outcome = Outcome::kQuit;
                break;
            }
            host_.BeginFrame();
            outcome = Frame();
            host_.EndFrame(LauncherTheme().bg);
        }
        if (outcome == Outcome::kPlay) StartHandoff();
        if (install_.running()) {
            install_.Cancel();
            install_.Finish();
        }
        custom_levels_.reset();
        if (!skip_finish_save_) {
            SavePlacement(placement_.Flush());
        }
        StopUpdater();
        launcher_.reset();
        host_.Close();
        if (config_.custom_level) *config_.custom_level = outcome == Outcome::kPlay ? level_ : jpt_custom_level_request_t{};
        Log(outcome == Outcome::kPlay ? "closed, starting the game" : "closed");
        return true;
    }

private:
    static unsigned Rgb(const ImVec4& c) {
        const auto byte = [](float v) { return unsigned(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return byte(c.x) << 16 | byte(c.y) << 8 | byte(c.z);
    }

    // The handoff window shows below this one before it closes, so the desktop never shows between them.
    void StartHandoff() {
        if (!config_.handoff) return;
        const trg::Theme theme = LauncherTheme();
        jpt_launch_handoff_request_t request{};
        request.window = host_.NativeWindow();
        request.custom = level_.scene[0] != 0;
        request.level = level_.package;
        request.background = Rgb(theme.bg);
        request.text = Rgb(theme.text);
        request.dim = Rgb(theme.dim);
        request.accent = Rgb(theme.accent);
        config_.handoff(&request);
    }

    void StopUpdater() {
        updater_->Cancel();
        // Do not join a network call on the UI thread when the launcher leaves.
        if (updater_->Busy()) updater_.release();
    }

    // Everything that belongs to the open window's renderer, then the first frame.
    bool DrawFirstFrame(Outcome& outcome) {
        placement_ = PlacementTracker(host_.Restored());
        fonts_ = trg::LoadSystemFonts(trg::layout::kTextBody);
        sponsor_icon_ = LoadSponsorIcon(host_);
        launcher_ = std::make_unique<trg::Launcher>(MakeConfig());
        host_.BeginFrame();
        outcome = Frame();
        if (host_.EndFrame(LauncherTheme().bg)) return true;
        failure_text_ = host_.RendererName() + ": " + host_.FailureText();
        Log("startup: first frame failed: %s", failure_text_.c_str());
        return false;
    }

    bool RetryWithSoftware(HostOptions& options, Outcome& outcome) {
        const bool retry = options.renderer == kLauncherRendererAuto && host_.RendererName() != kLauncherRendererSoftware;
        launcher_.reset();
        page_icons_.clear();
        header_art_ = sponsor_icon_ = ImTextureID{};
        header_art_width_ = header_art_height_ = 0;
        fonts_ = {};
        host_.Close();
        if (!retry) return false;
        Log("startup: fallback to software after the first frame");
        options.renderer = std::string(kLauncherRendererSoftware);
        if (!host_.Open(options)) {
            failure_text_ += "\n" + host_.FailureText();
            return false;
        }
        const std::string first = failure_text_;
        if (DrawFirstFrame(outcome)) return true;
        failure_text_ = first + "\n" + failure_text_;
        launcher_.reset();
        host_.Close();
        return false;
    }

    bool Ready() const { return Playable(Selected()); }

    const InstalledVersion* Selected() const { return selected_ >= 0 ? &versions_[size_t(selected_)] : nullptr; }

    const jpt_game_version_t* SelectedVersion() const {
        const InstalledVersion* v = Selected();
        return v && v->profile ? v->profile->version : nullptr;
    }

    // Each game version keeps its own language; a version with no choice yet plays in its release's language.
    std::string LanguageKey() const {
        char key[JPT_GAME_VERSION_ID_MAX + 16];
        return jpt_language_key(SelectedVersion(), key, sizeof key) ? std::string(key) : std::string(JPT_KEY_LANGUAGE);
    }

    std::string EffectiveLanguage() const {
        const jpt_game_version_t* version = SelectedVersion();
        const std::string key = LanguageKey();
        if (!version) return settings_.Get(JPT_KEY_LANGUAGE);
        const std::string chosen = settings_.Has(key) ? settings_.Get(key) : std::string();
        const std::string earlier = settings_.Has(JPT_KEY_LANGUAGE) ? settings_.Get(JPT_KEY_LANGUAGE) : std::string();
        return jpt_language_for(version, chosen.c_str(), earlier.c_str());
    }

    std::filesystem::path SavesDir(const InstalledVersion* v) const {
        char dir[JPT_USER_PATH_CAP * 2];
        if (!v || !v->profile ||
            !jpt_game_version_saves_dir(config_.user_dir, config_.saves_dir, v->profile->version, dir, sizeof dir))
            return PathFromUtf8(config_.saves_dir);
        return PathFromUtf8(dir);
    }

    Outcome Frame() {
        SetLauncherLanguage(EffectiveLanguage());
        NormalizeGraphicsCard();
        NormalizeCheats();
        TrackPlacement();
        PollInstall();
        custom_levels_->Poll(*launcher_);
        PollUpdate();
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
        if (disc_browser_.active()) {
            if (const std::optional<std::filesystem::path> picked =
                    disc_browser_.Draw(fonts_.regular, fonts_.size, launcher_->config().theme)) {
                const BrowsePurpose& purpose = disc_browser_.purpose();
                if (&purpose == &kBrowseDisc) StartInstall(Utf8(*picked), browse_replace_);
                else if (&purpose == &kBrowseInstallFolder) StartFolderInstall({*picked}, browse_replace_);
                else if (&purpose == &kBrowseDiscFolder) AddDiscFolder(*picked);
                else custom_levels_->Picked(Utf8(*picked), PickOf(purpose));
            }
        }
        if (result == trg::Result::kNone && !play_.starting() &&
            HandleUpdateAction(trg::update::DrawUpdateBanner(updater_->snapshot(), launcher_->config().theme, fonts_))) {
            return Outcome::kQuit;
        }
        if (play_.starting()) {
            DrawStartingMessage();
        }
        switch (result) {
        case trg::Result::kPlay:
            RequestPlay({});
            break;
        case trg::Result::kQuit:
            if (!play_.starting() && !disc_browser_.active() &&
                LauncherQuitAllowed(ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
                return Outcome::kQuit;
            }
            break;
        case trg::Result::kNone:
            break;
        }
        return Outcome::kRunning;
    }

    // Lists the installed versions again and selects the setting's one, or another ready one when it is gone.
    void Recheck() {
        versions_ = FindInstalledVersions(game_root_);
        const std::string wanted = settings_.Get(JPT_KEY_GAME_VERSION);
        const InstalledVersion* chosen = ChooseVersion(versions_, wanted);
        selected_ = chosen ? int(chosen - versions_.data()) : -1;
        check_ = chosen ? chosen->check : versions_.empty() ? GameCheck{} : versions_.front().check;
        if (!wanted.empty() && (!chosen || _stricmp(wanted.c_str(), chosen->profile->id()) != 0))
            settings_.Set(JPT_KEY_GAME_VERSION, chosen ? chosen->profile->id() : "");
        const std::filesystem::path registry = SavesDir(chosen) / JPT_REGISTRY_FILE;
        if (registry != registry_.file()) {
            registry_ = GameRegistry(registry);
            invert_dirty_ = false;
            ReadGameRegistry();
        }
        for (const InstalledVersion& v : versions_)
            Log("game version %s%s in %s: %s%s%s", v.profile ? v.profile->id() : "unknown", &v == chosen ? " (selected)" : "",
                Utf8(v.dir).c_str(), GameStateTitle(v.check.state), v.check.detail.empty() ? "" : ": ",
                v.check.detail.c_str());
        if (versions_.empty()) Log("game files: %s", GameStateTitle(check_.state));
    }

    void SelectVersion(const InstalledVersion& v) {
        settings_.Set(JPT_KEY_GAME_VERSION, v.profile->id());
        Recheck();
    }

    // PLAY saves the settings once here, then starts the transition. A failed save keeps the launcher open.
    void RequestPlay(const jpt_custom_level_request_t& level) {
        if (play_.starting()) {
            return;
        }
        Recheck();
        switch (play_.Request(install_.running() || custom_levels_->importing(), Ready(), [this] { return launcher_->Save(); })) {
        case PlayRequest::kStarting:
            level_ = level;
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
            if (install_.running() || custom_levels_->importing()) {
                return trg::PlayCheck{false, "Wait for the installation to finish.", 0};
            }
            if (!Ready()) {
                return trg::PlayCheck{false, "The selected game version cannot be played: see the Play page.", 0};
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
        trg::Page result;
        result.name = PageName(page);
        switch (page) {
        case PageId::kPlay:
            result.blurb = "Import game versions from your discs, choose one and start it.";
            result.draw = [this](trg::Ui& ui) { PagePlay(ui); };
            result.needs_attention = [this] { return !Ready(); };
            break;
        case PageId::kCustomLevels:
            result.blurb = "Import levels made by the community and play them.";
            result.draw = [this](trg::Ui& ui) { custom_levels_->Draw(ui); };
            break;
        case PageId::kDisplay:
            result.blurb = "Window, monitor, aspect ratio and presentation.";
            result.draw = [this](trg::Ui& ui) { PageDisplay(ui); };
            break;
        case PageId::kGraphics:
            result.blurb = "Renderer, graphics card, resolution, field of view, anti-aliasing and texture filtering.";
            result.draw = [this](trg::Ui& ui) { PageGraphics(ui); };
            break;
        case PageId::kGameplay:
            result.blurb = "Language, Skip Intro, video subtitles and what the window title shows.";
            result.draw = [this](trg::Ui& ui) { PageGameplay(ui); };
            break;
        case PageId::kControls:
            result.blurb = "Keyboard, mouse and gamepad.";
            result.draw = [this](trg::Ui& ui) { PageControls(ui); };
            break;
        case PageId::kCheats:
            result.blurb = kCheatsPageSubtitle;
            result.draw = [this](trg::Ui& ui) { PageCheats(ui); };
            break;
        case PageId::kBugReport:
            result.blurb = "Describe a problem and send a report with one game log.";
            result.draw = [this](trg::Ui& ui) { PageBugReport(ui); };
            break;
        case PageId::kAbout:
            result.blurb = "This port, and where your files and settings live.";
            result.draw = [this](trg::Ui& ui) { PageAbout(ui); };
            break;
        }
        return result;
    }

    // ---- Play

    void PagePlay(trg::Ui& ui) {
        const InstalledVersion* selected = Selected();
        const bool ready = Ready();
        std::string title = Tr(GameStateTitle(check_.state));
        if (selected && selected->profile) title += " \xC2\xB7 " + std::string(Tr(selected->profile->label()));
        ui.StatusCard(ready ? trg::Status::kReady : trg::Status::kAttention, title.c_str(),
                      StatusDetail(selected, check_).c_str());
        if (!install_failure_.empty()) {
            ui.Spacer(4);
            ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
            ImGui::TextUnformatted(Tr(install_failure_.c_str()));
            ImGui::PopStyleColor();
            ui.Help((TrMessage(install_failure_text_) + Tr(". The installed versions were not changed.")).c_str());
            ui.Spacer(4);
        }
        if (versions_.size() > 1) {
            ui.Row("Game version", kGameVersionHelp);
            VersionCombo();
        }
        ui.Row("Game files");
        if (!ui.TaskProgress(install_)) {
            const bool repair = selected && selected->profile && selected->check.state != GameState::kReady;
            const bool accent = repair || versions_.empty();
            const char* disc = repair ? "Reinstall from Disc Image..." : "Import Disc Image...";
            const char* folder = repair ? "Reinstall from Existing Installation..." : "Import Existing Installation...";
            if (accent ? ui.AccentButton(Tr(disc)) : ImGui::Button(Tr(disc), ImVec2(-FLT_MIN, 0))) BrowseAndInstall(repair);
            if (ImGui::Button(Tr(folder), ImVec2(-FLT_MIN, 0))) BrowseInstallFolder(repair);
            if (!install_missing_.empty() && !install_folders_.empty() && ImGui::Button(Tr("Add the Disc's Folder..."), ImVec2(-FLT_MIN, 0)))
                BrowseDiscFolder();
        }
        ui.EndRows();
        if (!install_missing_.empty() && !install_.running()) ui.Help(MissingFilesHelp(install_missing_).c_str());
        ui.Help(GameFilesHelp().c_str());
        if (selected && selected->profile && !install_.running() &&
            ui.ConfirmRow("Remove version", kRemoveVersionHelp, "Remove this version...",
                          "Delete this version's game files? Saves, settings and your disc image are kept.", "Remove")) {
            RemoveSelected();
        }
        ui.Toggle("Show this launcher", kShowLauncherHelp, JPT_KEY_SHOW_LAUNCHER, true, "Off", "On");
        ui.EndRows();
    }

    void VersionCombo() {
        const InstalledVersion* selected = Selected();
        const std::string current = selected && selected->profile ? VersionName(*selected) : std::string();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##game_version", current.c_str())) {
            for (size_t i = 0; i < versions_.size(); ++i) {
                if (!versions_[i].profile) continue;
                const bool is_selected = int(i) == selected_;
                if (ImGui::Selectable(VersionName(versions_[i]).c_str(), is_selected) && !is_selected) {
                    SelectVersion(versions_[i]);
                    break;
                }
            }
            ImGui::EndCombo();
        }
    }

    // ---- Custom Levels

    CustomLevelsHooks MakeCustomLevelsHooks() {
        CustomLevelsHooks hooks;
        hooks.choose = [this](LevelPick pick) { BrowseForLevel(pick); };
        hooks.game_ready = [this] { return Ready(); };
        hooks.data_dir = [this] {
            const InstalledVersion* selected = Selected();
            return selected ? selected->dir / "data" : std::filesystem::path();
        };
        hooks.play = [this](const jpt_custom_level_request_t& level) { RequestPlay(level); };
        hooks.open_folder = [](const std::filesystem::path& folder) { trg::OpenInFileManager(Utf8(folder)); };
        hooks.open_file = [](const std::filesystem::path& file) { trg::OpenUrl(Utf8(file)); };
        hooks.sort = [this] { return settings_.Get(kLauncherLevelSortKey); };
        hooks.set_sort = [this](const std::string& sort) {
            if (!settings_.SaveNow({{kLauncherLevelSortKey, sort}})) Log("the order of the custom levels could not be saved");
        };
        hooks.open_bug_report = [this](const std::string& package, const std::string& scene) {
            if (package.empty()) incident_page_->ChooseRunningLog();
            else incident_page_->ChooseLevelLog(package, scene);
            launcher_->GoToPage(launcher_->FindPage(PageName(PageId::kBugReport)));
        };
        return hooks;
    }

    void BrowseForLevel(LevelPick pick) {
        if (custom_levels_->importing() || disc_browser_.active()) return;
        const BrowsePurpose& purpose = PurposeOf(pick);
        if (UseBuiltInChooser()) {
            disc_browser_.Open(PortFolder(), purpose);
            return;
        }
        if (!FilePickerAvailable()) {
            Log("no file picker: neither zenity nor kdialog is on the PATH");
            launcher_->SetStatus(Tr("No file picker was found. Install zenity or kdialog to choose the level."), 10.0);
            return;
        }
        custom_levels_->Picked(pick == LevelPick::kArchive
                                   ? trg::BrowseForFile(Tr("Select a custom level (.zip, .7z or .rar)"), {{Tr("Level packages (*.zip, *.7z, *.rar)"), "*.zip;*.7z;*.rar"}})
                                   : trg::BrowseForFolder(Tr(purpose.title)),
                               pick);
    }

    // The folders of the game the player already has: an installation, the disc copied to a folder, or both.
    void BrowseInstallFolder(bool replace) {
        if (install_.running() || disc_browser_.active()) return;
        browse_replace_ = replace;
        if (UseBuiltInChooser()) {
            disc_browser_.Open(PortFolder(), kBrowseInstallFolder);
            return;
        }
        if (!FilePickerAvailable()) {
            Log("no file picker: neither zenity nor kdialog is on the PATH");
            launcher_->SetStatus(Tr("No file picker was found. Install zenity or kdialog to choose the folder."), 10.0);
            return;
        }
        const std::string picked = trg::BrowseForFolder(Tr(kBrowseInstallFolder.title));
        if (!picked.empty()) StartFolderInstall({PathFromUtf8(picked)}, replace);
    }

    void BrowseDiscFolder() {
        if (install_.running() || disc_browser_.active() || install_folders_.empty()) return;
        if (UseBuiltInChooser()) {
            disc_browser_.Open(PortFolder(), kBrowseDiscFolder);
            return;
        }
        if (!FilePickerAvailable()) {
            launcher_->SetStatus(Tr("No file picker was found. Install zenity or kdialog to choose the folder."), 10.0);
            return;
        }
        const std::string picked = trg::BrowseForFolder(Tr(kBrowseDiscFolder.title));
        if (!picked.empty()) AddDiscFolder(PathFromUtf8(picked));
    }

    // The disc's folder completes the installation chosen before; it is read after it.
    void AddDiscFolder(const std::filesystem::path& folder) {
        std::vector<std::filesystem::path> folders = install_folders_;
        if (std::find(folders.begin(), folders.end(), folder) == folders.end()) folders.push_back(folder);
        StartFolderInstall(folders, browse_replace_);
    }

    static InstallHooks TaskHooks(trg::Task& task, bool replace) {
        InstallHooks hooks;
        hooks.replace = replace;
        hooks.phase = [&task](const char* text) {
            task.SetLabel(text);
            task.Progress(0, 0);
        };
        hooks.progress = [&task](uint64_t done, uint64_t total) {
            task.Progress(static_cast<long long>(done), static_cast<long long>(total));
            return !task.cancelled();
        };
        hooks.log = [](const std::string& line) { Log("%s", line.c_str()); };
        return hooks;
    }

    void StartFolderInstall(const std::vector<std::filesystem::path>& folders, bool replace) {
        if (folders.empty() || install_.running() || play_.starting()) return;
        install_failure_.clear();
        install_failure_text_.clear();
        install_missing_.clear();
        install_folders_ = folders;
        outcome_ = std::make_shared<InstallResult>();
        const bool started = install_.Start("Reading the folders...", [outcome = outcome_, folders, root = game_root_, replace](trg::Task& task) {
            *outcome = InstallFromFolders(folders, root, TaskHooks(task, replace));
            return outcome->ok() ? std::string() : std::string(InstallStatusTitle(outcome->status));
        });
        if (!started) return;
        std::string list;
        for (const std::filesystem::path& folder : folders) list += (list.empty() ? "" : ", ") + Utf8(folder);
        Log("importing the game%s from the folder(s) %s into %s", replace ? " (reinstall)" : "", list.c_str(), Utf8(game_root_).c_str());
    }

    static bool UseBuiltInChooser() {
#ifdef _WIN32
        constexpr bool is_windows = true;
#else
        constexpr bool is_windows = false;
#endif
        return UseBuiltInFileChooser(is_windows, jpt_port_in_gamescope() != 0, FilePickerAvailable(), FileChooserPreference());
    }

    void RemoveSelected() {
        const InstalledVersion* selected = Selected();
        if (!selected || !selected->profile || install_.running()) return;
        const std::string id = selected->profile->id(), dir = Utf8(selected->dir);
        std::string error;
        const bool removed = RemoveVersion(*selected, error);
        Log("remove %s from %s: %s%s%s", id.c_str(), dir.c_str(), removed ? "done" : "failed", error.empty() ? "" : ": ",
            error.c_str());
        Recheck();
        launcher_->SetStatus(removed ? std::string(Tr("Version removed. Saves and settings were kept."))
                                     : Tr("The version could not be removed: ") + TrMessage(error),
                             8.0);
    }

    void BrowseAndInstall(bool replace) {
        if (install_.running() || disc_browser_.active()) {
            return;
        }
        if (UseBuiltInChooser()) {
            browse_replace_ = replace;
            disc_browser_.Open(PortFolder());
            return;
        }
        if (!FilePickerAvailable()) {
            Log("no file picker: neither zenity nor kdialog is on the PATH");
            launcher_->SetStatus(Tr("No file picker was found. Install zenity or kdialog to choose the disc image."), 10.0);
            return;
        }
        StartInstall(trg::BrowseForFile(Tr("Select your Trespasser disc image"), {{Tr("Disc images (*.iso)"), "*.iso"}}),
                     replace);
    }

    void StartInstall(const std::string& picked, bool replace) {
        if (picked.empty() || install_.running() || play_.starting()) {
            return;
        }
        install_failure_.clear();
        install_failure_text_.clear();
        install_missing_.clear();
        install_folders_.clear();
        outcome_ = std::make_shared<InstallResult>();
        const bool started = install_.Start(
            "Reading the disc image...",
            [outcome = outcome_, iso = PathFromUtf8(picked), root = game_root_, replace](trg::Task& task) {
                *outcome = InstallFromIso(iso, root, TaskHooks(task, replace));
                return outcome->ok() ? std::string() : std::string(InstallStatusTitle(outcome->status));
            });
        if (started) {
            Log("importing the game%s from %s into %s", replace ? " (reinstall)" : "", picked.c_str(),
                Utf8(game_root_).c_str());
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
        if (outcome->ok()) {
            install_folders_.clear();
            install_missing_.clear();
            settings_.Set(JPT_KEY_GAME_VERSION, outcome->profile->id());
            Recheck();
            if (outcome->status == InstallStatus::kUpToDate) {
                Log("game version already installed: %s", outcome->detail.c_str());
                launcher_->SetStatus(Tr("This version is already installed: nothing was copied. It is now selected."), 8.0);
                return;
            }
            Log("game installed: %s in %s, %zu files, %llu bytes, %zu repaired", outcome->detail.c_str(),
                Utf8(outcome->dir).c_str(), outcome->files, static_cast<unsigned long long>(outcome->bytes),
                outcome->repaired);
            launcher_->SetStatus(Ready() ? Tr("Game installed. Ready to play.")
                                         : Tr("Game installed. This version cannot be played yet: see the Play page."),
                                 8.0);
            return;
        }
        Recheck();
        if (outcome->status == InstallStatus::kCancelled) {
            Log("game install cancelled");
            launcher_->SetStatus(Tr("Installation cancelled. Nothing was changed."), 6.0);
            return;
        }
        for (const std::string& line : outcome->diagnostics) Log("unsupported build diagnostics: %s", line.c_str());
        Log("game install failed: %s (%s)", InstallStatusTitle(outcome->status), outcome->detail.c_str());
        install_failure_ = InstallStatusTitle(outcome->status);
        install_failure_text_ = outcome->detail;
        install_missing_ = outcome->missing;
        if (outcome->status != InstallStatus::kIncompleteInstallation) install_folders_.clear();
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

    void DetectGraphics() {
        Log("startup: gpu enumeration starting");
        cards_ = DetectGraphicsCards(SoftwareRecovery(config_),
                                     {[](std::string& detail) { return ListGpus(&detail); }, [](int index) { return GpuMsaaMask(index); }});
        for (size_t i = 0; i < cards_.gpus.size(); ++i) {
            if (!cards_.gpus[i].name[0]) {
                continue;
            }
            Log("graphics card %zu \"%s\", MSAA %s%s%s", i, cards_.gpus[i].label, cards_.msaa[i] & 4 ? "2x " : "",
                cards_.msaa[i] & 16 ? "4x " : "", cards_.msaa[i] & 256 ? "8x" : "");
        }
        Log("startup: gpu enumeration result=%s usable=%zu", GraphicsCardsResult(cards_), cards_.names.size());
        if (!cards_.detail.empty()) Log("startup: gpu enumeration reason=%s", cards_.detail.c_str());
    }

    void NormalizeGraphicsCard() {
        if (const std::optional<std::string> label = CardToStore(cards_, settings_.Get(JPT_KEY_GRAPHICS_CARD))) {
            settings_.Set(JPT_KEY_GRAPHICS_CARD, *label);
        }
    }

    void GraphicsCardCombo() {
        if (cards_.names.empty()) {
            ImGui::TextWrapped("%s", Tr(GraphicsCardsProblem(cards_)));
            return;
        }
        const std::string current = settings_.Get(JPT_KEY_GRAPHICS_CARD);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##graphics_card", current.c_str())) {
            for (const std::string& name : cards_.names) {
                if (ImGui::Selectable(name.c_str(), name == current) && name != current) {
                    settings_.Set(JPT_KEY_GRAPHICS_CARD, name);
                }
            }
            ImGui::EndCombo();
        }
    }

    void PageGraphics(trg::Ui& ui) {
        ui.Combo("Renderer", kRendererHelp, JPT_KEY_RENDERER, "auto",
#ifdef _WIN32
                 {{"auto", "Auto"}, {"d3d11", "Direct3D 11"}, {"vulkan", "Vulkan"}, {"software", "Software"}});
#else
                 {{"auto", "Auto"}, {"vulkan", "Vulkan"}, {"software", "Software"}});
#endif
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
        FieldOfViewRow(ui);
        AntiAliasingRow(ui);
        ui.Combo("Ambient occlusion", kAmbientOcclusionHelp, JPT_KEY_AMBIENT_OCCLUSION, "medium",
                 {{"off", "Off"}, {"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"ultra", "Ultra"}});
        ui.Toggle("Fog", kFogHelp, JPT_KEY_FOG_ENABLED, true, "Off", "On");
        ui.Toggle("Shadows", kShadowsHelp, JPT_KEY_SHADOWS_ENABLED, true, "Off", "On");
        {
            const bool available = Direct3DEffectsAvailable(settings_.Get(JPT_KEY_RENDERER));
            ImGui::BeginDisabled(!available);
            ui.Toggle("Water refraction", available ? kWaterRefractionHelp : kDirect3DOnlyHelp, JPT_KEY_WATER_REFRACTION, true,
                      "Off", "On");
            ui.Toggle("Enhanced sky", available ? kEnhancedSkyHelp : kDirect3DOnlyHelp, JPT_KEY_ENHANCED_SKY, true, "Off", "On");
            ImGui::EndDisabled();
        }
        ui.Toggle("Maximum object detail", kMaxObjectDetailHelp, JPT_KEY_MAX_OBJECT_DETAIL, false, "Off", "On");
        ui.Combo("Texture filtering", kTextureFilteringHelp, JPT_KEY_TEXTURE_FILTER, "anisotropic16",
                 {{"original", "Original"},
                  {"bilinear", "Bilinear"},
                  {"anisotropic4", "Anisotropic 4x"},
                  {"anisotropic8", "Anisotropic 8x"},
                  {"anisotropic16", "Anisotropic 16x"}});
        ui.Toggle("VSync", kVSyncHelp, JPT_KEY_VSYNC, true, "Off", "On");
        ui.EndRows();
    }

    // A slider with its value and a Default button on one row.
    template <typename Value>
    bool SliderWithDefault(const char* id, Value& value, Value lo, Value hi, Value def, const char* format) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const char* reset = Tr("Default");
        const float button = ImGui::CalcTextSize(reset).x + style.FramePadding.x * 2.0f;
        bool changed = false;
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(-(button + style.ItemSpacing.x));
        if constexpr (std::is_same_v<Value, int>) {
            changed = ImGui::SliderInt("##slider", &value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
        } else {
            changed = ImGui::SliderFloat("##slider", &value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(value == def);
        if (ImGui::Button(reset)) {
            value = def;
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }

    void FieldOfViewRow(trg::Ui& ui) {
        ui.Row("Field of view", kFieldOfViewHelp);
        int fov = JPT_FOV_DEFAULT;
        jpt_config_t parsed;
        jpt_config_defaults(&parsed);
        if (jpt_config_apply(&parsed, JPT_KEY_FOV, settings_.Get(JPT_KEY_FOV).c_str())) fov = parsed.fov;
        if (SliderWithDefault("fov", fov, JPT_FOV_MIN, JPT_FOV_MAX, JPT_FOV_DEFAULT, "%d\xC2\xB0")) {
            settings_.SetInt(JPT_KEY_FOV, jpt_fov_clamp(fov));
        }
    }

    void AntiAliasingRow(trg::Ui& ui) {
        const std::string current = settings_.Get(JPT_KEY_MSAA);
        const SampleLevels levels = AntiAliasingLevels(cards_, settings_.Get(JPT_KEY_GRAPHICS_CARD), current);
        std::vector<trg::Option> options{{"0", "Off"}};
        for (const unsigned samples : levels.levels) {
            options.push_back({std::to_string(samples), std::to_string(samples) + "x MSAA"});
        }
        std::string help = Tr(kAntiAliasingHelp);
        if (levels.unsupported) {
            help += " " + TrFormat("The saved %sx is not supported here: choose another level.", current.c_str());
        }
        ui.Choice("Anti-aliasing", help.c_str(), JPT_KEY_MSAA, "4", options, 4);
    }

    // ---- Gameplay

    void PageGameplay(trg::Ui& ui) {
        std::vector<trg::Option> languages;
        for (int i = 0; i < JPT_L10N_LANGUAGE_COUNT; ++i) {
            const jpt_l10n_language_t* language = jpt_l10n_language_listed(i);
            languages.push_back({language->code, language->name});
        }
        for (const GameplayItem item : kGameplayOrder) {
            switch (item) {
            case GameplayItem::kLanguage:
                ui.Combo("Language", kLanguageHelp, LanguageKey().c_str(), EffectiveLanguage().c_str(), languages);
                break;
            case GameplayItem::kMinimap:
                ui.Toggle("Enable Minimap", kMinimapHelp, JPT_KEY_MINIMAP, true, "Off", "On");
                break;
            case GameplayItem::kCompass:
                ui.Toggle("Enable Compass", kCompassHelp, JPT_KEY_COMPASS, true, "Off", "On");
                break;
            case GameplayItem::kWaypoints:
                ui.Toggle("Enable Waypoints", kWaypointsHelp, JPT_KEY_WAYPOINTS, true, "Off", "On");
                break;
            case GameplayItem::kVideoSubtitles:
                ui.Toggle("Video subtitles", kVideoSubtitlesHelp, JPT_KEY_VIDEO_SUBTITLES, true, "Off", "On");
                break;
            case GameplayItem::kSkipIntro:
                ui.Toggle("Skip Intro", kSkipIntroHelp, JPT_KEY_SKIP_INTRO, false, "Off", "On");
                ui.EndRows();
                break;
            case GameplayItem::kTrespasserCeHeading:
                ui.Heading("Trespasser CE");
                ui.Help(kTrespasserCeHelp);
                break;
            case GameplayItem::kDualStow:
                ui.Toggle("Dual stow", kDualStowHelp, JPT_KEY_DUAL_STOW, true, "Off", "On");
                break;
            case GameplayItem::kActiveAi:
                ui.Combo("Active AI", kActiveAiHelp, JPT_KEY_ACTIVE_AI, "enhanced", {{"original", "Original"}, {"enhanced", "Enhanced"}});
                ui.EndRows();
                break;
            case GameplayItem::kOfficialPatchHeading:
                ui.Heading("Official Patch 1.1");
                break;
            case GameplayItem::kOfficialPatchFixes:
                ui.Toggle("Patch 1.1 fixes", kOfficialPatchHelp, JPT_KEY_OFFICIAL_PATCH_117, true, "Off", "On");
                ui.EndRows();
                ui.Spacer(12);
                break;
            case GameplayItem::kFrameRateInTitle:
                ui.Toggle("Frame rate in title", kShowFpsTitleHelp, JPT_KEY_SHOW_FPS, true, "Off", "On");
                break;
            case GameplayItem::kFrameCounter:
                ui.Toggle("Frame counter", kShowFrameCounterHelp, JPT_KEY_SHOW_FRAME_COUNTER, false, "Off", "On");
                break;
            case GameplayItem::kRenderResolutionInTitle:
                ui.Toggle("Render resolution in title", kShowResolutionTitleHelp, JPT_KEY_SHOW_RESOLUTION_TITLE, false, "Off", "On");
                ui.EndRows();
                break;
            }
        }
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

    void MouseSensitivityRow(trg::Ui& ui) {
        ui.Row("Mouse sensitivity", kMouseSensitivityHelp);
        float sensitivity = JPT_MOUSE_SENSITIVITY_DEFAULT;
        jpt_config_t parsed;
        jpt_config_defaults(&parsed);
        if (jpt_config_apply(&parsed, JPT_KEY_MOUSE_SENSITIVITY, settings_.Get(JPT_KEY_MOUSE_SENSITIVITY).c_str())) {
            sensitivity = parsed.mouse_sensitivity;
        }
        if (SliderWithDefault("mouse_sensitivity", sensitivity, JPT_MOUSE_SENSITIVITY_MIN, JPT_MOUSE_SENSITIVITY_MAX,
                              JPT_MOUSE_SENSITIVITY_DEFAULT, "%.2f")) {
            char text[16];
            jpt_mouse_sensitivity_text(sensitivity, text, sizeof text);
            settings_.Set(JPT_KEY_MOUSE_SENSITIVITY, text);
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
            MouseSensitivityRow(ui);
            InvertMouseRow(ui);
            ui.Help(kKeyboardHelp);
            GuideRows(ui, 0, family, pad);
            ui.EndRows();
        }
        if (ui.Section("Port hotkeys", true)) {
            ui.Help(kHotkeysHelp);
            for (int hotkey = 0; hotkey < JPT_HOTKEY_COUNT; ++hotkey) {
                HotkeyRow(ui, hotkey);
            }
            ui.Info("Hint", "F1");
            ui.Info("Controls", "F2");
            ui.Info("Windowed / Borderless Fullscreen", "F11");
            ui.Info("Menu", "Esc");
            ui.Info("Exit", "Alt+F4");
            ui.EndRows();
        }
    }

    void HotkeyRow(trg::Ui& ui, int hotkey) {
        const char* key = jpt_hotkeys[hotkey].setting;
        const std::string before = settings_.Get(key);
        std::vector<trg::Option> options;
        char label[16];
        for (const jpt_hotkey_choice_t& choice : jpt_hotkey_choices) {
            options.push_back({choice.key, jpt_key_label(choice.vk, label, sizeof label)});
        }
        if (!ui.Combo(jpt_hotkeys[hotkey].name, kHotkeyHelp[hotkey], key, jpt_hotkeys[hotkey].default_key, options)) {
            return;
        }
        const std::string chosen = settings_.Get(key);
        for (int other = 0; other < JPT_HOTKEY_COUNT; ++other) {
            if (other != hotkey && _stricmp(settings_.Get(jpt_hotkeys[other].setting).c_str(), chosen.c_str()) == 0) {
                settings_.Set(jpt_hotkeys[other].setting, before);
            }
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

    void PageCheats(trg::Ui& ui) {
        ui.Help(kCheatsHelp);
        ui.Spacer(6);
        for (const jpt_cheat_info_t* cheat : LauncherCheats()) {
            if (cheat->kind == JPT_CHEAT_CHOICE) {
                std::vector<trg::Option> options;
                for (int i = 0; i < cheat->choice_count; ++i) {
                    options.push_back({cheat->choices[i].saved, cheat->choices[i].label});
                }
                ui.Combo(cheat->label, cheat->help, cheat->key, jpt_cheat_saved_text(cheat, cheat->default_value), options);
            } else {
                ui.Toggle(cheat->label, cheat->help, cheat->key, cheat->default_value != 0, "Off", "On");
            }
        }
        ui.EndRows();
    }

    void NormalizeCheats() {
        for (const jpt_cheat_info_t* cheat : LauncherCheats()) {
            const std::string text = settings_.Get(cheat->key);
            const std::string canonical = CanonicalCheatValue(*cheat, text);
            if (text != canonical) {
                settings_.Set(cheat->key, canonical);
            }
        }
    }

    // ---- Bug Report

    IncidentPage::Places IncidentPlaces() const {
        const std::filesystem::path logs = PathFromUtf8(config_.logs_dir ? config_.logs_dir : "logs");
        const std::filesystem::path running = config_.log_file ? PathFromUtf8(config_.log_file) : std::filesystem::path();
        return {logs, running, user_dir_, {InstallDir(), game_root_, user_dir_},
                [](const std::string& line) { Log("%s", line.c_str()); }};
    }

    ReportFacts CurrentReportFacts() {
        static const std::string operating_system = OperatingSystemText();
        const InstalledVersion* v = Selected();
        const bool known = v && v->profile && Ready();
        return {JPT_PORT_VERSION,
                known ? std::string(v->profile->label()) : std::string("Not installed"),
                known ? std::to_string(v->profile->version->build) : std::string(),
                operating_system};
    }

    void PageBugReport(trg::Ui& ui) { incident_page_->Draw(ui, CurrentReportFacts()); }

    // ---- Updates

    std::filesystem::path InstallDir() const {
        return PortFolder();
    }

    void PollUpdate() {
        if (updater_) updater_->Poll();
    }

    // The helper replaces this installation's programs once this process exits; a running game still holds them.
    bool InstallBlocked() {
        if (install_.running() || custom_levels_->importing()) {
            launcher_->SetStatus("Wait for the current install to finish.", 5.0);
            return true;
        }
        if (OtherProcessRunning(CurrentExecutablePath())) {
            launcher_->SetStatus("Close the game before installing the update.", 6.0);
            return true;
        }
        return false;
    }

    bool HandleUpdateAction(trg::update::UpdateUiAction action) {
        switch (action) {
        case trg::update::UpdateUiAction::kDownload:
            updater_->StartDownload();
            break;
        case trg::update::UpdateUiAction::kRetry:
            if (updater_->RetryNeedsInstall()) {
                if (InstallBlocked() || !launcher_->Save()) break;
                SavePlacement(placement_.Flush());
                if (updater_->Retry()) {
                    skip_finish_save_ = true;
                    return true;
                }
            } else {
                updater_->Retry();
            }
            break;
        case trg::update::UpdateUiAction::kRestart:
            if (InstallBlocked() || !launcher_->Save()) break;
            SavePlacement(placement_.Flush());
            if (updater_->BeginInstall()) {
                skip_finish_save_ = true;
                return true;
            }
            break;
        case trg::update::UpdateUiAction::kNone:
            break;
        }
        return false;
    }

    // ---- About

    void PageAbout(trg::Ui& ui) {
        ui.SponsorCard(
            "This open source PC port is maintained and developed in my free time. If you enjoy the project and would "
            "like to support its continued development, you can sponsor my work on GitHub.",
            "Support is completely optional and helps support ongoing improvements and future projects.");
#ifdef _WIN32
        ui.Paragraph("Jurassic Park: Trespasser Recompiled. PC port powered by static recompilation: the 1998 game "
                     "runs as native code on modern Windows, with a Direct3D 11 renderer. No game files are included; "
                     "the game runs from the player's own disc. Port by saulob, recompiled with pcrecomp by sp00nz. "
                     "Original game by DreamWorks Interactive.");
#else
        ui.Paragraph("Jurassic Park: Trespasser Recompiled. PC port powered by static recompilation: the 1998 game "
                     "runs as native code on modern Linux, with a Vulkan renderer. No game files are included; "
                     "the game runs from the player's own disc. Port by saulob, recompiled with pcrecomp by sp00nz. "
                     "Original game by DreamWorks Interactive.");
#endif
        ui.Spacer(4);
        std::error_code ec;
        if (const InstalledVersion* selected = Selected(); selected && std::filesystem::is_directory(selected->dir, ec)) {
            ui.FolderRow("Game files", "Where the selected game version is read from.", "Open game folder",
                         Utf8(selected->dir));
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
        const std::string rval = settings_.Get(JPT_KEY_RENDERER);
        const std::string renderer = rval == "software" ? "Software"
                                   : rval == "vulkan"   ? "Vulkan"
                                   : rval == "auto"     ? "Auto"
#ifdef _WIN32
                                                        : "Direct3D 11";
#else
                                                        : "Vulkan";
#endif
        ui.InfoGrid({{"Port version", JPT_PORT_VERSION},
                     {"Game", Selected() && Selected()->profile ? Tr(Selected()->profile->label())
                                                                : Tr(GameStateTitle(check_.state))},
                     {"Renderer", renderer},
#ifdef _WIN32
                     {"Platform", "Windows x86"}});
#else
                     {"Platform", "Linux x86_64"}});
#endif
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
    std::filesystem::path game_root_, user_dir_;
    GameRegistry registry_;
    std::vector<InstalledVersion> versions_;
    int selected_ = -1;
    GameCheck check_;
    LauncherHost host_;
    std::string failure_text_;
    Image header_art_image_;
    float header_art_aspect_ = kHeaderArtAspect;
    ImTextureID header_art_{};
    int header_art_width_ = 0;
    int header_art_height_ = 0;
    ImTextureID sponsor_icon_{};
    std::map<std::pair<PageId, int>, ImTextureID> page_icons_;
    std::unique_ptr<trg::Launcher> launcher_;
    std::unique_ptr<trg::update::UpdateController> updater_;
    std::unique_ptr<IncidentPage> incident_page_;
    std::unique_ptr<CustomLevelsPage> custom_levels_;
    jpt_custom_level_request_t level_{};
    bool skip_finish_save_ = false;
    trg::Fonts fonts_;
    PlacementTracker placement_;
    trg::Task install_;
    DiscBrowser disc_browser_;
    bool browse_replace_ = false;
    std::shared_ptr<InstallResult> outcome_;
    std::string install_failure_, install_failure_text_;
    // An incomplete folder import: its folders, which the disc's folder can complete, and what they lack.
    std::vector<std::filesystem::path> install_folders_;
    std::vector<std::string> install_missing_;
    std::vector<jpt_monitor_t> monitors_;
    GraphicsCards cards_;
    PlayGate play_;
    double play_started_ = 0.0;
    bool invert_ = false, invert_dirty_ = false;
    jpt_key_binding_t keymap_[JPT_KEYMAP_COUNT] = {};
};

jpt_launcher_result_t ReportLauncherFailure(const jpt_launcher_config_t& config, const std::string& detail, bool ready) {
    Log("could not open its window; the game was not started");
    std::string text = std::string(Tr("The launcher window could not be opened.")) + "\n\n" + detail + "\n\n" +
                       Tr("The game was not started.");
    if (!SoftwareRecovery(config)) {
        text += "\n" + TrFormat("To open the launcher with software rendering, start the program with: %s",
                                "--launcher --launcher-renderer software");
    }
    if (ready) text += "\n" + TrFormat("To start the game without the launcher, start the program with: %s", "--skip-launcher");
    if (config.log_file) text += std::string("\n\n") + config.log_file;
    ShowNativeError(kGameTitle, text);
    return JPT_LAUNCHER_FAILED;
}

} // namespace
} // namespace jpt

extern "C" const char* jpt_launcher_text(const char* english) { return jpt::Tr(english); }

extern "C" jpt_launcher_result_t jpt_launcher_run(const jpt_launcher_config_t* config) {
    using namespace jpt;
    SetLauncherLog(config->log);
    LauncherSettings settings(PathFromUtf8(config->settings_file));
    const std::vector<InstalledVersion> versions = FindInstalledVersions(PathFromUtf8(config->game_dir));
    const InstalledVersion* chosen = ChooseVersion(versions, settings.Get(JPT_KEY_GAME_VERSION));
    const bool ready = Playable(chosen);
    Log("game version: %s", chosen ? chosen->profile->label() : versions.empty() ? "none installed" : "none ready");
    const bool show_setting = settings.GetBool(JPT_KEY_SHOW_LAUNCHER, true);
    const bool shift = trg::ShiftHeld() || config->force_show != 0;
    if (!LauncherWanted(ready, show_setting, shift, config->skip_once != 0)) {
        Log("skipped (show_launcher %s%s)", show_setting ? "on" : "off", config->skip_once ? ", skip requested" : "");
        return JPT_LAUNCHER_PLAY;
    }
    Log("settings file %s", Utf8(settings.file()).c_str());
    Log("opening: %s%s%s", ready ? "game ready" : "game files not ready", show_setting ? ", show_launcher on" : "",
        shift ? ", Shift held or --launcher" : "");
    Log("startup: game renderer setting=%s", settings.Get(JPT_KEY_RENDERER).c_str());
    LauncherApp app(*config, settings);
    Outcome outcome = Outcome::kRunning;
    if (!app.Run(outcome)) return ReportLauncherFailure(*config, app.failure_text(), ready);
    return outcome == Outcome::kPlay ? JPT_LAUNCHER_PLAY : JPT_LAUNCHER_QUIT;
}

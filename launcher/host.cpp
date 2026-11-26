#include "host.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "displays.h"
#include "launcher_log.h"
#include "launcher_renderer.h"

#ifndef _WIN32
#include "jpt_app_identity.h"
#endif

namespace jpt {
namespace {

Rect ToRect(const SDL_Rect& rect) {
    return {rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
}

#ifdef _WIN32
constexpr bool kWindows = true;

bool PinModule(std::string_view name) {
    const std::wstring wide(name.begin(), name.end());
    HMODULE module = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE pinned = nullptr;
    return module && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCWSTR>(module), &pinned) != 0;
}

int64_t ThreadCpuNs() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
    const auto ticks = [](const FILETIME& time) { return (static_cast<int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) * 100;
}
#else
constexpr bool kWindows = false;

bool PinModule(std::string_view) {
    return false;
}

int64_t ThreadCpuNs() {
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return -1;
    return static_cast<int64_t>(time.tv_sec) * kNsPerSecond + time.tv_nsec;
}
#endif

int64_t NowNs() {
    return static_cast<int64_t>(SDL_GetTicksNS());
}

// The first module that could not be kept loaded; empty when all are.
std::string PinRendererModules(std::string_view renderer, int& pinned) {
    for (const std::string_view module : RendererModulesToPin(renderer)) {
        if (!PinModule(module)) return std::string(module);
        ++pinned;
    }
    return {};
}

std::string SdlError(const char* what) {
    const char* error = SDL_GetError();
    return std::string(what) + ": " + (error && *error ? error : "unknown error");
}

void LogLine(const std::string& line) {
    Log("%s", line.c_str());
}

} // namespace

#ifdef _WIN32
void ShowNativeError(const std::string& title, const std::string& text) {
    const auto wide = [](const std::string& utf8) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        std::wstring result(static_cast<size_t>(length > 0 ? length : 1), L'\0');
        if (length > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), length);
        return result;
    };
    MessageBoxW(nullptr, wide(text).c_str(), wide(title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
#else
void ShowNativeError(const std::string& title, const std::string& text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
}
#endif

// SDL presents software frames on Wayland, and by default on X11, through an accelerated texture. A plain framebuffer
// needs X11 (or XWayland); without it the Wayland path stays. SDL_Quit drops the hints before the game starts.
bool LauncherHost::StartVideo(bool plain_framebuffer) {
    const auto plain_driver = [] {
        const char* driver = SDL_GetCurrentVideoDriver();
        return !driver || SDL_strcmp(driver, "wayland") != 0;
    };
    if (plain_framebuffer) SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    if (video_ && (!plain_framebuffer || plain_driver())) return true;
    if (video_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    video_ = false;
#ifndef _WIN32
    if (plain_framebuffer) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
        video_ = SDL_InitSubSystem(SDL_INIT_VIDEO);
        if (!video_) SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    }
#endif
    if (!video_) video_ = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (video_ && plain_framebuffer && !plain_driver()) SDL_ResetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION);
    return video_;
}

bool LauncherHost::OpenWindow(const HostOptions& options, std::string& reason) {
    const std::vector<Display> displays = ListDisplays();
    if (displays.empty()) {
        reason = "no display found";
        return false;
    }
    std::vector<Rect> usable;
    for (const Display& display : displays) {
        usable.push_back(ToRect(display.usable));
    }

    const bool restore = options.restore && PlacementIsVisible(*options.restore, usable);
    restored_ = restore ? options.restore : std::nullopt;
    const Display* target = &displays.front();
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
    int width = 0, height = 0;
    if (restore) {
        const Rect& normal = options.restore->normal;
        const SDL_Point center{(normal.left + normal.right) / 2, (normal.top + normal.bottom) / 2};
        const SDL_DisplayID id = SDL_GetDisplayForPoint(&center);
        for (const Display& display : displays) {
            if (display.id == id) {
                target = &display;
            }
        }
        x = normal.left;
        y = normal.top;
        width = normal.width();
        height = normal.height();
    } else {
        x = SDL_WINDOWPOS_CENTERED_DISPLAY(target->id);
        y = SDL_WINDOWPOS_CENTERED_DISPLAY(target->id);
        width = static_cast<int>(options.width * target->scale);
        height = static_cast<int>(options.height * target->scale);
        FitWindowSize(width, height, *target);
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, options.title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    window_ = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window_) {
        reason = SdlError("window");
        return false;
    }
    SDL_SetWindowMinimumSize(window_, std::min(static_cast<int>(options.min_width * target->scale), width),
                             std::min(static_cast<int>(options.min_height * target->scale), height));
#ifndef _WIN32
    jpt_app_icon_apply(window_);
#endif
    Log("window %dx%d at %.2fx display scale", width, height, target->scale);
    return true;
}

// SDL_RenderPresent reports success whatever the driver answered, so the commands are flushed first and the error the
// present itself leaves behind is read.
std::string LauncherHost::PresentChecked() {
    if (!SDL_FlushRenderer(renderer_)) return SdlError("draw");
    SDL_ClearError();
    if (!SDL_RenderPresent(renderer_) || *SDL_GetError()) return SdlError("present");
    return {};
}

// What the first launcher frame needs: a texture the size of the font atlas, a clear, a draw and a present.
std::optional<RendererFailure> LauncherHost::ProbeFirstFrame() {
    constexpr int kSize = 512;
    const std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4, 255);
    SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kSize, kSize);
    if (!texture) return RendererFailure{RendererStage::kFirstFrame, SdlError("texture")};
    std::string reason;
    if (!SDL_UpdateTexture(texture, nullptr, pixels.data(), kSize * 4)) {
        reason = SdlError("texture upload");
    } else if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) || !SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255) ||
               !SDL_RenderClear(renderer_) || !SDL_RenderTexture(renderer_, texture, nullptr, nullptr)) {
        reason = SdlError("draw");
    } else {
        reason = PresentChecked();
    }
    SDL_DestroyTexture(texture);
    if (!reason.empty()) return RendererFailure{RendererStage::kFirstFrame, reason};
    return std::nullopt;
}

std::optional<RendererFailure> LauncherHost::OpenRenderer(const HostOptions& options, std::string_view name) {
    const std::string unpinned = PinRendererModules(name, pinned_modules_);
    if (!unpinned.empty()) return RendererFailure{RendererStage::kPin, "cannot keep " + unpinned + " loaded"};
    if (!StartVideo(name == kLauncherRendererSoftware)) return RendererFailure{RendererStage::kWindow, SdlError("SDL video")};
    std::string reason;
    if (!OpenWindow(options, reason)) return RendererFailure{RendererStage::kWindow, reason};

    const std::string sdl_name(name);
    renderer_ = SDL_CreateRenderer(window_, sdl_name.empty() ? nullptr : sdl_name.c_str());
    if (!renderer_) return RendererFailure{RendererStage::kCreate, SdlError("renderer")};
    const char* created = SDL_GetRendererName(renderer_);
    renderer_name_ = created ? created : "";
    if (std::optional<RendererFailure> failure = ProbeFirstFrame()) return failure;
    ConfigureVsync();

    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    sdl_backend_ = ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    if (!sdl_backend_) return RendererFailure{RendererStage::kBackend, "ImGui SDL backend"};
    renderer_backend_ = ImGui_ImplSDLRenderer3_Init(renderer_);
    if (!renderer_backend_) return RendererFailure{RendererStage::kBackend, "ImGui renderer backend"};
    return std::nullopt;
}

bool LauncherHost::Open(const HostOptions& options) {
    failure_text_.clear();
    first_frame_ = true;
    pinned_modules_ = 0;
#ifndef _WIN32
    jpt_app_identity_apply(nullptr);
#endif
    Log("startup: video");
    if (!StartVideo(options.renderer == kLauncherRendererSoftware)) {
        failure_text_ = SdlError("SDL video");
        Log("startup: failed: %s", failure_text_.c_str());
        Close();
        return false;
    }
    gamepad_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    LogLine(RendererRequestLine(options.renderer, kWindows));
    const RendererSelection selection = SelectLauncherRenderer(
        options.renderer, kWindows,
        [&](std::string_view name) {
            LogLine("startup: renderer trying=" + AttemptLabel(name));
            std::optional<RendererFailure> failure = OpenRenderer(options, name);
            LogLine(RendererAttemptLine({std::string(name), failure}));
            return failure;
        },
        [this] { ReleaseWindow(); }, [this] { return renderer_name_; });
    LogLine(RendererResultLine(selection));
    if (!selection.opened) {
        failure_text_ = RendererFailureText(selection);
        Close();
        return false;
    }
    LogLine(RendererSelectedLine(renderer_name_, selection.fallback, pinned_modules_));

    hidden_ = options.hidden;
    pacer_ = FramePacer(hidden_ ? 0 : kLauncherFrameNs);
    monitor_ = LoopMonitor();
    if (restored_ && restored_->maximized) {
        SDL_MaximizeWindow(window_);
    }
    if (restored_ && restored_->fullscreen) {
        SDL_SetWindowFullscreen(window_, true);
    }
    return true;
}

// The software renderer's VSync on a Windows window is only simulated by SDL, so it is turned off there and the frame cap
// alone paces the loop.
void LauncherHost::ConfigureVsync() {
    VsyncState state;
    state.requested = true;
    const bool set = SDL_SetRenderVSync(renderer_, 1);
    int value = 0;
    SDL_GetRenderVSync(renderer_, &value);
    int surface_value = 0;
    const bool software = renderer_name_ == SDL_SOFTWARE_RENDERER;
    const bool real = !software || (SDL_GetWindowSurfaceVSync(window_, &surface_value) && surface_value != 0);
    state.effective = set && value == 1 && real;
    if (set && !state.effective) SDL_SetRenderVSync(renderer_, 0);
    Log("%s", PacingLine(renderer_name_, state).c_str());
}

void* LauncherHost::NativeWindow() const {
#ifdef _WIN32
    if (window_) return SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    return nullptr;
}

void LauncherHost::Show() {
    if (!window_ || hidden_) {
        return;
    }
    SDL_ShowWindow(window_);
    SDL_RaiseWindow(window_);
}

ImTextureID LauncherHost::CreateTexture(const uint8_t* rgba, int width, int height) {
    if (!renderer_ || !rgba || width <= 0 || height <= 0) {
        return ImTextureID{};
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        return ImTextureID{};
    }
    if (!SDL_UpdateTexture(texture, nullptr, rgba, width * 4)) {
        SDL_DestroyTexture(texture);
        return ImTextureID{};
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    textures_.push_back(texture);
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
}

void LauncherHost::DestroyTexture(ImTextureID texture) {
    SDL_Texture* sdl_texture = reinterpret_cast<SDL_Texture*>(static_cast<uintptr_t>(texture));
    const auto found = std::find(textures_.begin(), textures_.end(), sdl_texture);
    if (found != textures_.end()) {
        SDL_DestroyTexture(sdl_texture);
        textures_.erase(found);
    }
}

// Whatever a renderer attempt opened, newest first. Safe after a partial attempt.
void LauncherHost::ReleaseWindow() {
    for (SDL_Texture* texture : textures_) {
        SDL_DestroyTexture(texture);
    }
    textures_.clear();
    if (renderer_backend_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        renderer_backend_ = false;
    }
    if (sdl_backend_) {
        ImGui_ImplSDL3_Shutdown();
        sdl_backend_ = false;
    }
    if (imgui_) {
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    renderer_name_.clear();
}

void LauncherHost::Close() {
    ReleaseWindow();
    if (video_ || gamepad_) {
        // Nothing else uses SDL yet: the game window thread starts it again from scratch.
        SDL_Quit();
        video_ = gamepad_ = false;
    }
}

bool LauncherHost::PumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
        case SDL_EVENT_QUIT:
            quit_ = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == SDL_GetWindowID(window_)) {
                quit_ = true;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_F11 && !event.key.repeat && !(event.key.mod & SDL_KMOD_CTRL)) {
                SetFullscreen(!Fullscreen());
            }
            break;
        default:
            break;
        }
    }
    return !quit_;
}

std::vector<ConnectedGamepad> LauncherHost::Gamepads() const {
    std::vector<ConnectedGamepad> pads;
    if (!gamepad_) {
        return pads;
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        pads.push_back({name ? name : "Unnamed controller", static_cast<int>(SDL_GetGamepadTypeForID(ids[i])),
                        SDL_GetGamepadVendorForID(ids[i]), SDL_GetGamepadProductForID(ids[i])});
    }
    SDL_free(ids);
    return pads;
}

bool LauncherHost::Minimized() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

bool LauncherHost::WaitForFrame() {
    for (;;) {
        if (!PumpEvents()) return false;
        const int64_t polled = NowNs();
        if (Minimized()) {
            pacer_.Reset();
            monitor_.Reset();
            SDL_Delay(kMinimizedSleepMs);
            continue;
        }
        const FrameWait next = pacer_.Next(polled);
        if (next.draw) return true;
        SDL_DelayNS(static_cast<Uint64>(next.wait_ns));
        monitor_.Waited(NowNs() - polled);
    }
}

void LauncherHost::NoteFrameDrawn(int64_t now_ns, const FrameTimes& times) {
    if (pacer_.interval_ns() <= 0) return;
    if (const std::optional<LoopSample> sample = monitor_.FrameDrawn(now_ns, times, ThreadCpuNs())) {
        Log("%s", LoopSampleLine(renderer_name_, *sample).c_str());
    }
}

void LauncherHost::BeginFrame() {
    frame_begin_ns_ = NowNs();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    const float scale = SDL_GetWindowDisplayScale(window_);
    ImGui::GetStyle().FontScaleDpi = scale > 0.0f ? scale : 1.0f;
    ImGui::NewFrame();
}

bool LauncherHost::EndFrame(const ImVec4& clear, const char* capture_path) {
    const int64_t submitting = NowNs();
    ImGui::Render();
    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(renderer_, clear.x, clear.y, clear.z, 1.0f);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (capture_path) {
        if (SDL_Surface* frame = SDL_RenderReadPixels(renderer_, nullptr)) {
            SDL_SavePNG(frame, capture_path);
            SDL_DestroySurface(frame);
        }
    }
    const int64_t presenting = NowNs();
    if (first_frame_) {
        first_frame_ = false;
        failure_text_ = PresentChecked();
        for (const ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->Status == ImTextureStatus_OK && texture->TexID == ImTextureID_Invalid) failure_text_ = SdlError("font texture");
        }
        return failure_text_.empty();
    }
    SDL_RenderPresent(renderer_);
    const int64_t presented = NowNs();
    NoteFrameDrawn(presented, {submitting - frame_begin_ns_, presenting - submitting, presented - presenting});
    return true;
}

bool LauncherHost::Fullscreen() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

void LauncherHost::SetFullscreen(bool fullscreen) {
    SDL_SetWindowFullscreen(window_, fullscreen);
}

std::optional<WindowState> LauncherHost::State() const {
    if (!window_) {
        return std::nullopt;
    }
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &width, &height);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    return WindowState{{x, y, x + width, y + height}, (flags & SDL_WINDOW_MAXIMIZED) != 0,
                       (flags & SDL_WINDOW_MINIMIZED) != 0};
}

} // namespace jpt

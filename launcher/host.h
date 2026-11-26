// The launcher's window: SDL3, an SDL renderer and Dear ImGui. It exists only until Play or Quit, before the host
// creates the game window and its Direct3D 11 device, so only one SDL window and one ImGui context are alive at a time.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "frame_pacer.h"
#include "launcher_renderer.h"
#include "placement.h"

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace jpt {

struct HostOptions {
    std::string title;
    int width = 1280;  // logical, scaled by the display's content scale
    int height = 720;
    int min_width = 900;
    int min_height = 560;
    std::optional<WindowPlacement> restore;  // used when it lies on a display
    bool hidden = false;                     // page screenshots without showing the window
    std::string renderer = std::string(kLauncherRendererAuto); // --launcher-renderer
};

// The system's own message box: needs no renderer and no open window.
void ShowNativeError(const std::string& title, const std::string& text);

struct ConnectedGamepad {
    std::string name;
    int type = 0;  // SDL_GamepadType
    uint16_t vendor = 0, product = 0;
};

class LauncherHost {
public:
    LauncherHost() = default;
    ~LauncherHost() { Close(); }
    LauncherHost(const LauncherHost&) = delete;
    LauncherHost& operator=(const LauncherHost&) = delete;

    // False when no window or renderer could be created; nothing is left open then and FailureText says why.
    bool Open(const HostOptions& options);
    const std::string& FailureText() const { return failure_text_; }
    // The SDL renderer in use; empty when none is open.
    const std::string& RendererName() const { return renderer_name_; }
    // Shows the window once its first frame has been drawn. Does nothing when it was opened hidden.
    void Show();
    void Close();
    // The HWND on Windows while the window is open; nullptr elsewhere.
    void* NativeWindow() const;

    // Handles the window's events (F11 switches the launcher's own fullscreen) and sleeps until the next frame is due, at
    // most 60 per second; a hidden window is not limited. False once the window was closed.
    bool WaitForFrame();

    // Between these two the caller draws the launcher with ImGui. A capture path saves the frame as a PNG. False only when
    // the first frame could not be drawn, uploaded or presented; FailureText says why.
    void BeginFrame();
    bool EndFrame(const ImVec4& clear, const char* capture_path = nullptr);

    bool Fullscreen() const;
    void SetFullscreen(bool fullscreen);
    std::optional<WindowState> State() const;

    // A picture for ImGui to draw (RGBA8, width * height * 4 bytes). The host owns it until Close; 0 when it could not be made.
    ImTextureID CreateTexture(const uint8_t* rgba, int width, int height);
    void DestroyTexture(ImTextureID texture);

    // The controllers SDL sees now, in connection order; empty when its gamepad support could not start.
    std::vector<ConnectedGamepad> Gamepads() const;

    // The saved placement the window was opened with, when it lay on a display.
    const std::optional<WindowPlacement>& Restored() const { return restored_; }

private:
    bool StartVideo(bool plain_framebuffer);
    bool OpenWindow(const HostOptions& options, std::string& reason);
    std::optional<RendererFailure> OpenRenderer(const HostOptions& options, std::string_view name);
    std::optional<RendererFailure> ProbeFirstFrame();
    std::string PresentChecked();
    void ReleaseWindow();
    bool PumpEvents();
    bool Minimized() const;
    void ConfigureVsync();
    void NoteFrameDrawn(int64_t now_ns, const FrameTimes& times);

    std::optional<WindowPlacement> restored_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    std::vector<SDL_Texture*> textures_;
    bool sdl_backend_ = false;
    bool renderer_backend_ = false;
    bool first_frame_ = true;
    int pinned_modules_ = 0;
    std::string failure_text_;
    bool video_ = false;
    bool gamepad_ = false;
    bool hidden_ = false;
    bool quit_ = false;
    FramePacer pacer_;
    LoopMonitor monitor_;
    std::string renderer_name_;
    int64_t frame_begin_ns_ = 0;
};

} // namespace jpt

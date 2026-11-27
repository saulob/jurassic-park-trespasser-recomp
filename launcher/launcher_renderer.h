// Which SDL renderer the launcher's own window uses. The game's renderer is chosen elsewhere and is not affected.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jpt {

// The --launcher-renderer values (jpt_launcher_renderer_valid).
inline constexpr std::string_view kLauncherRendererAuto = "auto";
inline constexpr std::string_view kLauncherRendererSoftware = "software";
inline constexpr std::string_view kLauncherRendererDirect3D11 = "direct3d11";

// SDL renderer names in the order they are tried; an empty name lets SDL pick its first working renderer. A renderer
// asked for by name is the only one tried.
inline std::vector<std::string_view> LauncherRendererOrder(bool windows, std::string_view request = kLauncherRendererAuto) {
    if (request == kLauncherRendererSoftware) return {kLauncherRendererSoftware};
    if (!windows) return {"", kLauncherRendererSoftware};
    if (request == kLauncherRendererDirect3D11) return {kLauncherRendererDirect3D11};
    return {kLauncherRendererDirect3D11, kLauncherRendererSoftware};
}

// System modules a renderer loads that must stay mapped until the process exits: SDL unloads d3d11.dll and dxgi.dll right
// after the device's last Release, while an NVIDIA driver thread can still be returning through them. Windows only: no
// Windows DLL name may reach the Linux executable.
inline std::vector<std::string_view> RendererModulesToPin(std::string_view renderer) {
#ifdef _WIN32
    if (renderer == kLauncherRendererDirect3D11) return {"d3d11.dll", "dxgi.dll"};
#else
    (void)renderer;
#endif
    return {};
}

enum class RendererStage { kPin, kWindow, kCreate, kFirstFrame, kBackend };

inline const char* RendererStageName(RendererStage stage) {
    switch (stage) {
    case RendererStage::kPin: return "pin";
    case RendererStage::kWindow: return "window";
    case RendererStage::kCreate: return "create";
    case RendererStage::kFirstFrame: return "first_frame";
    case RendererStage::kBackend: return "imgui_backend";
    }
    return "unknown";
}

struct RendererFailure {
    RendererStage stage = RendererStage::kCreate;
    std::string reason;
};

struct RendererAttempt {
    std::string name;
    std::optional<RendererFailure> failure;
};

struct RendererSelection {
    std::string requested;
    std::vector<RendererAttempt> attempts;
    std::string effective;
    bool opened = false;
    bool fallback = false;
};

inline std::string AttemptLabel(std::string_view name) {
    return name.empty() ? "accelerated" : std::string(name);
}

// Tries the renderers of the order until `open` accepts one. A failed attempt is followed by `discard`: whatever it left
// open must be gone before the next one starts. `opened_name` names the renderer SDL really opened.
inline RendererSelection SelectLauncherRenderer(std::string_view requested, bool windows,
                                                const std::function<std::optional<RendererFailure>(std::string_view)>& open,
                                                const std::function<void()>& discard,
                                                const std::function<std::string()>& opened_name) {
    RendererSelection selection;
    selection.requested = std::string(requested);
    for (const std::string_view name : LauncherRendererOrder(windows, requested)) {
        RendererAttempt attempt{std::string(name), open(name)};
        const bool failed = attempt.failure.has_value();
        selection.attempts.push_back(std::move(attempt));
        if (!failed) {
            selection.opened = true;
            selection.effective = opened_name();
            selection.fallback = selection.attempts.size() > 1 ||
                                 (requested != kLauncherRendererSoftware && selection.effective == kLauncherRendererSoftware);
            return selection;
        }
        discard();
    }
    return selection;
}

inline std::string RendererRequestLine(std::string_view requested, bool windows) {
    std::string order;
    for (const std::string_view name : LauncherRendererOrder(windows, requested)) {
        order += (order.empty() ? "" : ",") + AttemptLabel(name);
    }
    return "startup: renderer requested=" + std::string(requested) + " order=" + order;
}

inline std::string RendererAttemptLine(const RendererAttempt& attempt) {
    const std::string line = "startup: renderer attempt=" + AttemptLabel(attempt.name);
    if (!attempt.failure) return line + " result=ok";
    return line + " result=failed stage=" + RendererStageName(attempt.failure->stage) + " reason=" + attempt.failure->reason;
}

inline std::string RendererResultLine(const RendererSelection& selection) {
    if (!selection.opened) return "startup: renderer effective=none";
    std::string line = "startup: renderer effective=" + selection.effective + " fallback=" + (selection.fallback ? "1" : "0");
    if (selection.attempts.size() > 1) {
        const RendererAttempt& first = selection.attempts.front();
        line += " fallback_reason=" + AttemptLabel(first.name) + "_" + RendererStageName(first.failure->stage);
    }
    return line;
}

// Each renderer tried and why it failed, for the message shown when none opens.
inline std::string RendererFailureText(const RendererSelection& selection) {
    std::string text;
    for (const RendererAttempt& attempt : selection.attempts) {
        if (!attempt.failure) continue;
        text += (text.empty() ? "" : "\n") + AttemptLabel(attempt.name) + ": " + attempt.failure->reason;
    }
    return text;
}

} // namespace jpt

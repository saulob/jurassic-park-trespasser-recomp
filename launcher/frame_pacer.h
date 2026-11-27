// Frame cap of the launcher window and the one bounded log sample of how the loop behaves.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace jpt {

constexpr int kLauncherTargetHz = 60;
constexpr int64_t kNsPerSecond = 1'000'000'000;
constexpr int64_t kNsPerMs = 1'000'000;
constexpr int64_t kLauncherFrameNs = kNsPerSecond / kLauncherTargetHz;
constexpr int kMinimizedSleepMs = 30;

struct FrameWait {
    bool draw = false;
    int64_t wait_ns = 0;
};

// Deadline-based cap on a monotonic clock, so that a high refresh rate does not rebuild the UI at that rate and a late
// frame is not followed by a burst. An interval of 0 never waits.
class FramePacer {
public:
    explicit FramePacer(int64_t interval_ns = kLauncherFrameNs) : interval_(interval_ns) {}

    FrameWait Next(int64_t now_ns) {
        if (interval_ <= 0) return {true, 0};
        if (!scheduled_) {
            scheduled_ = true;
            deadline_ = now_ns + interval_;
            return {true, 0};
        }
        if (deadline_ - now_ns > interval_) deadline_ = now_ns + interval_;
        if (now_ns < deadline_) return {false, deadline_ - now_ns};
        deadline_ += interval_;
        if (deadline_ <= now_ns) deadline_ = now_ns + interval_;
        return {true, 0};
    }

    void Reset() { scheduled_ = false; }
    int64_t interval_ns() const { return interval_; }

private:
    int64_t interval_;
    int64_t deadline_ = 0;
    bool scheduled_ = false;
};

struct VsyncState {
    bool requested = false;
    bool effective = false;
};

// Wall-clock time of one frame: building the UI, recording the draw commands, and presenting, which executes them and
// may wait for VSync.
struct FrameTimes {
    int64_t build_ns = 0;
    int64_t submit_ns = 0;
    int64_t present_ns = 0;
};

struct LoopSample {
    int64_t frames = 0;
    int64_t elapsed_ns = 0;
    double hz = 0.0;
    FrameTimes total;
    int64_t wait_ns = 0;
    int64_t max_present_ns = 0;
    int cpu_pct = -1; // the UI thread's own CPU time, -1 when unavailable
};

// One sample, taken after the launcher has been visible long enough to be stable; nothing more is reported afterwards.
class LoopMonitor {
public:
    static constexpr int64_t kWarmupNs = 5 * kNsPerSecond;
    static constexpr int64_t kWindowNs = 5 * kNsPerSecond;

    void Waited(int64_t ns) {
        if (windowed_) wait_ += ns;
    }

    std::optional<LoopSample> FrameDrawn(int64_t now_ns, const FrameTimes& times, int64_t thread_cpu_ns = -1) {
        if (done_) return std::nullopt;
        if (!visible_) {
            visible_ = true;
            visible_since_ = now_ns;
        }
        if (!windowed_) {
            if (now_ns - visible_since_ >= kWarmupNs) StartWindow(now_ns, thread_cpu_ns);
            return std::nullopt;
        }
        ++frames_;
        total_.build_ns += times.build_ns;
        total_.submit_ns += times.submit_ns;
        total_.present_ns += times.present_ns;
        max_present_ = std::max(max_present_, times.present_ns);
        const int64_t elapsed = now_ns - window_start_;
        if (elapsed < kWindowNs) return std::nullopt;
        LoopSample sample;
        sample.frames = frames_;
        sample.elapsed_ns = elapsed;
        sample.hz = double(frames_) * double(kNsPerSecond) / double(elapsed);
        sample.total = total_;
        sample.wait_ns = wait_;
        sample.max_present_ns = max_present_;
        if (thread_cpu_ns >= 0 && window_cpu_ >= 0) {
            sample.cpu_pct = int(std::clamp<int64_t>((thread_cpu_ns - window_cpu_) * 100 / elapsed, 0, 100));
        }
        done_ = true;
        return sample;
    }

    // The window was hidden or minimised: the warm-up starts again.
    void Reset() {
        visible_ = false;
        windowed_ = false;
    }

private:
    void StartWindow(int64_t now_ns, int64_t thread_cpu_ns) {
        windowed_ = true;
        window_start_ = now_ns;
        window_cpu_ = thread_cpu_ns;
        frames_ = 0;
        total_ = {};
        wait_ = 0;
        max_present_ = 0;
    }

    bool visible_ = false;
    bool windowed_ = false;
    bool done_ = false;
    int64_t visible_since_ = 0;
    int64_t window_start_ = 0;
    int64_t window_cpu_ = -1;
    int64_t frames_ = 0;
    FrameTimes total_;
    int64_t wait_ = 0;
    int64_t max_present_ = 0;
};

inline std::string RendererLabel(std::string_view name) {
    std::string label;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        label += plain ? c : (c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : '_');
    }
    return label.empty() ? "unknown" : label;
}

inline std::string RendererSelectedLine(std::string_view renderer, bool fallback, int pinned_modules) {
    char line[160];
    std::snprintf(line, sizeof line, "renderer=%s fallback=%d pinned_modules=%d", RendererLabel(renderer).c_str(),
                  fallback ? 1 : 0, pinned_modules);
    return line;
}

inline std::string PacingLine(std::string_view renderer, const VsyncState& vsync) {
    char line[160];
    std::snprintf(line, sizeof line, "frame pacing: renderer=%s requested_vsync=%d effective_vsync=%d target_hz=%d",
                  RendererLabel(renderer).c_str(), vsync.requested ? 1 : 0, vsync.effective ? 1 : 0, kLauncherTargetHz);
    return line;
}

inline std::string LoopSampleLine(std::string_view renderer, const LoopSample& s) {
    const auto average_ms = [&s](int64_t ns) { return s.frames > 0 ? double(ns) / double(s.frames) / double(kNsPerMs) : 0.0; };
    char line[256];
    std::snprintf(line, sizeof line,
                  "loop sample: renderer=%s frames=%lld hz=%.1f build_ms=%.1f submit_ms=%.1f present_ms=%.1f wait_ms=%.1f "
                  "max_present_ms=%.1f ui_cpu_pct=%d",
                  RendererLabel(renderer).c_str(), static_cast<long long>(s.frames), s.hz, average_ms(s.total.build_ns),
                  average_ms(s.total.submit_ns), average_ms(s.total.present_ns), average_ms(s.wait_ns),
                  double(s.max_present_ns) / double(kNsPerMs), s.cpu_pct);
    return line;
}

} // namespace jpt

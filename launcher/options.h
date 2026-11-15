// The launcher's pages, the port's identity and how the Display and Graphics pages label their choices.
#pragma once

#include <string>
#include <vector>

#include "jpt_display.h"
#include "jpt_monitors.h"

namespace jpt {

inline constexpr char kGameTitle[] = "Jurassic Park: Trespasser Recompiled";
inline constexpr char kPortRepository[] = "github.com/saulob/jurassic-park-trespasser-recomp";
inline constexpr char kAuthorUrl[] = "https://github.com/saulob";
inline constexpr char kSponsorUrl[] = "https://github.com/sponsors/saulob";

enum class PageId { kPlay, kDisplay, kGraphics, kGameplay, kControls, kCheats, kBugReport, kAbout };

inline constexpr PageId kPageOrder[] = {PageId::kPlay,     PageId::kDisplay, PageId::kGraphics,  PageId::kGameplay,
                                        PageId::kControls, PageId::kCheats,  PageId::kBugReport, PageId::kAbout};

const char* PageName(PageId page);
// Pages that are listed but have nothing to set yet.
bool PageComingLater(PageId page);

// "1440p — 2560 × 1440", or "Original (compatibility size)".
std::string ResolutionLabel(int preset, jpt_size_t box);

// "1920 × 1080", or "Auto (the picture at the monitor's scale)".
std::string WindowSizeLabel(const jpt_window_size_t& size);
// Whether the window fits the monitor's work area as it is.
bool WindowSizeFits(const jpt_window_size_t& size, const jpt_monitor_t& monitor);

// "Display 2 (primary)   2560 × 1440   Name". Monitor settings are 1-based.
std::string MonitorLabel(int monitor, const std::vector<jpt_monitor_t>& monitors);

} // namespace jpt

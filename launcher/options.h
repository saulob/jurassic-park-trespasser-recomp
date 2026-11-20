// The launcher's pages, the port's identity and how the Display and Graphics pages label their choices.
#pragma once

#include <string>
#include <vector>

#include "jpt_cheats.h"
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

inline constexpr char kCheatsPageSubtitle[] = "Optional gameplay modifiers";
inline constexpr char kCheatsHelp[] =
    "Press F10 while playing to open the Cheats menu. Changes made there apply only to the current session: the values "
    "below are what every session starts with. Heal, Teleport, Level Select, Fly / Free Camera, Noclip and Stealth Mode are only in that menu.";

// The cheats the page shows, in F10's order: the saved ones of jpt_cheats.h.
std::vector<const jpt_cheat_info_t*> LauncherCheats();
// The value a cheat's setting is stored as, for text the file may spell another way ("1.0" for 1); the default for a
// text that is not one of its values.
std::string CanonicalCheatValue(const jpt_cheat_info_t& cheat, const std::string& text);

// "1440p — 2560 × 1440", or "Original (compatibility size)".
std::string ResolutionLabel(int preset, jpt_size_t box);

// "1920 × 1080", or "Auto (the picture at the monitor's scale)".
std::string WindowSizeLabel(const jpt_window_size_t& size);
// Whether the window fits the monitor's work area as it is.
bool WindowSizeFits(const jpt_window_size_t& size, const jpt_monitor_t& monitor);

// Water refraction and Enhanced sky: the Software renderer draws water and sky as the original, so they are greyed out
// there and keep their stored values for Direct3D 11.
inline constexpr char kDirect3DOnlyHelp[] = "Available with the Direct3D 11 renderer.";
bool Direct3DEffectsAvailable(const std::string& renderer);

// "Display 2 (primary)   2560 × 1440   Name". Monitor settings are 1-based.
std::string MonitorLabel(int monitor, const std::vector<jpt_monitor_t>& monitors);

} // namespace jpt

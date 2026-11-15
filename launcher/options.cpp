#include "options.h"

#include "localization/ui_text.h"

namespace jpt {
namespace {

constexpr char kTimes[] = " \xC3\x97 ";
constexpr char kDash[] = " \xE2\x80\x94 ";

std::string Dimensions(int width, int height) { return std::to_string(width) + kTimes + std::to_string(height); }

} // namespace

const char* PageName(PageId page) {
    switch (page) {
    case PageId::kPlay: return "Play";
    case PageId::kDisplay: return "Display";
    case PageId::kGraphics: return "Graphics";
    case PageId::kGameplay: return "Gameplay";
    case PageId::kControls: return "Controls";
    case PageId::kCheats: return "Cheats";
    case PageId::kBugReport: return "Bug Report";
    case PageId::kAbout: return "About";
    }
    return "";
}

bool PageComingLater(PageId page) { return page == PageId::kCheats || page == PageId::kBugReport; }

std::string ResolutionLabel(int preset, jpt_size_t box) {
    std::string label = Tr(jpt_render_presets[preset].label);
    if (preset == JPT_RENDER_ORIGINAL) return label + "   " + Tr("(compatibility size)");
    if (box.width > 0) label += kDash + Dimensions(box.width, box.height);
    return label;
}

std::string WindowSizeLabel(const jpt_window_size_t& size) {
    if (size.width <= 0) return Tr("Auto (the picture at the monitor's scale)");
    return Dimensions(size.width, size.height);
}

bool WindowSizeFits(const jpt_window_size_t& size, const jpt_monitor_t& monitor) {
    if (size.width <= 0) return true;
    const jpt_size_t requested{size.width, size.height};
    const jpt_size_t fitted = jpt_fit_window(requested, 0, 0, monitor.work.right - monitor.work.left,
                                             monitor.work.bottom - monitor.work.top);
    return fitted.width == requested.width && fitted.height == requested.height;
}

std::string MonitorLabel(int monitor, const std::vector<jpt_monitor_t>& monitors) {
    const std::string number = std::to_string(monitor);
    if (monitor < 1 || monitor > static_cast<int>(monitors.size())) {
        return Tr("Display ") + number + Tr(" (not connected)");
    }
    const jpt_monitor_t& m = monitors[static_cast<size_t>(monitor - 1)];
    std::string label = Tr("Display ") + number + (m.primary ? Tr(" (primary)") : "") + "   " +
                        Dimensions(m.bounds.right - m.bounds.left, m.bounds.bottom - m.bounds.top);
    if (m.name[0] && std::string(m.name) != "Generic PnP Monitor") label += std::string("   ") + m.name;
    return label;
}

} // namespace jpt

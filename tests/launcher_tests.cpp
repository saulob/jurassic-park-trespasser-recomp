// Headless checks of the launcher and display logic: settings, render sizes, Hor+, presentation, monitors, MSAA,
// the ISO and InstallShield readers, disc identity, the installer's staging, rollback, Cancel, re-import and repair,
// the installed versions, the release string table, the importer on the shapes of the four profiles (no original
// executable on the disc, then with it) and the Play gate. Disc images are synthetic and built in a temporary
// folder; TRESPASSER_TEST_ISO=<image>[;<image>...] also identifies real discs (read only).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "game_files.h"
#include "game_install.h"
#include "inflate.h"
#include "is_cabinet.h"
#include "iso9660.h"
#include "jpt_game_version.h"
#include "jpt_hash.h"
#include "localization/ui_text.h"
#include "jpt_config.h"
#include "jpt_display.h"
#include "jpt_gamepad.h"
#include "jpt_l10n.h"
#include "jpt_prompt_text.h"
#include "jpt_string_table.h"
#include "jpt_user_data.h"
#include "options.h"
#include "user_data.h"
#include "page_icons.h"
#include "play_gate.h"
#include "settings.h"
#include "system_info.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace jpt;

static int g_checks, g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

static void CheckSize(jpt_size_t s, int w, int h) {
    ++g_checks;
    if (s.width != w || s.height != h) {
        ++g_failures;
        std::printf("FAIL size %dx%d, expected %dx%d\n", s.width, s.height, w, h);
    }
}

// ---- synthetic ISO 9660 images

struct IsoInput {
    std::string path;  // '/' separated, as it should read back
    std::string content;
};

class IsoBuilder {
public:
    std::vector<unsigned char> Build(const std::string& volume, const std::vector<IsoInput>& files, bool joliet) {
        Dir root;
        for (const IsoInput& f : files) Add(root, f.path, &f);
        uint32_t next = joliet ? 19 : 18;
        Assign(root, next, false);
        if (joliet) Assign(root, next, true);
        for (const IsoInput& f : files) {
            extents_[&f] = next;
            next += static_cast<uint32_t>((f.content.size() + 2047) / 2048);
            if (f.content.empty()) next++;
        }
        image_.assign(size_t(next) * 2048, 0);
        Descriptor(16, 1, volume, root, false, next);
        if (joliet) Descriptor(17, 2, volume, root, true, next);
        unsigned char* end = &image_[(joliet ? 18 : 17) * 2048];
        end[0] = 255;
        std::memcpy(end + 1, "CD001", 5);
        end[6] = 1;
        Write(root, root, false);
        if (joliet) Write(root, root, true);
        for (const IsoInput& f : files) std::memcpy(&image_[size_t(extents_[&f]) * 2048], f.content.data(), f.content.size());
        return image_;
    }

private:
    struct Dir {
        std::map<std::string, Dir> dirs;
        std::map<std::string, const IsoInput*> files;
        uint32_t extent[2] = {};
    };

    static void Add(Dir& dir, const std::string& path, const IsoInput* f) {
        const size_t slash = path.find('/');
        if (slash == std::string::npos) dir.files[path] = f;
        else Add(dir.dirs[path.substr(0, slash)], path.substr(slash + 1), f);
    }

    void Assign(Dir& dir, uint32_t& next, bool joliet) {
        dir.extent[joliet] = next++;
        for (auto& [name, child] : dir.dirs) Assign(child, next, joliet);
    }

    static void Put32(unsigned char* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            p[i] = static_cast<unsigned char>(v >> (8 * i));
            p[7 - i] = static_cast<unsigned char>(v >> (8 * i));
        }
    }

    static std::string Name(const std::string& name, bool joliet, bool file) {
        if (!joliet) return file ? name + ";1" : name;
        std::string out;
        for (char c : name) {
            out.push_back('\0');
            out.push_back(c);
        }
        return out;
    }

    static size_t Record(unsigned char* p, const std::string& name, uint32_t extent, uint32_t size, bool dir) {
        const size_t len = (33 + name.size() + 1) & ~size_t(1);
        p[0] = static_cast<unsigned char>(len);
        Put32(p + 2, extent);
        Put32(p + 10, size);
        p[25] = dir ? 2 : 0;
        p[28] = 1;
        p[31] = 1;
        p[32] = static_cast<unsigned char>(name.size());
        std::memcpy(p + 33, name.data(), name.size());
        return len;
    }

    void Write(const Dir& dir, const Dir& parent, bool joliet) {
        unsigned char* p = &image_[size_t(dir.extent[joliet]) * 2048];
        size_t pos = Record(p, std::string(1, '\0'), dir.extent[joliet], 2048, true);
        pos += Record(p + pos, std::string(1, '\1'), parent.extent[joliet], 2048, true);
        for (const auto& [name, child] : dir.dirs)
            pos += Record(p + pos, Name(name, joliet, false), child.extent[joliet], 2048, true);
        for (const auto& [name, f] : dir.files)
            pos += Record(p + pos, Name(name, joliet, true), extents_[f], static_cast<uint32_t>(f->content.size()), false);
        for (const auto& [name, child] : dir.dirs) Write(child, dir, joliet);
    }

    void Descriptor(uint32_t sector, unsigned char type, const std::string& volume, const Dir& root, bool joliet,
                    uint32_t sectors) {
        unsigned char* d = &image_[size_t(sector) * 2048];
        d[0] = type;
        std::memcpy(d + 1, "CD001", 5);
        d[6] = 1;
        std::memset(d + 40, ' ', 32);
        std::memcpy(d + 40, volume.data(), std::min<size_t>(volume.size(), 32));
        Put32(d + 80, sectors);
        if (joliet) std::memcpy(d + 88, "%/E", 3);
        Record(d + 156, std::string(1, '\0'), root.extent[joliet], 2048, true);
    }

    std::vector<unsigned char> image_;
    std::map<const IsoInput*, uint32_t> extents_;
};

static void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static void WriteImage(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

// ---- tests

static void TestRenderPresets() {
    const int k4x3 = jpt_aspect_index("4:3"), k16x9 = jpt_aspect_index("16:9"), k16x10 = jpt_aspect_index("16:10"),
              k21x9 = jpt_aspect_index("21:9"), k32x9 = jpt_aspect_index("32:9");
    const int p720 = jpt_render_preset_index("720p"), p1080 = jpt_render_preset_index("1080p"),
              p1440 = jpt_render_preset_index("1440p"), p4k = jpt_render_preset_index("4k");
    CheckSize(jpt_render_box(p720, k16x9, 0, 0), 1280, 720);
    CheckSize(jpt_render_box(p1080, k16x9, 0, 0), 1920, 1080);
    CheckSize(jpt_render_box(p1440, k16x9, 0, 0), 2560, 1440);
    CheckSize(jpt_render_box(p4k, k16x9, 0, 0), 3840, 2160);
    CheckSize(jpt_render_box(p720, k16x10, 0, 0), 1280, 800);
    CheckSize(jpt_render_box(p1080, k16x10, 0, 0), 1920, 1200);
    CheckSize(jpt_render_box(p1440, k16x10, 0, 0), 2560, 1600);
    CheckSize(jpt_render_box(p4k, k16x10, 0, 0), 3840, 2400);
    CheckSize(jpt_render_box(p1080, k4x3, 0, 0), 1920, 1440);
    CheckSize(jpt_render_box(p4k, k4x3, 0, 0), 3840, 2880);
    CheckSize(jpt_render_box(p720, k21x9, 0, 0), 1720, 720);
    CheckSize(jpt_render_box(p1080, k21x9, 0, 0), 2560, 1080);
    CheckSize(jpt_render_box(p1440, k21x9, 0, 0), 3440, 1440);
    CheckSize(jpt_render_box(p4k, k21x9, 0, 0), 5120, 2160);
    CheckSize(jpt_render_box(p1440, k4x3, 0, 0), 2560, 1920);
    CheckSize(jpt_render_box(p720, k32x9, 0, 0), 2560, 720);
    CheckSize(jpt_render_box(p4k, k32x9, 0, 0), 7680, 2160);
    CheckSize(jpt_render_box(p1080, k32x9, 0, 0), 3840, 1080);
    CheckSize(jpt_render_box(p1440, k32x9, 0, 0), 5120, 1440);
    CheckSize(jpt_render_box(JPT_RENDER_ORIGINAL, k16x9, 1920, 1080), 0, 0);
    CheckSize(jpt_render_box(JPT_RENDER_NATIVE, k16x9, 2560, 1440), 2560, 1440);
    CheckSize(jpt_render_box(JPT_RENDER_NATIVE, k4x3, 1920, 1080), 1440, 1080);
    CheckSize(jpt_render_box(JPT_RENDER_NATIVE, k21x9, 1920, 1080), 1920, 810);
    CheckSize(jpt_render_box(JPT_RENDER_NATIVE, k32x9, 5120, 1440), 5120, 1440);
    CheckSize(jpt_render_box(JPT_RENDER_NATIVE, k16x9, 0, 0), 0, 0);
    CHECK(jpt_render_preset_index("4K") == p4k && jpt_render_preset_index("8k") == -1);
    CHECK(jpt_aspect_index("5:4") == k4x3);
}

static void TestRenderTargets() {
    const jpt_size_t box4k{3840, 2160};
    CheckSize(jpt_render_target(box4k, 640, 480), 2880, 2160);   // menus keep 4:3 inside a 16:9 box
    CheckSize(jpt_render_target(box4k, 1024, 576), 3840, 2160);  // 16:9 gameplay fills it
    CheckSize(jpt_render_target({2560, 1440}, 1024, 576), 2560, 1440);
    CheckSize(jpt_render_target({1920, 1080}, 1024, 576), 1920, 1080);
    CheckSize(jpt_render_target({5120, 2160}, 1280, 540), 5120, 2160);
    CheckSize(jpt_render_target({3840, 1080}, 2048, 576), 3840, 1080);
    CheckSize(jpt_render_target({1920, 1440}, 800, 600), 1920, 1440);
    CheckSize(jpt_render_target({0, 0}, 800, 600), 0, 0);
    // The real 3D targets of the launcher's choices (D3D11 gameplay at the render box itself).
    const auto gameplay = [](const char* preset, const char* aspect) {
        const jpt_size_t box = jpt_render_box(jpt_render_preset_index(preset), jpt_aspect_index(aspect), 0, 0);
        const jpt_size_t screen = jpt_gameplay_mode(box, jpt_aspect_index(aspect), 1);
        return jpt_render_target(box, screen.width, screen.height);
    };
    CheckSize(gameplay("1440p", "16:9"), 2560, 1440);
    CheckSize(gameplay("1440p", "4:3"), 2560, 1920);
    CheckSize(gameplay("1440p", "21:9"), 3440, 1440);  // within 2% of the 1280x540 mode: the box itself
    CheckSize(gameplay("1440p", "32:9"), 5120, 1440);
    CheckSize(gameplay("1440p", "16:10"), 2560, 1600);
    CheckSize(gameplay("4k", "16:9"), 3840, 2160);
    CheckSize(gameplay("4k", "21:9"), 5120, 2160);
    CheckSize(gameplay("1080p", "21:9"), 2560, 1080);
    CheckSize(jpt_render_target({2560, 1440}, 640, 480), 1920, 1440);  // menus in a 16:9 box
    CheckSize(jpt_render_target({3440, 1440}, 640, 480), 1920, 1440);
    for (const jpt_aspect_t& a : jpt_aspects) {
        CHECK(std::abs(double(a.compat_w) / a.compat_h - double(a.num) / a.den) < 0.005);
        CHECK(a.compat_w % 4 == 0);
    }
}

static void TestHorPlus() {
    CHECK(std::abs(jpt_hor_plus_scale(800, 600) - 1.0) < 1e-9);
    CHECK(std::abs(jpt_hor_plus_scale(1024, 576) - 4.0 / 3.0) < 1e-9);
    CHECK(std::abs(jpt_hor_plus_scale(960, 600) - 1.2) < 1e-9);
    CHECK(std::abs(jpt_hor_plus_scale(1280, 540) - 16.0 / 9.0) < 1e-9);
    CHECK(std::abs(jpt_hor_plus_scale(2048, 576) - 8.0 / 3.0) < 1e-9);
    // The game's default 75-degree horizontal view at 4:3: the widened camera keeps its vertical extent.
    const double pi = 3.14159265358979323846, tan_h = std::tan(75.0 * pi / 360.0), tan_v = tan_h / (4.0 / 3.0);
    for (const jpt_aspect_t& a : jpt_aspects) {
        const double aspect = double(a.compat_w) / a.compat_h, k = jpt_hor_plus_scale(a.compat_w, a.compat_h);
        CHECK(std::abs(tan_h * k / aspect - tan_v) < 1e-9);
        CHECK(tan_h * k >= tan_h - 1e-12);
    }
    // More world to the sides at each wider shape, the same vertical view.
    double previous = 0.0;
    for (const char* key : {"4:3", "16:9", "21:9", "32:9"}) {
        const jpt_aspect_t& a = jpt_aspects[jpt_aspect_index(key)];
        const double k = jpt_hor_plus_scale(a.compat_w, a.compat_h);
        const double horizontal = 2.0 * std::atan(tan_h * k) * 180.0 / pi;
        const double vertical = 2.0 * std::atan(tan_h * k / (double(a.compat_w) / a.compat_h)) * 180.0 / pi;
        CHECK(horizontal > previous);
        CHECK(std::abs(vertical - 2.0 * std::atan(tan_v) * 180.0 / pi) < 1e-9);
        previous = horizontal;
    }
    CHECK(std::abs(2.0 * std::atan(tan_h * jpt_hor_plus_scale(800, 600)) * 180.0 / pi - 75.0) < 1e-9);
    const double wide = 2.0 * std::atan(tan_h * jpt_hor_plus_scale(1024, 576)) * 180.0 / pi;
    CHECK(wide > 90.0 && wide < 92.0);  // 16:9 widens 75 degrees to about 91.1
}

static void TestPresentation() {
    jpt_rect_t r = jpt_present_rect(1920, 1080, 640, 480, 0);
    CHECK(r.left == 240 && r.top == 0 && r.right == 1680 && r.bottom == 1080);
    r = jpt_present_rect(1920, 1080, 640, 480, 1);
    CHECK(r.left == 0 && r.top == 0 && r.right == 1920 && r.bottom == 1080);
    r = jpt_present_rect(1920, 1080, 1280, 540, 0);
    CHECK(r.left == 0 && r.top == 135 && r.right == 1920 && r.bottom == 945);
    r = jpt_present_rect(1920, 1080, 1024, 576, 0);
    CHECK(r.left == 0 && r.top == 0 && r.right == 1920 && r.bottom == 1080);
    r = jpt_present_rect(1921, 1080, 1024, 576, 0);  // within a pixel: no one-pixel bar
    CHECK(r.right - r.left == 1921);
    r = jpt_present_rect(1280, 1024, 800, 600, 0);
    CHECK(r.left == 0 && r.top == 32 && r.bottom == 992);
}

static void TestWindowAndMonitor() {
    jpt_size_t fit = jpt_fit_window({3840, 2160}, 16, 39, 1920, 1040);
    CHECK(fit.width <= 1904 && fit.height <= 1001 && std::abs(fit.width * 9 - fit.height * 16) <= 16);
    CheckSize(jpt_fit_window({1280, 720}, 16, 39, 1920, 1040), 1280, 720);
    CheckSize(jpt_fit_window({5120, 1440}, 0, 0, 2560, 1440), 2560, 720);
    int w = 0, h = 0;
    CHECK(jpt_parse_size("3440x1440", &w, &h) && w == 3440 && h == 1440);
    CHECK(!jpt_parse_size("100x100", &w, &h) && !jpt_parse_size("1920x1080x", &w, &h));
    CHECK(jpt_monitor_index(1, 2) == 0 && jpt_monitor_index(2, 3) == 1);
    CHECK(jpt_monitor_index(5, 2) == 0 && jpt_monitor_index(0, 2) == 0 && jpt_monitor_index(1, 0) == 0);
    bool sizes_ok = std::string(jpt_window_sizes[0].key) == "auto";
    for (int i = 1; i < JPT_WINDOW_SIZE_COUNT; ++i) {
        jpt_config_t c;
        jpt_config_defaults(&c);
        sizes_ok = sizes_ok && jpt_config_apply(&c, JPT_KEY_WINDOW_SIZE, jpt_window_sizes[i].key) &&
                   c.window_w == jpt_window_sizes[i].width;
    }
    CHECK(sizes_ok);
}

static void TestMsaa() {
    CHECK(jpt_msaa_pick(8, (1u << 2) | (1u << 4)) == 4);
    CHECK(jpt_msaa_pick(4, (1u << 2) | (1u << 4) | (1u << 8)) == 4);
    CHECK(jpt_msaa_pick(4, 0) == 1);
    CHECK(jpt_msaa_pick(1, 0xFFFF) == 1);
    jpt_config_t c;
    jpt_config_defaults(&c);
    CHECK(c.msaa == 4);
    CHECK(!jpt_config_apply(&c, JPT_KEY_MSAA, "3") && c.msaa == 4);
    CHECK(jpt_config_apply(&c, JPT_KEY_MSAA, "0") && c.msaa == 1);
}

static void TestSettings(const fs::path& dir) {
    jpt_config_t c;
    jpt_config_defaults(&c);
    CHECK(c.show_launcher && !c.borderless && c.monitor == 1 && c.window_w == 0 && !c.stretch && c.hardware);
    CHECK(c.render_preset == JPT_RENDER_NATIVE && c.aspect == JPT_ASPECT_16_9 && c.texture_filter == JPT_FILTER_ANISO16);
    CHECK(c.msaa == 4 && c.hardware && !c.stretch && !c.borderless);
    CHECK(c.vsync && c.show_fps && !c.skip_intro);
    CHECK(std::string(jpt_default_value(JPT_KEY_SKIP_INTRO)) == "0");
    CHECK(jpt_config_apply(&c, JPT_KEY_SKIP_INTRO, "on") && c.skip_intro);
    CHECK(jpt_config_apply(&c, JPT_KEY_SKIP_INTRO, "0") && !c.skip_intro);
    CHECK(!jpt_config_apply(&c, JPT_KEY_SKIP_INTRO, "maybe") && !c.skip_intro);
    const fs::path ini = dir / "trespasser.ini";
    WriteFile(ini, "# my notes\nrenderer = software\n# vsync = 1\naspect_ratio = 99:1\nmsaa = 8 # high\nunknown = x\n");
    CHECK(jpt_config_load(ini.string().c_str(), &c));
    CHECK(!c.hardware && c.aspect == JPT_ASPECT_16_9 && c.msaa == 8);
    const char* keys[] = {JPT_KEY_VSYNC, JPT_KEY_RENDERER, JPT_KEY_DISPLAY_MODE};
    const char* values[] = {"0", "d3d11", "borderless"};
    CHECK(jpt_ini_set(ini.string().c_str(), keys, values, 3));
    const std::string text = ReadFile(ini);
    CHECK(text.find("# my notes") != std::string::npos && text.find("unknown = x") != std::string::npos);
    CHECK(text.find("vsync = 0") != std::string::npos && text.find("# vsync") == std::string::npos);
    CHECK(text.find("renderer = d3d11") != std::string::npos && text.find("software") == std::string::npos);
    CHECK(text.find("display_mode = borderless") != std::string::npos);
    CHECK(!fs::exists(dir / "trespasser.ini.tmp"));
    CHECK(text.find("settings_version = 3") != std::string::npos);
    jpt_config_load(ini.string().c_str(), &c);
    CHECK(!c.vsync && c.hardware && c.borderless);
    // The command line wins over the file.
    CHECK(jpt_config_apply(&c, JPT_KEY_RENDER_RESOLUTION, "3840x2160") && c.render_preset == -1 && c.render_w == 3840);
    CHECK(jpt_config_apply(&c, JPT_KEY_ASPECT_RATIO, "21:9") && c.aspect == jpt_aspect_index("21:9"));
    CHECK(!jpt_config_apply(&c, JPT_KEY_TEXTURE_FILTER, "trilinear"));

    LauncherSettings s(ini);
    CHECK(s.Get(JPT_KEY_ASPECT_RATIO) == "16:9");  // the saved 99:1 is invalid: the default
    CHECK(s.Get(JPT_KEY_MSAA) == "8");
    s.Set(JPT_KEY_ASPECT_RATIO, "21:9");
    s.SetBool(JPT_KEY_SHOW_FPS, false);
    CHECK(s.Dirty() && ReadFile(ini).find("21:9") == std::string::npos);
    CHECK(s.Save() && !s.Dirty());
    LauncherSettings again(ini);
    CHECK(again.Get(JPT_KEY_ASPECT_RATIO) == "21:9" && !again.GetBool(JPT_KEY_SHOW_FPS, true));
    CHECK(!again.GetBool(JPT_KEY_SKIP_INTRO, true));
    again.SetBool(JPT_KEY_SKIP_INTRO, true);
    CHECK(again.Save() && ReadFile(ini).find("skip_intro = 1") != std::string::npos);
    jpt_config_load(ini.string().c_str(), &c);
    CHECK(c.skip_intro);
    again.SetBool(JPT_KEY_SKIP_INTRO, false);
    CHECK(again.Save() && ReadFile(ini).find("skip_intro = 0") != std::string::npos);
    jpt_config_load(ini.string().c_str(), &c);
    CHECK(!c.skip_intro);
    again.ResetToDefaults();
    CHECK(again.Get(JPT_KEY_ASPECT_RATIO) == "16:9" && again.Dirty());
    CHECK(ReadFile(ini).find("# my notes") != std::string::npos);

    const fs::path reg = dir / "recomp-registry.txt";
    WriteFile(reg, "Installed=dword:00000001\nGore=dword:00000001\nKeyMap=hex(3):0100\n");
    GameRegistry r(reg);
    unsigned long v = 7;
    CHECK(r.GetDword("Gore", v) && v == 1 && !r.GetDword("InvertMouse", v) && !r.GetDword("KeyMap", v));
    CHECK(r.SetDword("Gore", 0) && r.SetDword("InvertMouse", 1));
    CHECK(r.GetDword("Gore", v) && v == 0 && r.GetDword("InvertMouse", v) && v == 1);
    CHECK(ReadFile(reg).find("KeyMap=hex(3):0100") != std::string::npos);
}


static void TestMigration(const fs::path& dir) {
    const fs::path ini = dir / "experimental.ini";
    // What the first launcher wrote in test 018: no settings_version.
    WriteFile(ini, "# Set by Trespasser Recompiled\nmsaa = 0\nrender_resolution = 1440p\ntexture_filter = original\n"
                   "vsync = 0\nshow_launcher = 0\nlauncher_x = 96\n");
    CHECK(jpt_config_migrate(ini.string().c_str()) == JPT_MIGRATED_EXPERIMENTAL);
    jpt_config_t c;
    jpt_config_load(ini.string().c_str(), &c);
    CHECK(c.msaa == 4 && c.render_preset == JPT_RENDER_NATIVE && c.texture_filter == JPT_FILTER_ANISO16 && c.vsync);
    CHECK(c.aspect == JPT_ASPECT_16_9 && !c.show_launcher);
    const std::string text = ReadFile(ini);
    CHECK(text.find("launcher_x = 96") != std::string::npos && text.find("settings_version = 3") != std::string::npos);
    CHECK(text.find("msaa") == std::string::npos && text.find("1440p") == std::string::npos);
    CHECK(jpt_config_migrate(ini.string().c_str()) == 0);  // once
    // A file written by this version keeps the player's choices.
    const char* keys[] = {JPT_KEY_ASPECT_RATIO, JPT_KEY_MSAA};
    const char* values[] = {"4:3", "0"};
    CHECK(jpt_ini_set(ini.string().c_str(), keys, values, 2));
    CHECK(jpt_config_migrate(ini.string().c_str()) == 0);
    jpt_config_load(ini.string().c_str(), &c);
    CHECK(c.aspect == JPT_ASPECT_4_3 && c.msaa == 1);
    CHECK(jpt_config_migrate((dir / "missing.ini").string().c_str()) == 0);

    // Version 2 with the earlier launcher's graphics defaults (log 023): only those values are reset, once.
    const fs::path v2 = dir / "v2.ini";
    WriteFile(v2, "# Set by Trespasser Recompiled\nlauncher_x = 96\nsettings_version = 2\nmsaa = 0\n"
                  "render_resolution = 1440p\ntexture_filter = original\nvsync = 0\ndisplay_mode = windowed\n");
    CHECK(jpt_config_migrate(v2.string().c_str()) == JPT_MIGRATED_GRAPHICS_DEFAULTS);
    jpt_config_load(v2.string().c_str(), &c);
    CHECK(c.msaa == 4 && c.texture_filter == JPT_FILTER_ANISO16 && c.vsync);
    CHECK(c.render_preset == jpt_render_preset_index("1440p") && !c.borderless);
    CHECK(ReadFile(v2).find("settings_version = 3") != std::string::npos);
    CHECK(ReadFile(v2).find("launcher_x = 96") != std::string::npos);
    CHECK(jpt_config_migrate(v2.string().c_str()) == 0);
    const char* old_keys[] = {JPT_KEY_TEXTURE_FILTER, JPT_KEY_MSAA, JPT_KEY_VSYNC};
    const char* old_values[] = {"original", "0", "0"};
    CHECK(jpt_ini_set(v2.string().c_str(), old_keys, old_values, 3));
    CHECK(jpt_config_migrate(v2.string().c_str()) == 0);  // chosen again after the migration: kept
    jpt_config_load(v2.string().c_str(), &c);
    CHECK(c.texture_filter == JPT_FILTER_ORIGINAL && c.msaa == 1 && !c.vsync);

    // Version 2 with explicit modern or other choices keeps them.
    WriteFile(v2, "settings_version = 2\nmsaa = 2\ntexture_filter = bilinear\nvsync = 1\n");
    CHECK(jpt_config_migrate(v2.string().c_str()) == JPT_MIGRATED_VERSION_ONLY);
    jpt_config_load(v2.string().c_str(), &c);
    CHECK(c.msaa == 2 && c.texture_filter == JPT_FILTER_BILINEAR && c.vsync);
    CHECK(ReadFile(v2).find("settings_version = 3") != std::string::npos);
}

static void TestPages() {
    const char* expected[] = {"Play", "Display", "Graphics", "Gameplay", "Controls", "Cheats", "Bug Report", "About"};
    CHECK(std::size(kPageOrder) == std::size(expected));
    for (size_t i = 0; i < std::size(kPageOrder) && i < std::size(expected); ++i) {
        const std::string name = PageName(kPageOrder[i]);
        CHECK(name == expected[i]);
        CHECK(name.find("Online") == std::string::npos && name.find("Multiplayer") == std::string::npos);
        CHECK(FindPageIcon(kPageOrder[i]) != nullptr);
        const std::vector<uint8_t> icon = RenderPageIcon(kPageOrder[i], 24);
        CHECK(icon.size() == 24 * 24 * 4);
        bool drawn = false;
        for (size_t p = 3; p < icon.size(); p += 4) drawn = drawn || icon[p] != 0;
        CHECK(drawn);
    }
    CHECK(std::string(kSponsorUrl) == "https://github.com/sponsors/saulob");
    CHECK(std::string(kGameTitle) == "Jurassic Park: Trespasser Recompiled");
    CHECK(ResolutionLabel(jpt_render_preset_index("1440p"), {2560, 1440}) == "1440p \xE2\x80\x94 2560 \xC3\x97 1440");
}

static uint32_t Be32(const std::string& bytes, size_t at) {
    return (uint32_t(uint8_t(bytes[at])) << 24) | (uint32_t(uint8_t(bytes[at + 1])) << 16) |
           (uint32_t(uint8_t(bytes[at + 2])) << 8) | uint8_t(bytes[at + 3]);
}

static void TestAssets() {
    const fs::path assets = fs::path(JPT_SOURCE_DIR) / "assets" / "launcher";
    const std::string art = ReadFile(assets / "header-art.png");
    CHECK(art.size() > 24 && art.compare(1, 3, "PNG") == 0 && Be32(art, 16) == 3840 && Be32(art, 20) == 432);
    const std::string heart = ReadFile(assets / "sponsor-heart.png");
    CHECK(heart.size() > 24 && Be32(heart, 16) == 64 && Be32(heart, 20) == 64);
    CHECK(fs::exists(assets / "icons" / "SOURCES.md") && fs::exists(assets / "sponsor-heart.svg"));
}

// 16:9 is the default Aspect ratio; a choice the player saved is never replaced.
static void TestAspectRatioChoice(const fs::path& dir) {
    jpt_config_t c;
    CHECK(JPT_ASPECT_COUNT == 5 && std::string(jpt_aspects[JPT_ASPECT_4_3].label) == "4:3 (original)");
    CHECK(std::string(jpt_default_value(JPT_KEY_ASPECT_RATIO)) == "16:9");
    CHECK(!jpt_config_load((dir / "no-settings-yet.ini").string().c_str(), &c) && c.aspect == JPT_ASPECT_16_9);

    const fs::path ini = dir / "aspect.ini";
    WriteFile(ini, "# Set by Trespasser Recompiled\nsettings_version = 3\nmsaa = 8\n");
    CHECK(jpt_config_load(ini.string().c_str(), &c) && c.aspect == JPT_ASPECT_16_9);

    for (const char* key : {"4:3", "16:9", "16:10", "21:9", "32:9"}) {
        const char* keys[] = {JPT_KEY_ASPECT_RATIO};
        const char* values[] = {key};
        CHECK(jpt_ini_set(ini.string().c_str(), keys, values, 1));
        CHECK(jpt_config_migrate(ini.string().c_str()) == 0);
        CHECK(jpt_config_load(ini.string().c_str(), &c) && std::string(jpt_aspects[c.aspect].key) == key);
        LauncherSettings saved(ini);
        CHECK(saved.Get(JPT_KEY_ASPECT_RATIO) == key);
    }

    WriteFile(ini, "settings_version = 3\naspect_ratio = widescreen\n");
    CHECK(jpt_config_load(ini.string().c_str(), &c) && c.aspect == JPT_ASPECT_16_9);
    LauncherSettings invalid(ini);
    CHECK(invalid.Get(JPT_KEY_ASPECT_RATIO) == "16:9");

    WriteFile(ini, "settings_version = 3\naspect_ratio = 4:3\n");
    LauncherSettings reset(ini);
    CHECK(reset.Get(JPT_KEY_ASPECT_RATIO) == "4:3");
    reset.ResetToDefaults();
    CHECK(reset.Get(JPT_KEY_ASPECT_RATIO) == "16:9");
}

static void TestPlayGate() {
    CHECK(LauncherWanted(false, false, false, true));  // files not ready: always
    CHECK(LauncherWanted(true, true, false, false));
    CHECK(!LauncherWanted(true, false, false, false));
    CHECK(!LauncherWanted(true, true, false, true));  // --skip-launcher
    CHECK(LauncherWanted(true, false, true, false));  // Shift or --launcher
    CHECK(!LauncherWanted(true, true, true, true));   // a skip wins, as in the other ports
    PlayGate gate;
    int saves = 0;
    CHECK(gate.Request(false, false, [&] { return ++saves, true; }) == PlayRequest::kFilesNotReady && saves == 0);
    CHECK(gate.Request(true, true, [&] { return ++saves, true; }) == PlayRequest::kBusy && saves == 0);
    CHECK(gate.Request(false, true, [&] { return ++saves, false; }) == PlayRequest::kSaveFailed && !gate.starting());
    CHECK(gate.Request(false, true, [&] { return ++saves, true; }) == PlayRequest::kStarting && gate.starting());
    CHECK(gate.Request(false, true, [&] { return ++saves, true; }) == PlayRequest::kBusy && saves == 2);
    gate.Cancel();
    CHECK(!gate.starting());
}

// "abc" hashes to the FIPS 180-2 test vector.
constexpr char kAbcSha256[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static std::string Sha(const std::string& text) {
    char hex[65];
    jpt_sha256_buffer_hex(text.data(), text.size(), hex);
    return hex;
}

// Synthetic versions the port never runs.
const jpt_game_version_t kTestVersion = {"test", "Test Build", "English", "en", "test release", 116, "lift116",
                                         nullptr, 1252, nullptr, nullptr, nullptr, nullptr};
const jpt_game_version_t kOtherVersion = {"other", "Other Build", "Spanish", "es", "other release", 117,
                                          "lift_other", nullptr, 1252, nullptr, nullptr, "versions\\other\\saves",
                                          "needs another engine"};

struct TestManifest {
    std::vector<std::string> digests;
    std::vector<InstallFile> files;
};

// The installed tree of TestDisc: the guest image, four more runtime files and data\, never setup\directx.
static const std::vector<InstallFile>& TestFiles() {
    static const TestManifest m = [] {
        const std::vector<std::tuple<const char*, const char*, std::string>> rows = {
            {"setup/tpassp6.exe", JPT_GUEST_IMAGE, "abc"}, {"setup/msvcirt.dll", "MSVCIRT.DLL", "m"},
            {"setup/smackw32.dll", "SMACKW32.DLL", "s"},   {"setup/processor.dll", "Processor.dll", "p"},
            {"readme.txt", "Readme.txt", "r"},             {"data/be.scn", "data/be.scn", "levelbe"},
            {"data/menu/tpassintro.smk", "data/menu/tpassintro.smk", "intro"}};
        TestManifest out;
        out.digests.reserve(rows.size());
        for (const auto& [src, dst, content] : rows) out.digests.push_back(Sha(content));
        for (size_t i = 0; i < rows.size(); ++i)
            out.files.push_back({SourceKind::kDisc, std::get<0>(rows[i]), std::get<1>(rows[i]), std::get<2>(rows[i]).size(),
                                 out.digests[i].c_str(), i == 0 ? FileRole::kGuestImage : FileRole::kData});
        return out;
    }();
    return m.files;
}

constexpr DiscIdentity kTestDiscs[] = {{"Test pressing", "TEST_DISC"}, {"Second pressing", "TEST_DISC_2"}};
static GameProfile TestProfile() {
    static const std::string identity = Sha("levelbe");
    return {&kTestVersion, "data/be.scn", identity.c_str(), kTestDiscs, nullptr, TestFiles()};
}

// `with_exe` false: the same disc without the original executable (the guest image's source).
static std::vector<IsoInput> TestDisc(bool joliet, bool with_exe = true) {
    auto n = [joliet](const char* lower, const char* upper) { return std::string(joliet ? lower : upper); };
    std::vector<IsoInput> files = {
        {n("setup/msvcirt.dll", "SETUP/MSVCIRT.DLL"), "m"},   {n("setup/smackw32.dll", "SETUP/SMACKW32.DLL"), "s"},
        {n("setup/processor.dll", "SETUP/PROCESSOR.DLL"), "p"}, {n("readme.txt", "README.TXT"), "r"},
        {n("data/be.scn", "DATA/BE.SCN"), "levelbe"},          {n("data/menu/tpassintro.smk", "DATA/MENU/TPASSINTRO.SMK"), "intro"},
        {n("setup/directx/dsetup.dll", "SETUP/DIRECTX/DSETUP.DLL"), "not installed"}};
    if (with_exe) files.insert(files.begin(), {n("setup/tpassp6.exe", "SETUP/TPASSP6.EXE"), "abc"});
    return files;
}

// ---- InstallShield 5 cabinet and DEFLATE

// 443 bytes of "Trespasser " * 40 + "end", fixed Huffman codes.
constexpr unsigned char kFixed[] = {11, 41, 74, 45, 46, 72, 44, 46, 78, 45, 82, 8, 25, 101, 14, 29, 102, 106, 94, 10, 0};
// 3948 bytes of DynamicText(), dynamic Huffman codes.
constexpr unsigned char kDynamic[] = {133,215,193,109,99,49,12,69,209,86,82,130,40,81,18,89,142,157,164,135,36,213,103,86,3,12,48,240,241,246,107,117,225,3,60,62,30,127,126,207,231,243,253,99,124,125,255,188,125,254,251,123,123,252,253,188,238,235,239,55,95,127,143,8,60,200,194,131,218,175,31,204,57,241,96,55,30,244,65,133,181,148,9,29,115,32,100,38,74,102,161,228,14,148,220,27,37,119,163,228,153,40,121,14,74,222,177,244,135,66,201,123,81,178,2,37,107,163,100,21,74,246,68,201,62,40,217,141,146,75,33,151,224,13,193,187,130,151,130,23,130,87,130,183,5,111,42,83,11,222,17,188,37,120,67,240,174,224,165,224,133,224,149,224,109,193,155,250,67,181,224,29,193,91,130,55,4,239,10,94,10,94,8,30,74,46,133,68,199,24,45,120,71,240,150,224,13,193,187,130,151,130,23,130,87,130,183,5,15,37,243,182,224,29,193,91,130,55,4,239,10,94,10,94,8,94,9,222,22,60,148,172,108,193,59,130,183,4,111,8,30,74,46,133,212,146,28,90,146,185,5,79,75,50,180,36,247,17,60,45,201,165,69,126,180,36,135,150,100,134,224,105,73,134,150,228,70,201,93,90,146,243,8,158,150,228,208,146,228,113,163,235,166,116,221,148,174,155,210,117,211,186,110,90,215,77,251,186,81,72,193,211,146,212,109,19,186,109,166,110,155,169,219,102,234,182,89,147,139,92,75,242,255,183,205,47};

static std::string DynamicText() {
    std::string out;
    for (int i = 0; i < 120; ++i) out += "aaaaabbbcd" + std::to_string(i * 37 % 1000) + "xyz eeeeeeeeeeeeeee ";
    return out;
}

static std::string Inflated(const unsigned char* data, size_t n, size_t max_out, bool& ok) {
    std::vector<uint8_t> out;
    ok = InflateRaw(data, n, out, max_out);
    return std::string(out.begin(), out.end());
}

static void TestInflate() {
    bool ok = false;
    std::string fixed;
    for (int i = 0; i < 40; ++i) fixed += "Trespasser ";
    fixed += "end";
    CHECK(Inflated(kFixed, sizeof kFixed, 1 << 20, ok) == fixed && ok);
    CHECK(Inflated(kDynamic, sizeof kDynamic, 1 << 20, ok) == DynamicText() && ok);
    const unsigned char stored[] = {1, 3, 0, 0xFC, 0xFF, 'a', 'b', 'c'};
    CHECK(Inflated(stored, sizeof stored, 16, ok) == "abc" && ok);
    Inflated(kDynamic, sizeof kDynamic, 100, ok);
    CHECK(!ok);  // would decode past its recorded size
    const unsigned char bad_lengths[] = {1, 3, 0, 0x00, 0x00, 'a', 'b', 'c'};
    Inflated(bad_lengths, sizeof bad_lengths, 16, ok);
    CHECK(!ok);
    const unsigned char reserved_type[] = {7};
    Inflated(reserved_type, sizeof reserved_type, 16, ok);
    CHECK(!ok);
    Inflated(kDynamic, 40, 1 << 20, ok);
    CHECK(!ok);  // cut in the middle of a block
}

struct CabInput {
    std::string folder, name;
    std::string packed;  // as stored
    size_t size;         // expanded
    bool compressed;
};

// The layout of setup/data1.cab: common header, descriptor at 0x200, file table, then the data.
static std::vector<uint8_t> BuildCabinet(const std::vector<CabInput>& files) {
    std::vector<std::string> folders;
    for (const CabInput& f : files)
        if (std::find(folders.begin(), folders.end(), f.folder) == folders.end()) folders.push_back(f.folder);
    std::vector<uint8_t> b(0x200 + 0x30, 0);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&b[at], &v, 2); };
    std::memcpy(b.data(), "ISc(", 4);
    put32(4, 0x01000004);
    put32(12, 0x200);
    const size_t base = b.size();
    put32(0x200 + 0x0C, uint32_t(base - 0x200));
    put32(0x200 + 0x1C, uint32_t(folders.size()));
    put32(0x200 + 0x28, uint32_t(files.size()));
    const size_t table_n = folders.size() + files.size();
    b.resize(base + 4 * table_n, 0);
    std::vector<size_t> name_at;
    auto add_name = [&](const std::string& s) {
        const size_t at = b.size() - base;
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
        return at;
    };
    for (size_t i = 0; i < folders.size(); ++i) put32(base + 4 * i, uint32_t(add_name(folders[i])));
    std::vector<size_t> desc;
    for (const CabInput& f : files) {
        const size_t name = add_name(f.name);
        desc.push_back(b.size() - base);
        b.resize(b.size() + 0x2A, 0);
        put32(base + desc.back(), uint32_t(name));
    }
    for (size_t i = 0; i < files.size(); ++i) {
        const CabInput& f = files[i];
        const size_t d = base + desc[i];
        put32(base + 4 * (folders.size() + i), uint32_t(desc[i]));
        put32(d + 4, uint32_t(std::find(folders.begin(), folders.end(), f.folder) - folders.begin()));
        put16(d + 8, f.compressed ? 4 : 0);
        put32(d + 10, uint32_t(f.size));
        put32(d + 14, uint32_t(f.packed.size()));
        put32(d + 0x26, uint32_t(b.size()));
        b.insert(b.end(), f.packed.begin(), f.packed.end());
    }
    return b;
}

static void TestCabinet() {
    std::string fixed;
    for (int i = 0; i < 40; ++i) fixed += "Trespasser ";
    fixed += "end";
    const std::vector<CabInput> inputs = {
        {"", "tpassp6.exe", "abc", 3, false},
        {"menu", "Main_bg.tga", std::string(reinterpret_cast<const char*>(kFixed), sizeof kFixed), fixed.size(), true},
        {"menu", "audio.ddf", std::string(reinterpret_cast<const char*>(kDynamic), sizeof kDynamic), DynamicText().size(), true},
    };
    Cabinet cab;
    std::string error;
    CHECK(cab.Open(BuildCabinet(inputs), error) && cab.files().size() == 3);
    std::vector<uint8_t> out;
    const CabinetFile* exe = cab.Find("TPASSP6.EXE");
    CHECK(exe && cab.Read(*exe, out, error) && std::string(out.begin(), out.end()) == "abc");
    const CabinetFile* tga = cab.Find("menu/main_bg.tga");
    CHECK(tga && cab.Read(*tga, out, error) && std::string(out.begin(), out.end()) == fixed);
    const CabinetFile* ddf = cab.Find("menu/audio.ddf");
    CHECK(ddf && cab.Read(*ddf, out, error) && std::string(out.begin(), out.end()) == DynamicText());
    CHECK(!cab.Find("menu/missing.tga"));

    std::vector<CabInput> wrong_size = inputs;
    wrong_size[1].size += 1;
    Cabinet cut;
    CHECK(cut.Open(BuildCabinet(wrong_size), error));
    tga = cut.Find("menu/Main_bg.tga");
    CHECK(tga && !cut.Read(*tga, out, error) && error.find("bytes, expected") != std::string::npos);

    std::vector<uint8_t> bytes = BuildCabinet(inputs);
    bytes[4] = 6;  // another InstallShield version
    Cabinet other;
    CHECK(!other.Open(bytes, error));
    bytes = BuildCabinet(inputs);
    bytes.resize(0x230);  // the table is gone
    CHECK(!other.Open(bytes, error));
    std::vector<CabInput> escape = inputs;
    escape[0].folder = "..";
    CHECK(!other.Open(BuildCabinet(escape), error));
}

// ---- disc identity, import, versions

static void TestIsoReader(const fs::path& dir) {
    for (bool joliet : {false, true}) {
        const fs::path path = dir / (joliet ? "joliet.iso" : "plain.iso");
        WriteImage(path, IsoBuilder().Build("TEST_DISC", TestDisc(joliet), joliet));
        IsoImage iso;
        std::string error;
        CHECK(iso.Open(path, error));
        CHECK(iso.joliet() == joliet && iso.volume_id() == "TEST_DISC" && iso.files().size() == 8);
        const IsoFile* exe = iso.Find("setup/tpassp6.exe");
        CHECK(exe && exe->size == 3);
        CHECK(iso.Find("SETUP/MSVCIRT.DLL") && !iso.Find("setup/missing.dll"));
        const fs::path out = dir / "out.bin";
        uint64_t seen = 0;
        std::string sha;
        CHECK(exe && iso.Extract(*exe, out, [&](uint64_t n) { return seen += n, true; }, error, &sha));
        CHECK(ReadFile(out) == "abc" && seen == 3 && !fs::exists(dir / "out.bin.part") && sha == kAbcSha256);
        CHECK(Sha256File(out) == kAbcSha256);
        std::vector<uint8_t> all;
        CHECK(exe && iso.ReadAll(*exe, all, error) && std::string(all.begin(), all.end()) == "abc");
    }
    std::string error;
    const fs::path junk = dir / "junk.iso";
    WriteFile(junk, std::string(40000, 'x'));
    IsoImage bad;
    CHECK(!bad.Open(junk, error) && !error.empty());
    std::vector<unsigned char> image = IsoBuilder().Build("TEST_DISC", TestDisc(false), false);
    image.resize(23 * 2048);  // descriptors and the five directories present, file data cut off
    const fs::path cut = dir / "cut.iso";
    WriteImage(cut, image);
    IsoImage truncated;
    CHECK(truncated.Open(cut, error));
    const IsoFile* exe = truncated.Find("setup/tpassp6.exe");
    std::vector<uint8_t> all;
    CHECK(exe && !truncated.Extract(*exe, dir / "cut.bin", nullptr, error) && !fs::exists(dir / "cut.bin"));
    const IsoFile* level = truncated.Find("data/menu/tpassintro.smk");
    CHECK(level && !truncated.ReadAll(*level, all, error));
}

static void TestIdentity(const fs::path& dir) {
    const GameProfile profiles[] = {TestProfile()};
    std::string why;
    IsoImage good, second, other, version, damaged, partial;
    WriteImage(dir / "good.iso", IsoBuilder().Build("TEST_DISC", TestDisc(true), true));
    CHECK(good.Open(dir / "good.iso", why));
    DiscMatch m = IdentifyDisc(good, profiles, why);
    CHECK(m.profile == &profiles[0] && m.disc == &kTestDiscs[0]);
    std::string plan_error;
    CHECK(PlanInstall(good, nullptr, profiles[0], plan_error).size() == 7);  // five runtime files and data/

    // The same disc without the original executable: recognised and planned, only the guest image has no source.
    WriteImage(dir / "no-exe.iso", IsoBuilder().Build("TEST_DISC", TestDisc(true, false), true));
    IsoImage no_exe;
    CHECK(no_exe.Open(dir / "no-exe.iso", why));
    m = IdentifyDisc(no_exe, profiles, why);
    CHECK(m.profile == &profiles[0] && m.disc == &kTestDiscs[0]);
    const std::vector<PlannedFile> bare = PlanInstall(no_exe, nullptr, profiles[0], plan_error);
    size_t absent = 0;
    for (const PlannedFile& f : bare) absent += !f.present() && f.file->role == FileRole::kGuestImage;
    CHECK(bare.size() == 7 && absent == 1 && !bare[0].present() && bare[1].present());

    // Another pressing of the version: told apart by the volume label, the label alone never identifies.
    WriteImage(dir / "second.iso", IsoBuilder().Build("TEST_DISC_2", TestDisc(true), true));
    CHECK(second.Open(dir / "second.iso", why));
    m = IdentifyDisc(second, profiles, why);
    CHECK(m.profile == &profiles[0] && m.disc == &kTestDiscs[1]);
    WriteImage(dir / "renamed.iso", IsoBuilder().Build("RENAMED", TestDisc(true), true));
    IsoImage renamed;
    CHECK(renamed.Open(dir / "renamed.iso", why) && IdentifyDisc(renamed, profiles, why).profile == &profiles[0]);

    WriteImage(dir / "other.iso", IsoBuilder().Build("SOME_GAME", {{"game.exe", "x"}}, true));
    CHECK(other.Open(dir / "other.iso", why));
    m = IdentifyDisc(other, profiles, why);
    CHECK(!m.profile && m.status == InstallStatus::kWrongDisc && why.find("not a Trespasser") != std::string::npos);

    // A Trespasser disc no profile knows: refused, with the data file fingerprints needed to add it.
    std::vector<IsoInput> files = TestDisc(true);
    files[5].content = "abcd";
    WriteImage(dir / "version.iso", IsoBuilder().Build("TEST_DISC", files, true));
    CHECK(version.Open(dir / "version.iso", why));
    m = IdentifyDisc(version, profiles, why);
    CHECK(!m.profile && m.status == InstallStatus::kUnsupportedBuild && why.find("Unsupported Trespasser build") == 0);
    bool fingerprint = false;
    for (const std::string& line : m.diagnostics) fingerprint = fingerprint || line.find(Sha("abcd")) != std::string::npos;
    CHECK(fingerprint);

    for (const std::string& line : m.diagnostics) CHECK(line.find("tpassp6") == std::string::npos && line.find(".exe") == std::string::npos);

    // The identity file, but another file of the version missing from the disc: not accepted as that version.
    files = TestDisc(true);
    files.erase(files.begin() + 6);
    WriteImage(dir / "partial.iso", IsoBuilder().Build("TEST_DISC", files, true));
    CHECK(partial.Open(dir / "partial.iso", why));
    m = IdentifyDisc(partial, profiles, why);
    CHECK(!m.profile && m.status == InstallStatus::kDamagedDisc && why.find("tpassintro.smk") != std::string::npos);

    // An executable alone identifies nothing, whatever its bytes.
    WriteImage(dir / "exe-only.iso", IsoBuilder().Build("TEST_DISC", {{"setup/tpassp6.exe", "abc"}}, true));
    IsoImage exe_only;
    CHECK(exe_only.Open(dir / "exe-only.iso", why));
    m = IdentifyDisc(exe_only, profiles, why);
    CHECK(!m.profile && m.status == InstallStatus::kWrongDisc);
}

static void TestInstaller(const fs::path& dir) {
    const fs::path iso = dir / "install.iso";
    WriteImage(iso, IsoBuilder().Build("TEST_DISC", TestDisc(true), true));
    const GameProfile profiles[] = {TestProfile()};
    const fs::path root = dir / "game", target = root / "versions" / "test";
    fs::remove_all(root);
    InstallHooks hooks;
    int phases = 0;
    hooks.phase = [&](const char*) { ++phases; };

    // Fresh install into versions\<id>.
    InstallResult r = InstallFromIso(iso, root, hooks, profiles);
    CHECK(r.status == InstallStatus::kInstalled && r.files == 7 && r.profile == &profiles[0] && phases >= 4);
    CHECK(r.dir == target && CheckGameFolder(target, profiles[0]).state == GameState::kReady);
    CHECK(!fs::exists(StagingFolder(target)) && !fs::exists(BackupFolder(target)));
    CHECK(ReadFile(target / JPT_GUEST_IMAGE) == "abc" && !fs::exists(target / "setup"));
    CHECK(!fs::exists(target / "trespass.exe") && !fs::exists(target / "tpassp6.exe"));

    // The same disc again: nothing is copied.
    WriteFile(target / "marker.txt", "kept");
    r = InstallFromIso(iso, root, hooks, profiles);
    CHECK(r.status == InstallStatus::kUpToDate && r.ok() && r.dir == target && fs::exists(target / "marker.txt"));

    // A damaged installed copy is replaced only when asked to.
    WriteFile(target / "data" / "be.scn", "x");
    InstallHooks replace = hooks;
    replace.replace = true;
    r = InstallFromIso(iso, root, replace, profiles);
    CHECK(r.status == InstallStatus::kInstalled && ReadFile(target / "data" / "be.scn") == "levelbe");
    CHECK(!fs::exists(target / "marker.txt"));

    // Another disc: refused before anything is copied.
    WriteImage(dir / "wrong.iso", IsoBuilder().Build("SOME_GAME", {{"game.exe", "x"}}, true));
    r = InstallFromIso(dir / "wrong.iso", root, replace, profiles);
    CHECK(r.status == InstallStatus::kWrongDisc && ReadFile(target / JPT_GUEST_IMAGE) == "abc");
    WriteFile(dir / "notiso.iso", "hello");
    r = InstallFromIso(dir / "notiso.iso", root, replace, profiles);
    CHECK(r.status == InstallStatus::kNotAnImage);

    // Cancel during the copy: the installed version stays.
    InstallHooks cancel = replace;
    cancel.progress = [](uint64_t done, uint64_t) { return done < 4; };
    r = InstallFromIso(iso, root, cancel, profiles);
    CHECK(r.status == InstallStatus::kCancelled && ReadFile(target / JPT_GUEST_IMAGE) == "abc");
    CHECK(!fs::exists(StagingFolder(target)));

    // A disc without the guest image's source: refused before anything is staged, the installed version stays.
    WriteImage(dir / "no-exe-install.iso", IsoBuilder().Build("TEST_DISC", TestDisc(true, false), true));
    r = InstallFromIso(dir / "no-exe-install.iso", root, replace, profiles);
    CHECK(r.status == InstallStatus::kNoGuestImageSource && !r.ok() && r.detail.find("the disc lacks setup/tpassp6.exe") == 0);
    CHECK(ReadFile(target / JPT_GUEST_IMAGE) == "abc" && ReadFile(target / "data" / "be.scn") == "levelbe");
    CHECK(!fs::exists(StagingFolder(target)) && !fs::exists(BackupFolder(target)));

    // The version folder cannot be moved aside (a file in it is open): rollback.
    HANDLE held = CreateFileW((target / JPT_GUEST_IMAGE).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    r = InstallFromIso(iso, root, replace, profiles);
    CloseHandle(held);
    CHECK(r.status == InstallStatus::kActivateFailed && ReadFile(target / JPT_GUEST_IMAGE) == "abc");
    CHECK(!fs::exists(StagingFolder(target)) && !fs::exists(BackupFolder(target)));

    // An image inside the version folder is refused (reinstalling would remove it).
    fs::copy_file(iso, target / "inside.iso");
    r = InstallFromIso(target / "inside.iso", root, replace, profiles);
    CHECK(r.status == InstallStatus::kInsideGameFolder);
    fs::remove(target / "inside.iso");

    // The ISO is only read.
    const auto stamp = fs::last_write_time(iso);
    const std::string before = ReadFile(iso);
    r = InstallFromIso(iso, root, replace, profiles);
    CHECK(r.ok() && fs::last_write_time(iso) == stamp && ReadFile(iso) == before);

    // A damaged copy of the disc: refused while no installed version has the pressed file...
    std::vector<IsoInput> files = TestDisc(true);
    files[6].content = "intrp";  // same size, other bytes
    WriteImage(dir / "damaged.iso", IsoBuilder().Build("TEST_DISC", files, true));
    const fs::path empty_root = dir / "empty";
    fs::remove_all(empty_root);
    r = InstallFromIso(dir / "damaged.iso", empty_root, hooks, profiles);
    CHECK(r.status == InstallStatus::kDamagedDisc && r.detail.find("data/menu/tpassintro.smk") != std::string::npos);
    CHECK(!fs::exists(empty_root / "versions" / "test") && !fs::exists(StagingFolder(empty_root / "versions" / "test")));
    // ...and repaired from one that has it.
    r = InstallFromIso(dir / "damaged.iso", root, replace, profiles);
    CHECK(r.status == InstallStatus::kInstalled && r.repaired == 1 && ReadFile(target / "data" / "menu" / "tpassintro.smk") == "intro");

    // Game file states.
    CHECK(CheckGameFolder(dir / "nothing", profiles[0]).state == GameState::kMissing);
    fs::remove(target / "data" / "menu" / "tpassintro.smk");
    const GameCheck missing = CheckGameFolder(target, profiles[0]);
    CHECK(missing.state == GameState::kInvalid && missing.detail.find("tpassintro.smk") != std::string::npos);
    // An executable in a folder is no installation: only the guest image makes a version.
    WriteFile(dir / "loose" / "trespass.exe", "zzz");
    const GameCheck loose = CheckGameFolder(dir / "loose", profiles[0]);
    CHECK(loose.state == GameState::kMissing && !loose.profile && loose.detail.find("no guest image") == 0);
    WriteFile(dir / "loose" / JPT_GUEST_IMAGE, "zz");
    const GameCheck partial = CheckGameFolder(dir / "loose", profiles[0]);
    CHECK(partial.state == GameState::kInvalid && partial.profile == &profiles[0] &&
          partial.detail.find("has 2 bytes, expected 3") != std::string::npos);
}

static void InstallTestTree(const fs::path& dir) {
    for (const InstallFile& f : TestFiles()) WriteFile(dir / f.installed, std::string(static_cast<size_t>(f.size), 'x'));
}

static void TestVersions(const fs::path& dir) {
    const std::vector<InstallFile>& files = TestFiles();
    const GameProfile profiles[] = {TestProfile(), {&kOtherVersion, "data/be.scn", "", kTestDiscs, nullptr, files}};
    const fs::path root = dir / "versions-game";
    fs::remove_all(root);
    CHECK(FindInstalledVersions(root, profiles).empty() && !ChooseVersion({}, "test"));

    // Files directly in the game folder, an executable among them (an installation made before versions existed):
    // not a version, never listed, never touched.
    InstallTestTree(root);
    WriteFile(root / "trespass.exe", "abc");
    WriteFile(root / "SAVEGAME.000", "save");
    std::vector<InstalledVersion> found = FindInstalledVersions(root, profiles);
    CHECK(found.empty() && !ChooseVersion(found, ""));

    // A version folder with the old executable name and no guest image: ignored, not an error, not ready.
    InstallTestTree(root / "versions" / "other");
    fs::remove(root / "versions" / "other" / JPT_GUEST_IMAGE);
    WriteFile(root / "versions" / "other" / "trespass.exe", "ba7816bf");
    CHECK(FindInstalledVersions(root, profiles).empty());
    CHECK(CheckGameFolder(root / "versions" / "other", profiles[1]).state == GameState::kMissing);

    // With its guest image the folder is a version; the old executable beside it is ignored. A file of another size
    // keeps it from being ready, and Play never uses it.
    WriteFile(root / "versions" / "other" / JPT_GUEST_IMAGE, "x");
    found = FindInstalledVersions(root, profiles);
    CHECK(found.size() == 1 && found[0].profile == &profiles[1] && found[0].check.state == GameState::kInvalid);
    CHECK(found[0].check.detail.find(JPT_GUEST_IMAGE) == 0 && !ChooseVersion(found, "other"));
    InstallTestTree(root / "versions" / "other");
    found = FindInstalledVersions(root, profiles);
    CHECK(found.size() == 1 && found[0].check.state == GameState::kReady && ChooseVersion(found, "other") == &found[0]);
    CHECK(ReadFile(root / "versions" / "other" / "trespass.exe") == "ba7816bf");

    // A second version in its own folder: both listed, the choice follows the setting, any ready one otherwise.
    InstallTestTree(root / "versions" / "test");
    found = FindInstalledVersions(root, profiles);
    CHECK(found.size() == 2 && found[0].dir == root / "versions" / "test" && found[1].dir == root / "versions" / "other");
    CHECK(ChooseVersion(found, "other") && ChooseVersion(found, "other")->profile == &profiles[1]);  // the setting wins
    CHECK(ChooseVersion(found, "")->profile == &profiles[0]);  // else the first ready one the port can run
    CHECK(ChooseVersion(found, "missing")->profile == &profiles[0]);
    CHECK(ChooseVersion(found, "test")->profile == &profiles[0]);
    WriteFile(root / "versions" / "test" / "data" / "be.scn", "short");
    found = FindInstalledVersions(root, profiles);
    CHECK(found.size() == 2 && found[0].check.state == GameState::kInvalid &&
          ChooseVersion(found, "test") && ChooseVersion(found, "test")->profile == &profiles[1]);
    InstallTestTree(root / "versions" / "test");
    found = FindInstalledVersions(root, profiles);

    // Removing a managed version deletes its folder only.
    std::string error;
    const InstalledVersion managed = *ChooseVersion(found, "test");
    CHECK(RemoveVersion(managed, error) && !fs::exists(root / "versions" / "test"));
    CHECK(fs::exists(root / "trespass.exe") && fs::exists(root / "data" / "be.scn") && ReadFile(root / "SAVEGAME.000") == "save");
    CHECK(fs::exists(root / "versions" / "other" / JPT_GUEST_IMAGE));
    found = FindInstalledVersions(root, profiles);
    CHECK(found.size() == 1 && found[0].profile == &profiles[1]);

    // A folder that is not a version folder is never removed.
    InstalledVersion stray{&profiles[0], dir / "stray", {}};
    fs::create_directories(stray.dir);
    CHECK(!RemoveVersion(stray, error) && fs::exists(stray.dir));
}

static std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return char(std::tolower((unsigned char)c)); });
    return text;
}

static void TestGameVersionTable() {
    CHECK(jpt_game_version_by_id("en-116") == &jpt_game_versions[0] && jpt_game_version_by_id("EN-116"));
    CHECK(!jpt_game_version_by_id("ja-116") && !jpt_game_version_by_id(nullptr));
    std::set<std::string> ids;
    for (const jpt_game_version_t& v : jpt_game_versions) {
        ids.insert(v.id);
        CHECK(jpt_game_version_playable(&v) == (v.unplayable == nullptr));
        CHECK(std::string(v.lift).rfind("lift", 0) == 0);
        CHECK((std::string(v.lift) == "lift116") == (v.build == 116));
        CHECK(jpt_l10n_language(v.language_code) != nullptr);
        CHECK(std::strlen(v.id) < JPT_GAME_VERSION_ID_MAX && (v.saves == nullptr) == (std::string(v.id) == "en-116"));
    }
    CHECK(ids == std::set<std::string>({"en-116", "ru-116", "es-117", "ptbr-117"}));
    CHECK(jpt_game_version_playable(jpt_game_version_by_id("ru-116")));
    CHECK(jpt_game_version_playable(jpt_game_version_by_id("es-117")) && jpt_game_version_playable(jpt_game_version_by_id("ptbr-117")));

    // Profiles of the launcher: one per version, each manifest with one guest image, no installed executable, distinct
    // names, and a data file on the disc that no other version has.
    CHECK(KnownProfiles().size() == JPT_GAME_VERSION_COUNT);
    std::set<std::string> identity_digests;
    for (const GameProfile& p : KnownProfiles()) {
        std::set<std::string> names;
        int guest = 0, tables = 0;
        const InstallFile* identity = nullptr;
        for (const InstallFile& f : p.files) {
            const std::string lower = Lower(f.installed);
            CHECK(names.insert(lower).second && std::strlen(f.sha256) == 64);
            CHECK(f.kind == SourceKind::kDisc || p.cabinet);
            CHECK(lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".exe") != 0);
            CHECK(lower.find("trespass.exe") == std::string::npos && lower.find("tpassp") == std::string::npos);
            CHECK((f.role == FileRole::kStringTable) == (f.source_sha256 != nullptr && f.source_size != 0));
            if (f.role == FileRole::kGuestImage) {
                ++guest;
                CHECK(lower == JPT_GUEST_IMAGE && names.count(lower) == 1);
            }
            if (f.role == FileRole::kStringTable) {
                ++tables;
                CHECK(std::strlen(f.source_sha256) == 64 && f.size < f.source_size);
                CHECK(p.version->text_strings && Lower(p.version->text_strings) == "text\\strings.dat" && lower == "text/strings.dat");
            }
            if (f.kind == SourceKind::kDisc && f.role == FileRole::kData && Lower(f.source) == Lower(p.identity_file))
                identity = &f;
        }
        CHECK(guest == 1 && tables == (p.version->text_strings ? 1 : 0) && !p.discs.empty());
        CHECK(identity && std::string(identity->sha256) == p.identity_sha256);
        CHECK(std::string(p.identity_file).find(".exe") == std::string::npos);
        CHECK(identity_digests.insert(p.identity_sha256).second);
        for (const GameProfile& other : KnownProfiles())
            if (&other != &p)
                for (const InstallFile& f : other.files) CHECK(std::string(f.sha256) != p.identity_sha256);
    }
    char out[64];
    CHECK(jpt_game_version_saves_dir("U", "U\\saves", jpt_game_version_by_id("en-116"), out, sizeof out) &&
          std::string(out) == "U\\saves");
    CHECK(jpt_game_version_saves_dir("U", "U\\saves", jpt_game_version_by_id("ru-116"), out, sizeof out) &&
          std::string(out) == "U\\versions\\ru-116\\saves");

    jpt_config_t c;
    jpt_config_defaults(&c);
    CHECK(c.game_version[0] == 0 && jpt_config_valid(JPT_KEY_GAME_VERSION, "ru-116"));
    CHECK(jpt_config_valid(JPT_KEY_GAME_VERSION, "") && !jpt_config_valid(JPT_KEY_GAME_VERSION, "ja-116"));
}

static void TestHostResolution(const fs::path& dir) {
    const fs::path root = dir / "resolve";
    fs::remove_all(root);
    char out[MAX_PATH];
    const jpt_game_version_t* v = nullptr;
    const std::string r = root.string();
    CHECK(!jpt_game_version_resolve(r.c_str(), "", 1, out, sizeof out, &v));
    // A Spanish folder is found as installed and, the version being playable, chosen to run.
    WriteFile(root / "versions" / "es-117" / "trespass.exe", "x");  // an executable alone is not an installation
    CHECK(!jpt_game_version_installed(r.c_str(), jpt_game_version_by_id("es-117"), out, sizeof out));
    CHECK(!jpt_game_version_resolve(r.c_str(), "es-117", 1, out, sizeof out, &v));
    WriteFile(root / "versions" / "es-117" / JPT_GUEST_IMAGE, "x");
    CHECK(jpt_game_version_installed(r.c_str(), jpt_game_version_by_id("es-117"), out, sizeof out));
    CHECK(jpt_game_version_resolve(r.c_str(), "es-117", 1, out, sizeof out, &v) && v == jpt_game_version_by_id("es-117"));
    CHECK(std::string(out) == (root / "versions" / "es-117").string());
    // A chosen version that is not installed falls back to the first installed one.
    WriteFile(root / "versions" / "ru-116" / JPT_GUEST_IMAGE, "x");
    CHECK(jpt_game_version_resolve(r.c_str(), "ptbr-117", 1, out, sizeof out, &v) && v == jpt_game_version_by_id("ru-116"));
    CHECK(std::string(out) == (root / "versions" / "ru-116").string());
}

static void TestUserFiles(const fs::path& dir) {
    const fs::path old = dir / "oldgame", user = dir / "carry";
    WriteFile(old / "SAVEGAME.000", "a");
    WriteFile(old / "SAVEGAME.001", "b");
    WriteFile(old / JPT_REGISTRY_FILE, "Gore=dword:00000000\n");
    WriteFile(old / JPT_GUEST_IMAGE, "x");
    WriteFile(user / "SAVEGAME.001", "newer");
    CHECK(jpt_carry_user_files(old.string().c_str(), user.string().c_str(), nullptr) == 2);
    CHECK(ReadFile(user / "SAVEGAME.000") == "a" && ReadFile(user / "SAVEGAME.001") == "newer");
    CHECK(fs::exists(user / JPT_REGISTRY_FILE) && !fs::exists(user / JPT_GUEST_IMAGE) && fs::exists(old / "SAVEGAME.000"));
    CHECK(jpt_is_user_file("savegame.003") && jpt_is_user_file("recomp-registry.txt"));
    CHECK(!jpt_is_user_file(JPT_GUEST_IMAGE) && !jpt_is_user_file("be-130.swp"));
}

// ---- the release string table and the importer on the shapes of the four real profiles

#define RT_STRING_W MAKEINTRESOURCEW(6)

static std::vector<uint8_t> Bytes(const std::string& text) { return {text.begin(), text.end()}; }

static bool SameStrings(const std::vector<ReleaseString>& a, const std::vector<ReleaseString>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].bytes != b[i].bytes) return false;
    return true;
}

static bool ReadTable(const std::vector<uint8_t>& table, std::vector<ReleaseString>& out) {
    out.clear();
    return jpt_string_table_read(table.data(), table.size(),
                                 [](void* user, uint32_t id, const char* bytes, uint32_t length) {
                                     static_cast<std::vector<ReleaseString>*>(user)->push_back({id, std::string(bytes, length)});
                                 },
                                 &out) >= 0;
}

// The 16 entries of one RT_STRING block: each a length in UTF-16 units and the units.
static std::string StringBlock(const std::vector<std::pair<int, std::u16string>>& entries) {
    std::string block;
    for (int i = 0; i < 16; ++i) {
        std::u16string text;
        for (const auto& [index, value] : entries)
            if (index == i) text = value;
        const uint16_t length = static_cast<uint16_t>(text.size());
        block.append(reinterpret_cast<const char*>(&length), 2);
        block.append(reinterpret_cast<const char*>(text.data()), text.size() * 2);
    }
    return block;
}

// A PE file with only the given string blocks as resources: the test program's own copy, its resources replaced.
// Never run; the importer reads its resources as data.
static bool MakeResourcePe(const fs::path& path, const std::vector<std::pair<int, std::string>>& blocks, std::string& bytes) {
    wchar_t self[MAX_PATH];
    std::error_code ec;
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::create_directories(path.parent_path(), ec);
    fs::copy_file(self, path, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    HANDLE update = BeginUpdateResourceW(path.c_str(), TRUE);
    if (!update) return false;
    bool ok = true;
    for (const auto& [block, data] : blocks)
        ok = ok && UpdateResourceW(update, RT_STRING_W, MAKEINTRESOURCEW(static_cast<WORD>(block)), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                   const_cast<char*>(data.data()), static_cast<DWORD>(data.size())) != FALSE;
    if (!EndUpdateResourceW(update, !ok)) return false;
    bytes = ReadFile(path);
    return ok && !bytes.empty();
}

static const std::vector<ReleaseString> kExpectedStrings = {
    {0, "Hello"}, {2, "caf\xE9"}, {3, "A?"}, {21, "second"}, {65535, "edge"}};

static std::vector<std::pair<int, std::string>> StringBlocks() {
    return {{1, StringBlock({{0, u"Hello"}, {2, u"café"}, {3, u"AЖ"}})},
            {2, StringBlock({{5, u"second"}})},
            {4096, StringBlock({{15, u"edge"}})},
            {4097, StringBlock({{0, u"beyond the id limit"}})}};
}

static void TestStringTable(const fs::path& dir) {
    std::vector<uint8_t> table = BuildStringTable(kExpectedStrings);
    std::vector<ReleaseString> back;
    CHECK(table.size() == jpt_string_table_size(5, 5 + 4 + 2 + 6 + 4) && std::memcmp(table.data(), "JPTS", 4) == 0);
    CHECK(ReadTable(table, back) && SameStrings(back, kExpectedStrings));
    CHECK(ReadTable(BuildStringTable({}), back) && back.empty());
    std::vector<uint8_t> cut = table;
    cut.pop_back();
    CHECK(!ReadTable(cut, back));
    cut = table;
    cut[0] = 'X';
    CHECK(!ReadTable(cut, back) && !ReadTable({}, back));

    const fs::path scratch = dir / "derive";
    std::vector<uint8_t> derived;
    std::string error;
    CHECK(!DeriveStringTable(Bytes("MZ but not a program"), scratch, derived, error) && !error.empty());
    CHECK(!fs::exists(scratch / "resources.tmp"));

    std::string pe;
    CHECK(MakeResourcePe(dir / "resources.bin", StringBlocks(), pe));
    error.clear();
    CHECK(DeriveStringTable(Bytes(pe), scratch, derived, error) && error.empty());
    CHECK(ReadTable(derived, back) && SameStrings(back, kExpectedStrings) && derived == table);  // beyond 65535: dropped
    CHECK(!fs::exists(scratch / "resources.tmp"));

    std::string bare;
    CHECK(MakeResourcePe(dir / "no-strings.bin", {}, bare));
    CHECK(!DeriveStringTable(Bytes(bare), scratch, derived, error) && !error.empty());
    CHECK(!fs::exists(scratch / "resources.tmp"));
}

// The manifest and disc of a synthetic version shaped like one of the real profiles: en, es and ptbr copy their guest
// image from setup/tpassp6.exe on the disc; ru takes it from the cabinet and derives text/strings.dat from
// setup/trespass.exe, with its data files on the disc and in the cabinet. No original game file is involved.
struct Fixture {
    struct Row {
        SourceKind kind;
        std::string source, installed, content;
        FileRole role;
    };
    const jpt_game_version_t* version = nullptr;
    std::string guest;
    std::vector<Row> rows;
    std::deque<std::string> keep;
    std::vector<InstallFile> files;
    std::vector<DiscIdentity> discs;
    std::map<std::string, std::string> installed;  // what the version folder should hold, derived files aside
    std::vector<ReleaseString> strings;
    GameProfile profile{};

    const char* Keep(const std::string& text) {
        keep.push_back(text);
        return keep.back().c_str();
    }

    bool ru() const { return std::string(version->id) == "ru-116"; }

    // The pressed disc; the guest image's source and the text source are optional.
    std::vector<IsoInput> Disc(bool guest_source, bool text_source) const {
        std::vector<IsoInput> disc = {{"setup/directx/dsetup.dll", "not installed"}};
        std::vector<CabInput> cabinet;
        for (const Row& r : rows) {
            if (r.role == FileRole::kGuestImage && !guest_source) continue;
            if (r.role == FileRole::kStringTable && !text_source) continue;
            if (r.kind == SourceKind::kDisc) {
                disc.push_back({r.source, r.content});
            } else {
                const size_t slash = r.source.find('/');
                cabinet.push_back({slash == std::string::npos ? "" : r.source.substr(0, slash),
                                   slash == std::string::npos ? r.source : r.source.substr(slash + 1), r.content,
                                   r.content.size(), false});
            }
        }
        if (profile.cabinet) {
            const std::vector<uint8_t> bytes = BuildCabinet(cabinet);
            disc.push_back({profile.cabinet, std::string(bytes.begin(), bytes.end())});
        }
        return disc;
    }
};

static std::unique_ptr<Fixture> MakeFixture(size_t index, const std::string& pe, const fs::path& scratch) {
    auto fx = std::make_unique<Fixture>();
    fx->version = &jpt_game_versions[index];
    const std::string id = fx->version->id;
    fx->guest = "guest image of " + id + std::string(37, '#');
    const SourceKind disc = SourceKind::kDisc, cab = SourceKind::kCabinet;
    std::string identity_file;
    if (fx->ru()) {
        fx->rows = {{cab, "tpassp6.exe", JPT_GUEST_IMAGE, fx->guest, FileRole::kGuestImage},
                    {disc, "SETUP/MSVCIRT.DLL", "MSVCIRT.DLL", "m", FileRole::kData},
                    {disc, "README.TXT", "Readme.txt", "r", FileRole::kData},
                    {disc, "SETUP/TRESPASS.EXE", "text/strings.dat", pe, FileRole::kStringTable},
                    {disc, "TRESP.TTF", "text/tresp.ttf", "font of " + id, FileRole::kData},
                    {disc, "DATA/BE.SCN", "data/be.scn", "levelbe", FileRole::kData},
                    {cab, "menu/loader.ddf", "data/menu/loader.ddf", "menu loader of " + id, FileRole::kData}};
        identity_file = "tresp.ttf";
    } else {
        fx->rows = {{disc, "setup/tpassp6.exe", JPT_GUEST_IMAGE, fx->guest, FileRole::kGuestImage},
                    {disc, "setup/msvcirt.dll", "MSVCIRT.DLL", "m", FileRole::kData},
                    {disc, "readme.txt", "Readme.txt", "r", FileRole::kData},
                    {disc, "data/be.scn", "data/be.scn", "levelbe", FileRole::kData},
                    {disc, "data/menu/loader.ddf", "data/menu/loader.ddf", "loader of " + id, FileRole::kData}};
        identity_file = "data/menu/loader.ddf";
    }
    std::string identity_sha;
    for (const Fixture::Row& r : fx->rows) {
        std::string content = r.content;
        uint64_t source_size = 0;
        const char* source_sha = nullptr;
        if (r.role == FileRole::kStringTable) {
            std::vector<uint8_t> table;
            std::string error;
            CHECK(DeriveStringTable(Bytes(pe), scratch, table, error));
            CHECK(ReadTable(table, fx->strings));
            content.assign(table.begin(), table.end());
            source_size = r.content.size();
            source_sha = fx->Keep(Sha(r.content));
        } else {
            fx->installed[r.installed] = content;
        }
        if (Lower(r.source) == identity_file) identity_sha = Sha(content);
        fx->files.push_back({r.kind, fx->Keep(r.source), fx->Keep(r.installed), content.size(), fx->Keep(Sha(content)), r.role,
                             source_size, source_sha});
    }
    if (index == 0) fx->discs = {{"English retail (UK)", "FX_UK"}, {"English retail (US)", "FX_US"}};
    else fx->discs = {{"Synthetic pressing", "FX_DISC"}};
    fx->profile = {fx->version, fx->Keep(identity_file), fx->Keep(identity_sha), fx->discs,
                   fx->ru() ? "setup/data1.cab" : nullptr, fx->files};
    return fx;
}

static std::vector<std::string> Tree(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) names.push_back(Lower(fs::relative(it->path(), dir).generic_string()));
    return names;
}

static void TestImportShapes(const fs::path& dir) {
    const fs::path scratch = dir / "fixture-scratch";
    std::string pe;
    CHECK(MakeResourcePe(dir / "fixture-source.bin", StringBlocks(), pe));
    std::vector<std::unique_ptr<Fixture>> fixtures;
    GameProfile all[JPT_GAME_VERSION_COUNT];
    for (size_t i = 0; i < JPT_GAME_VERSION_COUNT; ++i) {
        fixtures.push_back(MakeFixture(i, pe, scratch));
        all[i] = fixtures[i]->profile;
    }
    CHECK(!fs::exists(scratch / "resources.tmp"));

    for (size_t i = 0; i < JPT_GAME_VERSION_COUNT; ++i) {
        const Fixture& fx = *fixtures[i];
        const std::string id = fx.version->id;
        const fs::path root = dir / ("shape-" + id), target = root / "versions" / id;
        fs::remove_all(root);
        std::string why;
        InstallHooks hooks;
        InstallHooks replace;
        replace.replace = true;
        CHECK(fx.profile.files.size() == fx.rows.size() && fx.strings.size() == (fx.ru() ? kExpectedStrings.size() : 0));

        // The disc without any original executable: the profile is recognised among all four and every data file is
        // validated; only the sources of the guest image (and the text) are absent, and nothing is installed.
        const fs::path bare = dir / ("shape-" + id + "-bare.iso"), full = dir / ("shape-" + id + "-full.iso");
        const std::string volume = fx.discs.back().volume_id;
        WriteImage(bare, IsoBuilder().Build(volume, fx.Disc(false, false), true));
        WriteImage(full, IsoBuilder().Build(volume, fx.Disc(true, true), true));
        IsoImage image;
        CHECK(image.Open(bare, why));
        DiscMatch m = IdentifyDisc(image, all, why);
        CHECK(m.profile == &all[i] && m.disc == &all[i].discs.back());
        Cabinet cabinet;
        CHECK(OpenCabinet(image, all[i], cabinet, why));
        const std::vector<PlannedFile> plan = PlanInstall(image, all[i].cabinet ? &cabinet : nullptr, all[i], why);
        size_t absent = 0;
        for (const PlannedFile& f : plan) {
            CHECK(f.present() != (f.file->role != FileRole::kData));
            absent += !f.present();
        }
        CHECK(plan.size() == fx.rows.size() && absent == (fx.ru() ? 2u : 1u));
        InstallResult r = InstallFromIso(bare, root, hooks, all);
        CHECK(r.status == InstallStatus::kNoGuestImageSource && !r.ok() && r.detail.find("the disc lacks ") == 0);
        CHECK(r.detail.find("the file the guest image is made from") != std::string::npos);
        CHECK(!fs::exists(root) && !fs::exists(StagingFolder(target)) && !fs::exists(BackupFolder(target)));
        CHECK(std::string(InstallStatusTitle(InstallStatus::kNoGuestImageSource)).find("lacks") != std::string::npos);
        if (fx.ru()) {  // the guest image present, the release's text source absent
            WriteImage(dir / "shape-ru-text.iso", IsoBuilder().Build(volume, fx.Disc(true, false), true));
            r = InstallFromIso(dir / "shape-ru-text.iso", root, hooks, all);
            CHECK(r.status == InstallStatus::kNoGuestImageSource && r.detail.find("the file the game text is made from") != std::string::npos);
            CHECK(!fs::exists(root));
        }

        // With the executable: the guest image is its bytes, the tree holds no original executable.
        IsoImage with_exe;
        CHECK(with_exe.Open(full, why));
        CHECK(IdentifyDisc(with_exe, all, why).profile == &all[i]);
        r = InstallFromIso(full, root, hooks, all);
        CHECK(r.status == InstallStatus::kInstalled && r.profile == &all[i] && r.dir == target && r.files == fx.rows.size());
        CHECK(ReadFile(target / JPT_GUEST_IMAGE) == fx.guest && Sha256File(target / JPT_GUEST_IMAGE) == Sha(fx.guest));
        CHECK(CheckGameFolder(target, all[i]).state == GameState::kReady);
        for (const auto& [name, content] : fx.installed) CHECK(ReadFile(target / name) == content);
        const std::vector<std::string> tree = Tree(target);
        CHECK(tree.size() == fx.rows.size());
        for (const std::string& name : tree)
            CHECK(name.find(".exe") == std::string::npos && name.find("tpassp") == std::string::npos && name != "resources.tmp");
        CHECK(!fs::exists(StagingFolder(target)) && !fs::exists(BackupFolder(target)));
        if (fx.ru()) {
            std::vector<ReleaseString> strings;
            CHECK(ReadTable(Bytes(ReadFile(target / "text" / "strings.dat")), strings) && SameStrings(strings, kExpectedStrings));
            CHECK(!fs::exists(target / "text" / "trespass.exe"));
        }

        // Importing it again, even from a disc without the executable, copies nothing; a replacement that cannot
        // be made leaves the installed version byte for byte as it was.
        WriteFile(target / "trespass.exe", "an old executable");
        CHECK(InstallFromIso(full, root, hooks, all).status == InstallStatus::kUpToDate);
        CHECK(InstallFromIso(bare, root, hooks, all).status == InstallStatus::kUpToDate);
        CHECK(CheckGameFolder(target, all[i]).state == GameState::kReady && ReadFile(target / "trespass.exe") == "an old executable");
        r = InstallFromIso(bare, root, replace, all);
        CHECK(r.status == InstallStatus::kNoGuestImageSource && ReadFile(target / JPT_GUEST_IMAGE) == fx.guest);
        CHECK(ReadFile(target / "trespass.exe") == "an old executable" && Tree(target).size() == fx.rows.size() + 1);
        CHECK(!fs::exists(StagingFolder(target)) && !fs::exists(BackupFolder(target)));
        std::string error;
        const std::vector<InstalledVersion> found = FindInstalledVersions(root, all);
        CHECK(found.size() == 1 && found[0].profile == &all[i] && RemoveVersion(found[0], error) && !fs::exists(target));

        // Old executables are ignored: in the game folder (an installation made before versions existed), and in the
        // version folder without a guest image. Neither is copied, neither makes a version.
        const fs::path old_root = dir / ("shape-" + id + "-old"), old_target = old_root / "versions" / id;
        fs::remove_all(old_root);
        for (const Fixture::Row& row : fx.rows) {
            if (row.role == FileRole::kData) {
                WriteFile(old_root / row.installed, row.content);
                WriteFile(old_target / row.installed, row.content);
            }
        }
        WriteFile(old_root / "trespass.exe", "game folder executable");
        WriteFile(old_target / "trespass.exe", "version folder executable");
        CHECK(FindInstalledVersions(old_root, all).empty() && !ChooseVersion(FindInstalledVersions(old_root, all), id));
        CHECK(CheckGameFolder(old_target, all[i]).state == GameState::kMissing);
        r = InstallFromIso(full, old_root, hooks, all);
        CHECK(r.status == InstallStatus::kInstalled && CheckGameFolder(old_target, all[i]).state == GameState::kReady);
        CHECK(ReadFile(old_root / "trespass.exe") == "game folder executable" && fs::exists(old_root / "data" / "be.scn"));
        for (const std::string& name : Tree(old_target)) CHECK(name.find(".exe") == std::string::npos);
        CHECK(!fs::exists(BackupFolder(old_target)));
    }

    // The four pressings tell each other apart: no disc is taken for another profile's, with or without the executable.
    for (size_t i = 0; i < JPT_GAME_VERSION_COUNT; ++i)
        for (const bool exe : {false, true}) {
            IsoImage image;
            std::string why;
            WriteImage(dir / "cross.iso", IsoBuilder().Build("ANY_LABEL", fixtures[i]->Disc(exe, exe), true));
            CHECK(image.Open(dir / "cross.iso", why) && IdentifyDisc(image, all, why).profile == &all[i]);
        }
}

// TRESPASSER_TEST_ISO=<image>[;<image>...]: each real disc is identified and planned in full (read only). With
// TRESPASSER_TEST_INSTALL_ROOT=<folder> each one is also imported there, in order, and imported again.
static void TestRealDiscs() {
    char list[4096], install_root[MAX_PATH];
    if (!GetEnvironmentVariableA("TRESPASSER_TEST_ISO", list, sizeof list)) {
        std::printf("real discs: skipped (TRESPASSER_TEST_ISO not set)\n");
        return;
    }
    const bool install = GetEnvironmentVariableA("TRESPASSER_TEST_INSTALL_ROOT", install_root, sizeof install_root) != 0;
    std::string rest = list;
    while (!rest.empty()) {
        const size_t semi = rest.find(';');
        const std::string path = rest.substr(0, semi);
        rest = semi == std::string::npos ? "" : rest.substr(semi + 1);
        IsoImage iso;
        std::string why;
        CHECK(iso.Open(path, why));
        const DiscMatch m = IdentifyDisc(iso, KnownProfiles(), why);
        CHECK(m.profile != nullptr);
        if (!m.profile) {
            std::printf("real disc %s: %s\n", path.c_str(), why.c_str());
            continue;
        }
        Cabinet cabinet;
        CHECK(OpenCabinet(iso, *m.profile, cabinet, why));
        const std::vector<PlannedFile> plan = PlanInstall(iso, m.profile->cabinet ? &cabinet : nullptr, *m.profile, why);
        uint64_t bytes = 0;
        for (const PlannedFile& f : plan) bytes += f.file->size;
        CHECK(plan.size() == m.profile->files.size());
        std::printf("real disc %s: volume \"%s\", %s: %s (%s), %zu files, %llu bytes\n", path.c_str(),
                    iso.volume_id().c_str(), m.disc->edition, m.profile->label(), m.profile->id(), plan.size(),
                    static_cast<unsigned long long>(bytes));
        if (!install) continue;
        InstallHooks hooks;
        hooks.log = [](const std::string& line) { std::printf("  %s\n", line.c_str()); };
        const InstallResult r = InstallFromIso(path, install_root, hooks);
        CHECK(r.ok() && r.profile == m.profile);
        std::printf("  import: %s (%s), %zu files, %zu repaired\n", InstallStatusTitle(r.status), r.detail.c_str(), r.files,
                    r.repaired);
        if (!r.ok()) continue;
        CHECK(CheckGameFolder(r.dir, *m.profile).state == GameState::kReady);
        const InstallResult again = InstallFromIso(path, install_root, InstallHooks{});
        CHECK(again.status == InstallStatus::kUpToDate && again.dir == r.dir);
    }
    if (install) {
        for (const InstalledVersion& v : FindInstalledVersions(install_root))
            std::printf("installed: %s in %s: %s\n", v.profile ? v.profile->id() : "unknown", Utf8(v.dir).c_str(),
                        GameStateTitle(v.check.state));
    }
}

// ---- Language: one choice in Gameplay, the launcher in English except for Português Brasil

static std::string ReadSource(const char* relative) {
    std::ifstream in(fs::path(JPT_SOURCE_DIR) / relative, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static bool HasRow(const char* english) {
    for (size_t i = 0; i < kPtBrUiTextCount; ++i)
        if (std::strcmp(kPtBrUiText[i].english, english) == 0) return true;
    return false;
}

static std::string Conversions(const char* text) {
    std::string out;
    for (const char* p = text; *p; ++p) {
        if (*p != '%') continue;
        if (p[1] == '%') ++p;
        else out += p[1];
    }
    return out;
}

static size_t Spaces(const char* text, bool leading) {
    const size_t n = std::strlen(text);
    size_t count = 0;
    while (count < n && text[leading ? count : n - 1 - count] == ' ') ++count;
    return count;
}

static void TestUiTable() {
    std::map<std::string, int> seen;
    for (size_t i = 0; i < kPtBrUiTextCount; ++i) {
        const UiTextRow& row = kPtBrUiText[i];
        CHECK(row.english[0] && row.text[0]);
        CHECK(++seen[row.english] == 1);
        CHECK(Conversions(row.english) == Conversions(row.text));
        CHECK(Spaces(row.english, true) == Spaces(row.text, true) && Spaces(row.english, false) == Spaces(row.text, false));
    }
    for (size_t i = 0; i < kPtBrPhraseCount; ++i) {
        CHECK(kPtBrPhrases[i].english[0] && kPtBrPhrases[i].text[0]);
    }
    CHECK(kPtBrUiTextCount > 200);
}

static void TestLauncherLanguage(const fs::path& dir) {
    const char* english_only[] = {"en", "fr", "de", "es", "ru", "it", "xx", ""};
    SetLauncherLanguage("en");
    CHECK(!LauncherIsPortuguese() && std::string(Tr("Play")) == "Play" && Tr(std::string("Display")) == "Display");
    SetLauncherLanguage("pt-BR");
    CHECK(LauncherIsPortuguese());
    const std::pair<const char*, const char*> pages[] = {
        {"Play", "Jogar"},       {"Display", "Tela"},     {"Graphics", "Gráficos"}, {"Gameplay", "Jogabilidade"},
        {"Controls", "Controles"}, {"Cheats", "Trapaças"},  {"Bug Report", "Relatar Problema"}, {"About", "Sobre"},
        {"Language", "Idioma"},  {"Save", "Salvar"},     {"Quit", "Sair"}};
    for (const auto& [english, portuguese] : pages) CHECK(std::string(Tr(english)) == portuguese);
    CHECK(std::string(Tr("A text that is in no table")) == "A text that is in no table");
    CHECK(TrFormat("Connected (%d controllers)", 2) == "Conectado (2 controles)");
    CHECK(TrFormat("GameStrings conversion by %s", "Nick3069") == "conversão das GameStrings por Nick3069");
    CHECK(TrMessage("missing data/a.bin") == "ausente: data/a.bin");
    CHECK(TrMessage("data/a.bin has 5 bytes, expected 9") == "data/a.bin tem 5 bytes, esperado 9");
    CHECK(TrMessage("Left trigger (LT) + Right stick") == "Gatilho esquerdo (LT) + Analógico direito");
    CHECK(TrMessage("the disc has no data/ folder") == "o disco não tem data/ (pasta)");
    CHECK(TrMessage("W") == "W" && TrMessage("") == "");
    for (const char* language : english_only) {
        SetLauncherLanguage(language);
        CHECK(!LauncherIsPortuguese() && std::string(Tr("Play")) == "Play" && std::string(Tr("About")) == "About");
        CHECK(TrMessage("missing data/a.bin") == "missing data/a.bin");
        CHECK(TrFormat("Connected (%d controllers)", 2) == "Connected (2 controllers)");
    }

    // the launcher follows the setting at once: a pending choice, no Save, no restart
    LauncherSettings settings(dir / "language.ini");
    CHECK(settings.Get(JPT_KEY_LANGUAGE) == "en");
    SetLauncherLanguage(settings.Get(JPT_KEY_LANGUAGE));
    CHECK(std::string(Tr("Play")) == "Play");
    settings.Set(JPT_KEY_LANGUAGE, "pt-BR");
    SetLauncherLanguage(settings.Get(JPT_KEY_LANGUAGE));
    CHECK(std::string(Tr("Play")) == "Jogar");
    for (const char* language : {"fr", "de", "es", "ru", "it", "en"}) {
        settings.Set(JPT_KEY_LANGUAGE, "pt-BR");
        SetLauncherLanguage(settings.Get(JPT_KEY_LANGUAGE));
        CHECK(std::string(Tr("Play")) == "Jogar");
        settings.Set(JPT_KEY_LANGUAGE, language);
        SetLauncherLanguage(settings.Get(JPT_KEY_LANGUAGE));
        CHECK(std::string(Tr("Play")) == "Play" && std::string(Tr("Gameplay")) == "Gameplay");
    }
    {
        std::ofstream(dir / "bad_language.ini") << "language = klingon\n";
        const LauncherSettings invalid(dir / "bad_language.ini");
        CHECK(invalid.Get(JPT_KEY_LANGUAGE) == "en");
        std::ofstream(dir / "saved_language.ini") << "language = pt-BR\n";
        const LauncherSettings saved(dir / "saved_language.ini");
        CHECK(saved.Get(JPT_KEY_LANGUAGE) == "pt-BR");
    }
    SetLauncherLanguage("en");
}

// Every text the launcher shows has a row: the shared tables, the page and status titles and the literals of the pages.
static void TestUiCoverage() {
    int missing = 0;
    const auto require = [&missing](const char* english, const char* where) {
        if (!HasRow(english)) {
            ++missing;
            std::printf("no Português Brasil row for \"%s\" (%s)\n", english, where);
        }
    };
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto wording = [&lower](const char* text) {
        const size_t n = std::strlen(text);
        for (size_t i = 0; i + 1 < n; ++i)
            if (lower(text[i]) && lower(text[i + 1])) return true;
        return false;
    };
    const std::set<std::string> kept = {"Create", "Start", "Nintendo", "Xbox", "PlayStation", "Steam Deck"};
    for (const PageId page : kPageOrder) require(PageName(page), "page");
    for (const GameState state : {GameState::kReady, GameState::kMissing, GameState::kInvalid})
        require(GameStateTitle(state), "game state");
    for (int status = 0; status <= static_cast<int>(InstallStatus::kActivateFailed); ++status)
        require(InstallStatusTitle(static_cast<InstallStatus>(status)), "install status");
    for (int outcome = 0; outcome <= static_cast<int>(UserDataOutcome::kExplicit); ++outcome)
        require(UserDataFolderHelp(static_cast<UserDataOutcome>(outcome)), "user data");
    for (const jpt_aspect_t& aspect : jpt_aspects)
        if (wording(aspect.label)) require(aspect.label, "aspect ratio");
    for (const jpt_render_preset_t& preset : jpt_render_presets)
        if (wording(preset.label)) require(preset.label, "render resolution");
    for (int style = 0; style < JPT_GLYPHS_COUNT; ++style)
        if (!kept.count(jpt_glyph_style_labels[style])) require(jpt_glyph_style_labels[style], "glyph style");
    for (const jpt_pad_action_info_t& action : jpt_pad_actions) require(action.name, "gamepad action");
    for (int family = 0; family < JPT_FAMILY_COUNT; ++family) {
        for (int control = 0; control < JPT_PAD_CONTROL_COUNT; ++control) {
            const char* name = jpt_pad_control_name(static_cast<jpt_pad_family_t>(family), control);
            if (name && wording(name) && !kept.count(name)) require(name, "gamepad control");
        }
    }
    jpt_key_binding_t keymap[JPT_KEYMAP_COUNT];
    jpt_keymap_default(keymap);
    jpt_gamepad_settings_t pad;
    jpt_gamepad_defaults(&pad);
    for (int gamepad = 0; gamepad < 2; ++gamepad) {
        jpt_prompt_context_t context{};
        context.gamepad = gamepad;
        context.family = JPT_FAMILY_XBOX;
        context.settings = &pad;
        context.keymap = keymap;
        context.keymap_count = JPT_KEYMAP_COUNT;
        jpt_guide_row_t rows[JPT_GUIDE_MAX_ROWS];
        const int count = jpt_controls_guide(&context, rows, JPT_GUIDE_MAX_ROWS);
        for (int i = 0; i < count; ++i) require(rows[i].action, "controls guide");
    }
    for (const jpt_l10n_credit_t& credit : jpt_l10n_credits) require(credit.content, "translation credit");

    // the literals of the pages: whatever reads as text must have a row
    const std::regex group(R"re("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)re");
    const std::regex piece(R"re("((?:[^"\\]|\\.)*)")re");
    for (const char* file : {"launcher/launcher.cpp", "launcher/options.cpp", "launcher/incident_page.cpp", "launcher/my_reports_page.cpp",
                              "launcher/incident_client.cpp", "launcher/updater/update_ui.cpp"}) {
        std::string source = ReadSource(file);
        const size_t stop = source.find("extern \"C\" jpt_launcher_result_t jpt_launcher_run");
        if (stop != std::string::npos) source.resize(stop);
        for (std::sregex_iterator it(source.begin(), source.end(), group), end; it != end; ++it) {
            std::string text;
            const std::string joined = it->str();
            for (std::sregex_iterator p(joined.begin(), joined.end(), piece); p != end; ++p) text += (*p)[1].str();
            const size_t line_start = source.rfind('\n', size_t(it->position())) + 1;
            const std::string line = source.substr(line_start, source.find('\n', size_t(it->position())) - line_start);
            const bool single_word = text.find(' ') == std::string::npos;
            const bool camel = single_word && std::any_of(text.begin() + 1, text.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
            const bool prose = !single_word && wording(text.c_str());
            const bool title = single_word && text.size() > 3 && text[0] >= 'A' && text[0] <= 'Z' && !camel &&
                               std::regex_match(text, std::regex("[A-Za-z]+"));
            if (line.find("Log(") != std::string::npos || line.find("log(") != std::string::npos || line.find("Say(") != std::string::npos || line.find("#include") != std::string::npos ||
                text.find('\\') != std::string::npos || text.find('/') != std::string::npos ||
                text.find("##") != std::string::npos || text.rfind("--", 0) == 0 || text.find(".png") != std::string::npos ||
                text == "JURASSIC PARK: TRESPASSER" || !(prose || title))
                continue;
            require(text.c_str(), file);
        }
    }
    CHECK(missing == 0);
}

static void TestLanguageNames() {
    const char* english_names[] = {"English", "Brazilian Portuguese", "French", "German", "Spanish", "Russian", "Italian"};
    const char* portuguese_names[] = {"Inglês", "Português Brasil", "Francês", "Alemão", "Espanhol", "Russo", "Italiano"};
    const char* codes[] = {"en", "pt-BR", "fr", "de", "es", "ru", "it"};
    CHECK(std::size(jpt_l10n_languages) == std::size(codes));
    for (size_t i = 0; i < std::size(codes); ++i) {
        CHECK(std::string(jpt_l10n_languages[i].code) == codes[i] && std::string(jpt_l10n_languages[i].name) == english_names[i]);
        for (const char* launcher : {"en", "fr", "de", "es", "ru", "it", "pt-BR", "en"}) {
            SetLauncherLanguage(launcher);
            const bool portuguese = std::string(launcher) == "pt-BR";
            CHECK(std::string(Tr(jpt_l10n_languages[i].name)) == (portuguese ? portuguese_names[i] : english_names[i]));
        }
    }
    {
        const char* listed_names[] = {"English", "Brazilian Portuguese", "French", "German", "Italian", "Russian", "Spanish"};
        const char* listed_codes[] = {"en", "pt-BR", "fr", "de", "it", "ru", "es"};
        for (int i = 0; i < JPT_L10N_LANGUAGE_COUNT; ++i) {
            const jpt_l10n_language_t* language = jpt_l10n_language_listed(i);
            CHECK(language && std::string(language->name) == listed_names[i] && std::string(language->code) == listed_codes[i]);
            jpt_config_t config;
            jpt_config_defaults(&config);
            CHECK(jpt_config_apply(&config, JPT_KEY_LANGUAGE, listed_codes[i]) && std::string(config.language) == listed_codes[i]);
        }
        jpt_config_t config;
        jpt_config_defaults(&config);
        CHECK(std::string(config.language) == "en" && std::string(jpt_default_value(JPT_KEY_LANGUAGE)) == "en");
        CHECK(std::string(jpt_l10n_languages[4].code) == "es" && std::string(jpt_l10n_languages[6].code) == "it");
        const std::string launcher = ReadSource("launcher/launcher.cpp");
        CHECK(launcher.find("jpt_l10n_language_listed(i)") != std::string::npos &&
              launcher.find("for (const jpt_l10n_language_t& language : jpt_l10n_languages)") == std::string::npos);
    }
    SetLauncherLanguage("en");
    CHECK(std::string(Tr("Language")) == "Language");
    SetLauncherLanguage("pt-BR");
    CHECK(std::string(Tr("Language")) == "Idioma");
    CHECK(std::string(Tr("Changes the game text, menus and subtitles. Voices, effects and music remain in English.")) ==
          "Altera os textos, menus e legendas do jogo. Vozes, efeitos e música permanecem em inglês.");
    CHECK(std::string(Tr("PC port completion")) == "Complemento do port para PC" && std::string(Tr("official")) == "oficial" &&
          std::string(Tr(JPT_L10N_PORT_CONTENT)) != JPT_L10N_PORT_CONTENT);
    CHECK(std::string(Tr("Frame rate in title")) == "Taxa de quadros no título" &&
          std::string(Tr("Render resolution in title")) == "Resolução de render no título" &&
          std::string(Tr("Shows the frame rate in the window title.")) == "Mostra a taxa de quadros no título da janela." &&
          std::string(Tr("Shows the game's render resolution in the window title.")) ==
              "Mostra a resolução de render do jogo no título da janela.");
    CHECK(std::string(Tr("Frame counter")) == "Contador de quadros" &&
          std::string(Tr("Shows the frame rate in the top right corner of the game. F6 shows or hides it while playing.")) !=
              "Shows the frame rate in the top right corner of the game. F6 shows or hides it while playing.");
    CHECK(std::string(Tr("Off")) == "Desligado" && std::string(Tr("On")) == "Ligado");
    // English -> Português Brasil -> Français -> Português Brasil -> English, with no restart in between
    for (const char* language : {"en", "pt-BR", "fr", "pt-BR", "en"}) {
        SetLauncherLanguage(language);
        CHECK(std::string(Tr("Gameplay")) == (std::string(language) == "pt-BR" ? "Jogabilidade" : "Gameplay"));
    }
    SetLauncherLanguage("en");
}

static void TestGoreAndTitleOptions() {
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const std::string options = ReadSource("launcher/options.cpp");
    const std::string table = ReadSource("launcher/localization/pt_br_ui_text.cpp");
    for (const char* text : {"Gore", "gore", "Segmented(\"##gore"}) {
        CHECK(launcher.find(text) == std::string::npos && options.find(text) == std::string::npos &&
              table.find(text) == std::string::npos);
    }
    CHECK(launcher.find("registry_.SetDword(\"Gore\"") == std::string::npos);
    const size_t gameplay = launcher.find("void PageGameplay(");
    const size_t controls = launcher.find("// ---- Controls");
    const std::string body = launcher.substr(gameplay, controls - gameplay);
    const size_t language = body.find("ui.Combo(\"Language\""), skip = body.find("\"Skip Intro\""),
                 fps = body.find("\"Frame rate in title\""), counter = body.find("\"Frame counter\""),
                 resolution = body.find("\"Render resolution in title\"");
    CHECK(language < skip && skip < fps && fps < counter && counter < resolution && resolution != std::string::npos);
    CHECK(body.find("overage") == std::string::npos && body.find("JPT_KEY_SHOW_FRAME_COUNTER") != std::string::npos);
    CHECK(body.find("JPT_KEY_SHOW_RESOLUTION_TITLE") != std::string::npos && body.find("JPT_KEY_SHOW_FPS") != std::string::npos);
    for (int i = 0; i < JPT_SETTING_COUNT; ++i) CHECK(std::string(jpt_settings[i].key).find("gore") == std::string::npos);
    CHECK(std::string(jpt_default_value(JPT_KEY_SHOW_FPS)) == "1" && std::string(jpt_default_value(JPT_KEY_SHOW_RESOLUTION_TITLE)) == "0" &&
          std::string(jpt_default_value(JPT_KEY_SHOW_FRAME_COUNTER)) == "0");
}

static void TestGameplayOrder() {
    constexpr GameplayItem kExpected[] = {
        GameplayItem::kLanguage,
        GameplayItem::kMinimap,
        GameplayItem::kCompass,
        GameplayItem::kWaypoints,
        GameplayItem::kVideoSubtitles,
        GameplayItem::kSkipIntro,
        GameplayItem::kTrespasserCeHeading,
        GameplayItem::kDualStow,
        GameplayItem::kActiveAi,
        GameplayItem::kOfficialPatchHeading,
        GameplayItem::kOfficialPatchFixes,
        GameplayItem::kFrameRateInTitle,
        GameplayItem::kFrameCounter,
        GameplayItem::kRenderResolutionInTitle,
    };
    CHECK(std::size(kGameplayOrder) == std::size(kExpected));
    for (size_t i = 0; i < std::size(kExpected) && i < std::size(kGameplayOrder); ++i) {
        CHECK(kGameplayOrder[i] == kExpected[i]);
    }
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const size_t gameplay = launcher.find("void PageGameplay(");
    const size_t controls = launcher.find("// ---- Controls");
    CHECK(gameplay != std::string::npos && controls != std::string::npos && gameplay < controls);
    const std::string body = launcher.substr(gameplay, controls - gameplay);
    CHECK(body.find("for (const GameplayItem item : kGameplayOrder)") != std::string::npos);
    const char* const markers[] = {
        "case GameplayItem::kLanguage:",
        "case GameplayItem::kMinimap:",
        "case GameplayItem::kCompass:",
        "case GameplayItem::kWaypoints:",
        "case GameplayItem::kVideoSubtitles:",
        "case GameplayItem::kSkipIntro:",
        "case GameplayItem::kTrespasserCeHeading:",
        "case GameplayItem::kDualStow:",
        "case GameplayItem::kActiveAi:",
        "case GameplayItem::kOfficialPatchHeading:",
        "case GameplayItem::kOfficialPatchFixes:",
        "case GameplayItem::kFrameRateInTitle:",
        "case GameplayItem::kFrameCounter:",
        "case GameplayItem::kRenderResolutionInTitle:",
    };
    size_t prev = 0;
    for (const char* marker : markers) {
        const size_t at = body.find(marker);
        CHECK(at != std::string::npos && at > prev);
        prev = at;
    }
    CHECK(body.find("ui.Toggle(\"Enable Compass\", kCompassHelp, JPT_KEY_COMPASS, true, \"Off\", \"On\")") != std::string::npos);
    CHECK(body.find("ui.Toggle(\"Enable Waypoints\", kWaypointsHelp, JPT_KEY_WAYPOINTS, true, \"Off\", \"On\")") != std::string::npos);
    CHECK(std::string(jpt_default_value(JPT_KEY_MINIMAP)) == "1" && std::string(jpt_default_value(JPT_KEY_COMPASS)) == "1" &&
          std::string(jpt_default_value(JPT_KEY_WAYPOINTS)) == "1");
    CHECK(HasRow("Enable Compass") && HasRow("Enable Waypoints") &&
          HasRow("Shows the compass strip at the top of the screen, with the cardinal directions. Off hides only the strip: the minimap and the objective waypoints stay as set.") &&
          HasRow("Shows the current objective on the screen, on the compass and on the minimap: the diamond, the arrow, the distance and the route markers. Off hides only those; objectives and F4 Show Objective still work."));
}

static void TestNoLanguagePage() {
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const std::string options = ReadSource("launcher/options.cpp");
    CHECK(options.find("kLanguage") == std::string::npos && launcher.find("PageLanguage") == std::string::npos);
    CHECK(!fs::exists(fs::path(JPT_SOURCE_DIR) / "assets" / "launcher" / "icons" / "language.svg"));
    CHECK(!fs::exists(fs::path(JPT_SOURCE_DIR) / "launcher" / "translations.cpp"));
    const size_t gameplay = launcher.find("void PageGameplay(");
    const size_t controls = launcher.find("// ---- Controls");
    CHECK(gameplay != std::string::npos && controls != std::string::npos && gameplay < controls);
    const std::string body = launcher.substr(gameplay, controls - gameplay);
    CHECK(body.find("ui.Combo(\"Language\"") != std::string::npos && body.find("LanguageKey()") != std::string::npos &&
          body.find("EffectiveLanguage()") != std::string::npos);
    size_t combos = 0;
    for (size_t at = launcher.find("\"Language\""); at != std::string::npos; at = launcher.find("\"Language\"", at + 1)) ++combos;
    CHECK(combos == 1);
    for (const char* forbidden : {"\"game_text_language\"", "\"subtitle_language\"", "JPT_KEY_LEGACY", "Install downloaded",
                                  "Open TresCom", "BrowseForFolder"})
        CHECK(launcher.find(forbidden) == std::string::npos);
    CHECK(launcher.find("Translation credits") != std::string::npos);
    std::size_t sections = 0;
    for (const PageId page : kPageOrder) sections += std::string(PageName(page)) == "Language";
    CHECK(sections == 0 && std::size(kPageOrder) == 8);
}

static std::vector<jpt_gpu_t> Gpus(std::initializer_list<const char*> names) {
    std::vector<jpt_gpu_t> gpus;
    for (const char* name : names) {
        jpt_gpu_t gpu{};
        std::snprintf(gpu.name, sizeof gpu.name, "%s", name);
        gpus.push_back(gpu);
    }
    jpt_gpu_assign_labels(gpus.data(), static_cast<int>(gpus.size()));
    return gpus;
}

static int ResolveGpu(const std::vector<jpt_gpu_t>& gpus, const char* request) {
    return jpt_gpu_resolve(gpus.data(), static_cast<int>(gpus.size()), request);
}

// The name of the adapter a device created on DXGI adapter `index` really runs on, as d3d11_present.c creates it.
static std::string DeviceAdapterName(int index) {
    std::string name;
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return name;
    if (SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(index), &adapter))) {
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                        reinterpret_cast<const D3D_FEATURE_LEVEL*>(jpt_gpu_feature_levels),
                                        JPT_GPU_FEATURE_LEVEL_COUNT, D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
            IDXGIDevice* dxgi = nullptr;
            IDXGIAdapter* used = nullptr;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
                SUCCEEDED(dxgi->GetAdapter(&used)) && SUCCEEDED(used->GetDesc(&desc))) {
                char text[JPT_GPU_NAME_MAX] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, text, sizeof text, nullptr, nullptr);
                name = text;
                while (!name.empty() && name.back() == ' ') name.pop_back();
            }
            if (used) used->Release();
            if (dxgi) dxgi->Release();
            device->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return name;
}

static void TestGraphicsCard(const fs::path& dir) {
    const char* intel = "Intel(R) UHD Graphics";
    const char* nvidia = "NVIDIA GeForce RTX 4070";
    const std::vector<jpt_gpu_t> two = Gpus({intel, nvidia});
    CHECK(ResolveGpu(two, "") == 0);       // no saved card: the first usable one, whatever its vendor
    CHECK(ResolveGpu(two, nullptr) == 0);
    CHECK(ResolveGpu(two, nvidia) == 1);
    CHECK(ResolveGpu(two, intel) == 0);
    CHECK(ResolveGpu(two, "AMD Radeon RX 7800 XT") == 0);  // a card no longer installed
    CHECK(ResolveGpu(two, "NVIDIA GeForce RTX 407") == 0);  // exact labels only
    CHECK(ResolveGpu(Gpus({nvidia, intel}), "") == 0);       // enumeration order, never a vendor preference
    CHECK(GpuChoices(two) == (std::vector<std::string>{intel, nvidia}));
    CHECK(std::string(two[0].label) == intel && std::string(two[1].label) == nvidia);  // unique names: no suffix

    // An unusable entry keeps its DXGI position: the choice that follows it is its own adapter, not index - 1.
    const std::vector<jpt_gpu_t> gaps = Gpus({nvidia, "", intel});
    CHECK(ResolveGpu(gaps, nvidia) == 0 && ResolveGpu(gaps, intel) == 2 && ResolveGpu(gaps, "") == 0);
    CHECK(GpuChoices(gaps) == (std::vector<std::string>{nvidia, intel}));
    CHECK(jpt_gpu_find(gaps.data(), 3, "") == -1 && jpt_gpu_find(gaps.data(), 3, "gone") == -1);
    const std::vector<jpt_gpu_t> first_bad = Gpus({"", intel, "", nvidia});
    CHECK(ResolveGpu(first_bad, "") == 1 && ResolveGpu(first_bad, nvidia) == 3 && ResolveGpu(first_bad, "gone") == 1);
    CHECK(ResolveGpu(Gpus({"", ""}), nvidia) == -1 && ResolveGpu(Gpus({}), "") == -1);
    CHECK(jpt_gpu_resolve(nullptr, 0, "") == -1);
    CHECK(GpuChoices({}).empty() && GpuChoices(Gpus({"", ""})).empty());

    // Two adapters of the same name stay two choices; only the repeat is suffixed.
    const std::string second = std::string(nvidia) + " (2)";
    const std::vector<jpt_gpu_t> twins = Gpus({nvidia, nvidia});
    CHECK(GpuChoices(twins) == (std::vector<std::string>{nvidia, second}));
    CHECK(ResolveGpu(twins, nvidia) == 0 && ResolveGpu(twins, second.c_str()) == 1);  // a saved old name = the first match
    CHECK(ResolveGpu(twins, "") == 0 && ResolveGpu(twins, (std::string(nvidia) + " (3)").c_str()) == 0);
    const std::vector<jpt_gpu_t> mixed = Gpus({nvidia, intel, "", nvidia, nvidia});
    CHECK(GpuChoices(mixed) == (std::vector<std::string>{nvidia, intel, second, std::string(nvidia) + " (3)"}));
    CHECK(ResolveGpu(mixed, second.c_str()) == 3 && ResolveGpu(mixed, (std::string(nvidia) + " (3)").c_str()) == 4 &&
          ResolveGpu(mixed, intel) == 1);

    // The setting has no fixed default; any name is kept as it is.
    CHECK(jpt_default_value(JPT_KEY_GRAPHICS_CARD) && !*jpt_default_value(JPT_KEY_GRAPHICS_CARD));
    jpt_config_t c;
    jpt_config_defaults(&c);
    CHECK(c.graphics_card[0] == 0);
    CHECK(jpt_config_apply(&c, JPT_KEY_GRAPHICS_CARD, nvidia) && std::string(c.graphics_card) == nvidia);
    CHECK(!jpt_config_apply(&c, JPT_KEY_GRAPHICS_CARD, std::string(JPT_GPU_LABEL_MAX, 'x').c_str()) &&
          std::string(c.graphics_card) == nvidia);

    // The launcher writes the resolved card; the game reads it back and resolves the same adapter.
    const fs::path ini = dir / "graphics_card.ini";
    const auto launcher_then_game = [&](const std::string& stored) {
        WriteFile(ini, stored.empty() ? "renderer = d3d11\n" : "graphics_card = " + stored + "\n");
        LauncherSettings settings(ini);
        const int found = ResolveGpu(two, settings.Get(JPT_KEY_GRAPHICS_CARD).c_str());
        CHECK(found >= 0);
        if (settings.Get(JPT_KEY_GRAPHICS_CARD) != two[size_t(found)].label) settings.Set(JPT_KEY_GRAPHICS_CARD, two[size_t(found)].label);
        CHECK(settings.Save());
        jpt_config_t game;
        CHECK(jpt_config_load(ini.string().c_str(), &game));
        CHECK(ResolveGpu(two, game.graphics_card) == found);
        return std::string(game.graphics_card);
    };
    CHECK(launcher_then_game("") == intel);
    CHECK(launcher_then_game(nvidia) == nvidia);
    CHECK(launcher_then_game(intel) == intel);
    CHECK(launcher_then_game("AMD Radeon RX 7800 XT") == intel);
    CHECK(ReadFile(ini).find("AMD") == std::string::npos);

    // A setting saved before duplicates were told apart keeps choosing the first match; the second is saved by its label.
    const auto saved_choice = [&](const std::string& stored) {
        WriteFile(ini, "graphics_card = " + stored + "\n");
        jpt_config_t game;
        CHECK(jpt_config_load(ini.string().c_str(), &game));
        return ResolveGpu(twins, game.graphics_card);
    };
    CHECK(saved_choice(nvidia) == 0 && saved_choice(second) == 1 && saved_choice("gone") == 0);

    // This PC (DXGI only, no window): entry i of the list is the adapter a device created on index i runs on.
    const std::vector<jpt_gpu_t> real = ListGpus();
    for (size_t i = 0; i < real.size(); ++i) {
        if (!real[i].name[0]) continue;
        std::printf("graphics card %zu: %s\n", i, real[i].label);
        CHECK(DeviceAdapterName(static_cast<int>(i)) == real[i].name);
        CHECK(ResolveGpu(real, real[i].label) == static_cast<int>(i));
    }
    if (GpuChoices(real).empty()) std::printf("graphics cards: none usable on this PC\n");

    // The page offers the real cards next to the renderer, and no Render size row.
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const size_t renderer = launcher.find("ui.Combo(\"Renderer\"");
    const size_t card = launcher.find("ui.Row(\"Graphics card\", kGraphicsCardHelp)");
    const size_t resolution = launcher.find("ui.Combo(\"Render resolution\"");
    CHECK(renderer != std::string::npos && renderer < card && card < resolution && resolution != std::string::npos);
    CHECK(launcher.find("ui.Info(\"Graphics card\"") == std::string::npos);
    CHECK(launcher.find("BeginCombo(\"##graphics_card\"") != std::string::npos);
    CHECK(launcher.find("\"No graphics card found\"") != std::string::npos);
    CHECK(launcher.find("NormalizeGraphicsCard();") != std::string::npos);
    for (const char* gone : {"Render size", "RenderSizeText", "Not detected", "QueryGpu", "\"Automatic\", \"Auto\""})
        CHECK(launcher.find(gone) == std::string::npos && ReadSource("launcher/options.cpp").find(gone) == std::string::npos);
    CHECK(!HasRow("Render size") && !HasRow("Not detected") && HasRow("No graphics card found") && HasRow("Graphics card"));
    SetLauncherLanguage("pt-BR");
    CHECK(std::string(Tr("Selects which graphics card renders the game. If the selected card is no longer available, the "
                         "first available card is used.")) ==
          "Seleciona qual placa de vídeo renderiza o jogo. Se a placa selecionada não estiver mais disponível, a primeira "
          "placa disponível será usada.");
    SetLauncherLanguage("en");

    // The renderer creates its device on the adapter the saved name resolves to.
    const std::string main_c = ReadSource("src/host/main.c");
    CHECK(main_c.find("present_set_graphics_card(cfg.graphics_card);") != std::string::npos);
    const std::string present = ReadSource("src/host/ddraw/d3d11_present.c");
    const size_t resolved = present.find("index = jpt_gpu_resolve(gpus, count, s_graphics_card);");
    const size_t opened = present.find("IDXGIAdapter* chosen = open_adapter(chosen_adapter_index());");
    const size_t created = present.find("hr = create_on(chosen, D3D_DRIVER_TYPE_UNKNOWN, &level);");
    const size_t fallback = present.find("hr = create_on(NULL, D3D_DRIVER_TYPE_HARDWARE, &level);");
    const size_t warp = present.find("hr = create_on(NULL, D3D_DRIVER_TYPE_WARP, &level);");
    CHECK(resolved != std::string::npos && opened != std::string::npos && opened < created && created < fallback &&
          fallback < warp && warp != std::string::npos);
    CHECK(present.find("D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE") == std::string::npos);
    CHECK(present.find("EnumAdapters1(factory, (UINT)index, &adapter)") != std::string::npos);
    CHECK(present.find("saved graphics card \\\"%s\\\" not available; using %s") != std::string::npos);
}

// Invert mouse is an input setting: the Controls page, next to the keyboard and mouse bindings, as the game's own value.
static void TestInvertMouseInControls() {
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const size_t gameplay = launcher.find("void PageGameplay(");
    const size_t controls = launcher.find("// ---- Controls");
    const size_t page = launcher.find("void PageControls(");
    const size_t keyboard = launcher.find("ui.Section(\"Keyboard & Mouse\"");
    const size_t row = launcher.find("InvertMouseRow(ui);");
    const size_t bindings = launcher.find("GuideRows(ui, 0, family, pad);");
    CHECK(gameplay < controls && controls < page && page < keyboard && keyboard < row && row < bindings);
    CHECK(launcher.substr(gameplay, controls - gameplay).find("nvert") == std::string::npos);
    size_t rows = 0;
    for (size_t at = launcher.find("\"Invert mouse\""); at != std::string::npos; at = launcher.find("\"Invert mouse\"", at + 1))
        ++rows;
    CHECK(rows == 1);
    CHECK(launcher.find("registry_.GetDword(\"InvertMouse\", value)") != std::string::npos &&
          launcher.find("registry_.SetDword(\"InvertMouse\", invert_ ? 1 : 0)") != std::string::npos);
    for (int i = 0; i < JPT_SETTING_COUNT; ++i) CHECK(std::string(jpt_settings[i].key).find("invert_mouse") == std::string::npos);
}

// Field of view (Graphics) and Mouse sensitivity (Controls): defaults, what the launcher saves, Reset, Portuguese.
static void TestFovAndMouseSensitivity(const fs::path& dir) {
    const fs::path ini = dir / "fov_mouse.ini";
    WriteFile(ini, "settings_version = 3\nrenderer = d3d11\n");
    LauncherSettings s(ini);
    CHECK(s.Get(JPT_KEY_FOV) == "75" && s.Get(JPT_KEY_MOUSE_SENSITIVITY) == "1.00");
    s.SetInt(JPT_KEY_FOV, 95);
    s.Set(JPT_KEY_MOUSE_SENSITIVITY, "1.35");
    CHECK(s.Save());
    jpt_config_t c;
    CHECK(jpt_config_load(ini.string().c_str(), &c) && c.fov == 95 && c.mouse_sensitivity > 1.349f &&
          c.mouse_sensitivity < 1.351f && c.hardware);
    LauncherSettings again(ini);
    CHECK(again.Get(JPT_KEY_FOV) == "95" && again.Get(JPT_KEY_MOUSE_SENSITIVITY) == "1.35");
    again.ResetToDefaults();
    CHECK(again.Save() && again.Get(JPT_KEY_FOV) == "75" && again.Get(JPT_KEY_MOUSE_SENSITIVITY) == "1.00");
    CHECK(jpt_config_load(ini.string().c_str(), &c) && c.fov == 75 && c.mouse_sensitivity == 1.0f);
    WriteFile(ini, "fov = 300\nmouse_sensitivity = 0.01\n");
    CHECK(jpt_config_load(ini.string().c_str(), &c) && c.fov == JPT_FOV_MAX && c.mouse_sensitivity == JPT_MOUSE_SENSITIVITY_MIN);
    WriteFile(ini, "fov = wide\nmouse_sensitivity = fast\n");
    LauncherSettings broken(ini);
    CHECK(broken.Get(JPT_KEY_FOV) == "75" && broken.Get(JPT_KEY_MOUSE_SENSITIVITY) == "1.00");

    const std::string launcher = ReadSource("launcher/launcher.cpp");
    CHECK(launcher.find("SliderWithDefault(\"fov\", fov, JPT_FOV_MIN, JPT_FOV_MAX, JPT_FOV_DEFAULT") != std::string::npos);
    char default_text[16];
    jpt_mouse_sensitivity_text(JPT_MOUSE_SENSITIVITY_DEFAULT, default_text, sizeof default_text);
    CHECK(std::string(default_text) == "1.00" && std::string(jpt_default_value(JPT_KEY_MOUSE_SENSITIVITY)) == default_text);
    CHECK(JPT_MOUSE_SENSITIVITY_MIN == 0.25f && JPT_MOUSE_SENSITIVITY_MAX == 4.0f && JPT_MOUSE_SENSITIVITY_STEP == 0.05f);
    const size_t mouse_row = launcher.find("void MouseSensitivityRow("), controls_page = launcher.find("void PageControls(");
    CHECK(mouse_row != std::string::npos && mouse_row < controls_page);
    const std::string mouse_body = launcher.substr(mouse_row, controls_page - mouse_row);
    CHECK(mouse_body.find("SliderWithDefault(\"mouse_sensitivity\", sensitivity, JPT_MOUSE_SENSITIVITY_MIN, "
                          "JPT_MOUSE_SENSITIVITY_MAX,") != std::string::npos &&
          mouse_body.find("JPT_MOUSE_SENSITIVITY_DEFAULT") != std::string::npos &&
          mouse_body.find("SetNextItemWidth(-FLT_MIN)") == std::string::npos);
    CHECK(launcher.find("Reset all settings on the About page") == std::string::npos &&
          launcher.find("Controls how fast the mouse turns the view and moves Anne's hand on both axes. 1.00 is the original speed.") !=
              std::string::npos);
    SetLauncherLanguage("pt-BR");
    CHECK(std::string(Tr("Controls how fast the mouse turns the view and moves Anne's hand on both axes. 1.00 is the original speed.")) ==
          "Controla a velocidade do mouse ao girar a visão e mover a mão de Anne nos dois eixos. 1,00 é a velocidade original.");
    SetLauncherLanguage("en");
    CHECK(launcher.find("\"mouse_sensitivity_x\"") == std::string::npos && launcher.find("MouseSensY\"") == std::string::npos);
    SetLauncherLanguage("pt-BR");
    CHECK(std::string(Tr("Field of view")) == "Campo de visão (FOV)" && std::string(Tr("Mouse sensitivity")) == "Sensibilidade do mouse" &&
          std::string(Tr("Default")) == "Padrão");
    SetLauncherLanguage("en");
    CHECK(std::string(Tr("Default")) == "Default");
}

// The translations are part of the program: no installation, download or folder of the player's.
static void TestEmbeddedOnly() {
    for (const char* file : {"src/host/l10n.c", "src/common/jpt_l10n.c", "src/common/jpt_l10n.h", "launcher/launcher.cpp",
                             "src/host/main.c"}) {
        const std::string source = ReadSource(file);
        for (const char* forbidden : {"user_data\\\\translations", "\"translations\"", "JPT_L10N_DIR", "WinHttp", "InternetOpen",
                                      "URLDownload", "Decomp_Downloads", "TresCom page", "staging\\\\translations"})
            CHECK(source.find(forbidden) == std::string::npos);
    }
    const fs::path assets = fs::path(JPT_SOURCE_DIR) / "assets" / "translations";
    for (const char* name : {"pt-BR.strings.txt", "fr.strings.txt", "de.strings.txt", "es.strings.txt", "ru.strings.txt",
                             "ru.subtitles.txt", "it.subtitles.txt"})
        CHECK(fs::is_regular_file(assets / name));
    CHECK(ReadSource("CMakeLists.txt").find("assets/translations/*.txt") != std::string::npos);
}

static void TestDirect3DEffectsPolicy() {
    CHECK(Direct3DEffectsAvailable("d3d11") && Direct3DEffectsAvailable("") && !Direct3DEffectsAvailable("software"));
    SetLauncherLanguage("pt-BR");
    CHECK(std::string(Tr(kDirect3DOnlyHelp)) == "Disponível com o renderizador Direct3D 11.");
    SetLauncherLanguage("en");
    CHECK(std::string(Tr(kDirect3DOnlyHelp)) == "Available with the Direct3D 11 renderer.");
    // Greyed out (not hidden, not rewritten) under Software, both effects, with the D3D11-only help.
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const size_t gate = launcher.find("Direct3DEffectsAvailable(settings_.Get(JPT_KEY_RENDERER))");
    const size_t disabled = launcher.find("ImGui::BeginDisabled(!available);", gate);
    const size_t water = launcher.find("ui.Toggle(\"Water refraction\", available ? kWaterRefractionHelp : kDirect3DOnlyHelp", disabled);
    const size_t sky = launcher.find("ui.Toggle(\"Enhanced sky\", available ? kEnhancedSkyHelp : kDirect3DOnlyHelp", water);
    const size_t end = launcher.find("ImGui::EndDisabled();", sky);
    CHECK(gate != std::string::npos && disabled != std::string::npos && water != std::string::npos && sky != std::string::npos &&
          end != std::string::npos);
    CHECK(launcher.find("Set(JPT_KEY_WATER_REFRACTION") == std::string::npos &&
          launcher.find("Set(JPT_KEY_ENHANCED_SKY") == std::string::npos);
}

static void TestFogAndShadowsRows() {
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const size_t page = launcher.find("void PageGraphics(");
    const size_t fog = launcher.find("ui.Toggle(\"Fog\", kFogHelp, JPT_KEY_FOG_ENABLED, true, \"Off\", \"On\")", page);
    const size_t shadows =
        launcher.find("ui.Toggle(\"Shadows\", kShadowsHelp, JPT_KEY_SHADOWS_ENABLED, true, \"Off\", \"On\")", fog);
    const size_t ao = launcher.find("JPT_KEY_AMBIENT_OCCLUSION", page);
    const size_t gate = launcher.find("Direct3DEffectsAvailable(settings_.Get(JPT_KEY_RENDERER))", page);
    CHECK(page != std::string::npos && ao < fog && fog < shadows && shadows < gate && gate != std::string::npos);
    CHECK(HasRow("Fog") && HasRow("Shadows"));
    CHECK(HasRow("The original game's distance fog over objects, terrain and water, in every renderer. Off draws them without "
                 "it; the sky keeps its own haze, and the draw distance does not change."));
    CHECK(HasRow("The original game's shadows of objects and dinosaurs on the ground, in every renderer. Off draws the ground "
                 "without them. Ambient occlusion is a separate setting."));
}

static std::vector<int> CheatValues(const fs::path& ini) {
    jpt_config_t c;
    jpt_config_load(ini.string().c_str(), &c);
    return std::vector<int>(c.cheat, c.cheat + JPT_CHEAT_COUNT);
}

static void TestCheats(const fs::path& dir) {
    // the placeholder is gone
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    CHECK(launcher.find("PageComingLater") == std::string::npos &&
          launcher.find("PageCheats(ui)") != std::string::npos);
    CHECK(std::string(kCheatsHelp).find("F10") != std::string::npos &&
          std::string(kCheatsHelp).find("only to the current session") != std::string::npos);

    // the page shows the saved cheats of the one list, in F10's order
    const std::vector<const jpt_cheat_info_t*> rows = LauncherCheats();
    const char* labels[] = {"Movement Speed", "Jump Height", "Invulnerability", "Infinite Ammo", "Freeze Dinosaurs", "Super Health"};
    const char* keys[] = {"cheat_movement_speed", "cheat_jump_height", "cheat_invulnerability", "cheat_infinite_ammo",
                          "cheat_freeze_dinosaurs", "cheat_super_health"};
    CHECK(rows.size() == std::size(labels) && rows.size() == JPT_CHEAT_PERSISTENT_COUNT);
    for (size_t i = 0; i < rows.size() && i < std::size(labels); ++i) {
        CHECK(std::string(rows[i]->label) == labels[i] && std::string(rows[i]->key) == keys[i]);
        CHECK(rows[i]->kind == (i < 2 ? JPT_CHEAT_CHOICE : JPT_CHEAT_TOGGLE));
        CHECK(std::string(rows[i]->help).size() > 10);
    }
    for (const jpt_cheat_info_t* row : rows) {
        const std::string label = row->label;
        CHECK(label != "Heal" && label != "Teleport" && label != "Level Select" && label != "Fly / Free Camera");
        CHECK(label.find("Coordinates") == std::string::npos && label.find("Third Person") == std::string::npos);
    }
    CHECK(launcher.find("LauncherCheats()") != std::string::npos);
    for (const char* label : labels) {
        CHECK(launcher.find(std::string("\"") + label + "\"") == std::string::npos);
        CHECK(ReadSource("launcher/options.cpp").find(std::string("\"") + label + "\"") == std::string::npos);
    }
    CHECK(launcher.find("Coordinates") == std::string::npos && launcher.find("Third Person") == std::string::npos);

    // the choices are the F10 lists
    const jpt_cheat_info_t& speed = *rows[0];
    const jpt_cheat_info_t& jump = *rows[1];
    const char* speed_labels[] = {"0.5x", "1.0x", "1.5x", "2.0x", "3.0x", "5.0x", "7.5x", "10.0x"};
    const char* jump_labels[] = {"0.5x", "1.0x", "1.5x", "2.0x", "3.0x", "5.0x"};
    CHECK(speed.choice_count == int(std::size(speed_labels)) && jump.choice_count == int(std::size(jump_labels)));
    for (int i = 0; i < speed.choice_count && i < int(std::size(speed_labels)); ++i)
        CHECK(std::string(speed.choices[i].label) == speed_labels[i]);
    for (int i = 0; i < jump.choice_count && i < int(std::size(jump_labels)); ++i)
        CHECK(std::string(jump.choices[i].label) == jump_labels[i]);
    CHECK(std::string(speed.choices[speed.default_value].label) == "1.0x" &&
          std::string(jump.choices[jump.default_value].label) == "1.0x");

    // defaults: nothing is on, speed and jump are the original
    const fs::path fresh = dir / "cheats_fresh.ini";
    WriteFile(fresh, "settings_version = 3\nrenderer = d3d11\n");
    LauncherSettings defaults(fresh);
    for (const jpt_cheat_info_t* row : rows)
        CHECK(defaults.Get(row->key) == jpt_cheat_saved_text(row, row->default_value));
    CHECK(defaults.Get("cheat_movement_speed") == "1" && defaults.Get("cheat_jump_height") == "1");
    for (const char* key : {"cheat_invulnerability", "cheat_infinite_ammo", "cheat_freeze_dinosaurs", "cheat_super_health"})
        CHECK(defaults.Get(key) == "0");
    const std::vector<int> old_config = CheatValues(fresh);
    for (int i = 0; i < JPT_CHEAT_COUNT; ++i) CHECK(old_config[size_t(i)] == jpt_cheats[i].default_value);
    jpt_config_t config;
    jpt_config_defaults(&config);
    CHECK(config.cheat[JPT_CHEAT_SPEED] == 1 && config.cheat[JPT_CHEAT_JUMP] == 1 && !config.cheat[JPT_CHEAT_INVULNERABLE] &&
          !config.cheat[JPT_CHEAT_AMMO] && !config.cheat[JPT_CHEAT_FREEZE_AI] && !config.cheat[JPT_CHEAT_SUPER_HEALTH]);

    // Save keeps the values without starting; Play saves the same way
    const fs::path saved = dir / "cheats_saved.ini";
    WriteFile(saved, "settings_version = 3\nrenderer = d3d11\n");
    LauncherSettings s(saved);
    s.Set("cheat_movement_speed", "5");
    s.Set("cheat_jump_height", "2");
    s.SetBool("cheat_invulnerability", true);
    s.SetBool("cheat_super_health", true);
    CHECK(s.Dirty() && ReadFile(saved).find("cheat_") == std::string::npos);
    CHECK(s.Save() && !s.Dirty());
    const std::string text = ReadFile(saved);
    CHECK(text.find("cheat_movement_speed = 5") != std::string::npos && text.find("cheat_invulnerability = 1") != std::string::npos);
    CHECK(text.find("renderer = d3d11") != std::string::npos);
    LauncherSettings reopened(saved);
    CHECK(reopened.Get("cheat_movement_speed") == "5" && reopened.Get("cheat_jump_height") == "2" &&
          reopened.GetBool("cheat_invulnerability", false) && !reopened.GetBool("cheat_infinite_ammo", true) &&
          reopened.GetBool("cheat_super_health", false) && !reopened.GetBool("cheat_freeze_dinosaurs", true));
    std::vector<int> runtime = CheatValues(saved);
    CHECK(runtime[JPT_CHEAT_SPEED] == 5 && runtime[JPT_CHEAT_JUMP] == 3 && runtime[JPT_CHEAT_INVULNERABLE] == 1 &&
          runtime[JPT_CHEAT_AMMO] == 0 && runtime[JPT_CHEAT_FREEZE_AI] == 0 && runtime[JPT_CHEAT_SUPER_HEALTH] == 1);

    const fs::path played = dir / "cheats_played.ini";
    WriteFile(played, "settings_version = 3\n");
    LauncherSettings play_settings(played);
    play_settings.SetBool("cheat_infinite_ammo", true);
    play_settings.SetBool("cheat_freeze_dinosaurs", true);
    play_settings.Set("cheat_movement_speed", "10");
    PlayGate gate;
    CHECK(gate.Request(false, true, [&] { return play_settings.Save(); }) == PlayRequest::kStarting);
    runtime = CheatValues(played);
    CHECK(runtime[JPT_CHEAT_AMMO] == 1 && runtime[JPT_CHEAT_FREEZE_AI] == 1 && runtime[JPT_CHEAT_SPEED] == 7 &&
          runtime[JPT_CHEAT_INVULNERABLE] == 0);

    // Reset all settings puts every cheat back, in the file too
    LauncherSettings reset(saved);
    reset.ResetToDefaults();
    CHECK(reset.Save());
    const std::string after = ReadFile(saved);
    CHECK(after.find("cheat_movement_speed = 1") != std::string::npos && after.find("cheat_jump_height = 1") != std::string::npos &&
          after.find("cheat_invulnerability = 0") != std::string::npos && after.find("cheat_super_health = 0") != std::string::npos);
    runtime = CheatValues(saved);
    for (int i = 0; i < JPT_CHEAT_COUNT; ++i) CHECK(runtime[size_t(i)] == jpt_cheats[i].default_value);

    // a hand-edited file never crashes and never turns a cheat on
    const fs::path bad = dir / "cheats_bad.ini";
    WriteFile(bad, "cheat_invulnerability = banana\ncheat_movement_speed = 4\ncheat_jump_height = fast\ncheat_infinite_ammo = 2\n"
                   "cheat_super_health =\ncheat_freeze_dinosaurs = nan\n");
    runtime = CheatValues(bad);
    for (int i = 0; i < JPT_CHEAT_COUNT; ++i) CHECK(runtime[size_t(i)] == jpt_cheats[i].default_value);
    LauncherSettings broken(bad);
    CHECK(broken.Get("cheat_invulnerability") == "0" && broken.Get("cheat_movement_speed") == "1" &&
          broken.Get("cheat_jump_height") == "1" && broken.Get("cheat_infinite_ammo") == "0" &&
          broken.Get("cheat_super_health") == "0" && broken.Get("cheat_freeze_dinosaurs") == "0");
    const fs::path spelled = dir / "cheats_spelled.ini";
    WriteFile(spelled, "cheat_movement_speed = 2.0\ncheat_jump_height = 0.50\ncheat_invulnerability = on\n");
    runtime = CheatValues(spelled);
    CHECK(runtime[JPT_CHEAT_SPEED] == 3 && runtime[JPT_CHEAT_JUMP] == 0 && runtime[JPT_CHEAT_INVULNERABLE] == 1);
    CHECK(CanonicalCheatValue(speed, "2.0") == "2" && CanonicalCheatValue(speed, "garbage") == "1" &&
          CanonicalCheatValue(jump, "0.50") == "0.5" && CanonicalCheatValue(*rows[2], "yes") == "1" &&
          CanonicalCheatValue(*rows[2], "?") == "0");

    // actions and the removed options are not settings
    for (const char* key : {"cheat_heal", "cheat_teleport", "cheat_level_select", "cheat_free_camera", "cheat_fly_free_camera",
                            "cheat_show_coordinates", "cheat_third_person"}) {
        CHECK(jpt_default_value(key) == nullptr && !jpt_config_valid(key, "1"));
        CHECK(text.find(key) == std::string::npos);
    }
    int cheat_keys = 0;
    for (int i = 0; i < JPT_SETTING_COUNT; ++i) cheat_keys += std::strncmp(jpt_settings[i].key, "cheat_", 6) == 0;
    CHECK(cheat_keys == JPT_CHEAT_PERSISTENT_COUNT);

    // Portuguese: the page, the rows and their help
    SetLauncherLanguage("pt-BR");
    CHECK(std::string(Tr("Cheats")) == "Trapaças");
    const char* pt[] = {"Velocidade de movimento", "Altura do pulo", "Invulnerabilidade", "Munição infinita",
                        "Congelar dinossauros", "Supervida"};
    for (size_t i = 0; i < rows.size() && i < std::size(pt); ++i) {
        CHECK(std::string(Tr(rows[i]->label)) == pt[i]);
        CHECK(HasRow(rows[i]->help) && std::string(Tr(rows[i]->help)) != rows[i]->help);
    }
    CHECK(HasRow(kCheatsHelp) && HasRow(kCheatsPageSubtitle));
    CHECK(std::string(Tr("Off")) != "" && std::string(Tr(kCheatsPageSubtitle)) != kCheatsPageSubtitle);
    SetLauncherLanguage("en");
    CHECK(std::string(Tr(rows[0]->label)) == "Movement Speed" && std::string(Tr(rows[0]->help)) == rows[0]->help);
}

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const fs::path dir = fs::path(temp) / ("jpt_launcher_tests_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    TestRenderPresets();
    TestRenderTargets();
    TestHorPlus();
    TestPresentation();
    TestWindowAndMonitor();
    TestMsaa();
    TestSettings(dir);
    TestAspectRatioChoice(dir);
    TestPlayGate();
    TestInflate();
    TestCabinet();
    TestIsoReader(dir);
    TestIdentity(dir);
    TestInstaller(dir);
    TestStringTable(dir);
    TestImportShapes(dir);
    TestVersions(dir);
    TestGameVersionTable();
    TestHostResolution(dir);
    TestUserFiles(dir);
    TestMigration(dir);
    TestPages();
    TestAssets();
    TestUiTable();
    TestLauncherLanguage(dir);
    TestUiCoverage();
    TestNoLanguagePage();
    TestDirect3DEffectsPolicy();
    TestFogAndShadowsRows();
    TestLanguageNames();
    TestGoreAndTitleOptions();
    TestGameplayOrder();
    TestGraphicsCard(dir);
    TestInvertMouseInControls();
    TestFovAndMouseSensitivity(dir);
    TestCheats(dir);
    TestEmbeddedOnly();
    TestRealDiscs();
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("launcher_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

// Headless checks of the launcher and display logic: settings, render sizes, Hor+, presentation, monitors, MSAA,
// the ISO reader, disc identity, the installer's staging, rollback and Cancel, and the Play gate. Disc images are
// synthetic and built in a temporary folder; TRESPASSER_TEST_ISO=<image> also identifies a real disc (read only).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "game_files.h"
#include "game_install.h"
#include "iso9660.h"
#include "jpt_config.h"
#include "jpt_display.h"
#include "jpt_user_data.h"
#include "options.h"
#include "page_icons.h"
#include "play_gate.h"
#include "settings.h"

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

// "abc" hashes to the FIPS 180-2 test vector.
constexpr char kAbcSha256[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

constexpr DiscFile kTestDiscFiles[] = {
    {"setup/tpassp6.exe", "trespass.exe"}, {"setup/msvcirt.dll", "MSVCIRT.DLL"}, {"setup/smackw32.dll", "SMACKW32.DLL"},
    {"setup/processor.dll", "Processor.dll"}, {"readme.txt", "Readme.txt"},
};
constexpr ManifestEntry kTestManifest[] = {
    {"trespass.exe", 3}, {"MSVCIRT.DLL", 1}, {"SMACKW32.DLL", 1}, {"Processor.dll", 1}, {"Readme.txt", 1},
    {"data/be.scn", 7}, {"data/menu/tpassintro.smk", 5},
};
const GameProfile kTestProfile{"test", "Test Build", "TEST_DISC", "trespass.exe", kAbcSha256, 3, kTestDiscFiles,
                               "data/", kTestManifest};
const GameProfile kWrongHashProfile{"wrong", "Wrong Hash", "TEST_DISC", "trespass.exe",
                                    "0000000000000000000000000000000000000000000000000000000000000000", 3,
                                    kTestDiscFiles, "data/", kTestManifest};

static std::vector<IsoInput> TestDisc(bool joliet) {
    auto n = [joliet](const char* lower, const char* upper) { return std::string(joliet ? lower : upper); };
    return {{n("setup/tpassp6.exe", "SETUP/TPASSP6.EXE"), "abc"}, {n("setup/msvcirt.dll", "SETUP/MSVCIRT.DLL"), "m"},
            {n("setup/smackw32.dll", "SETUP/SMACKW32.DLL"), "s"}, {n("setup/processor.dll", "SETUP/PROCESSOR.DLL"), "p"},
            {n("readme.txt", "README.TXT"), "r"},                  {n("data/be.scn", "DATA/BE.SCN"), "levelbe"},
            {n("data/menu/tpassintro.smk", "DATA/MENU/TPASSINTRO.SMK"), "intro"},
            {n("setup/directx/dsetup.dll", "SETUP/DIRECTX/DSETUP.DLL"), "not installed"}};
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
    CHECK(c.vsync && c.show_fps);
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
        CHECK(PageComingLater(kPageOrder[i]) == (name == "Cheats" || name == "Bug Report"));
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
    const std::string size_text =
        RenderSizeText(jpt_render_box(jpt_render_preset_index("1440p"), JPT_ASPECT_16_9, 0, 0), JPT_ASPECT_16_9);
    CHECK(size_text.find("2560 \xC3\x97 1440") != std::string::npos);
    CHECK(size_text.find("1024") == std::string::npos);
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
        CHECK(exe && iso.Extract(*exe, out, [&](uint64_t n) { return seen += n, true; }, error));
        CHECK(ReadFile(out) == "abc" && seen == 3 && !fs::exists(dir / "out.bin.part"));
        CHECK(Sha256File(out) == kAbcSha256);
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
    CHECK(exe && !truncated.Extract(*exe, dir / "cut.bin", nullptr, error) && !fs::exists(dir / "cut.bin"));
}

static void TestIdentity(const fs::path& dir) {
    const GameProfile profiles[] = {kTestProfile};
    std::string why;
    IsoImage good, other, version;
    WriteImage(dir / "good.iso", IsoBuilder().Build("TEST_DISC", TestDisc(true), true));
    CHECK(good.Open(dir / "good.iso", why));
    CHECK(IdentifyDisc(good, profiles, why) == &profiles[0]);
    std::string plan_error;
    const std::vector<PlannedFile> plan = PlanInstall(good, profiles[0], plan_error);
    CHECK(plan.size() == 7);  // five runtime files and data/, not setup/directx
    WriteImage(dir / "other.iso", IsoBuilder().Build("SOME_GAME", {{"game.exe", "x"}}, true));
    CHECK(other.Open(dir / "other.iso", why));
    CHECK(!IdentifyDisc(other, profiles, why) && why.find("not a Trespasser") != std::string::npos);
    std::vector<IsoInput> files = TestDisc(true);
    files[0].content = "abcd";
    WriteImage(dir / "version.iso", IsoBuilder().Build("TEST_DISC", files, true));
    CHECK(version.Open(dir / "version.iso", why));
    CHECK(!IdentifyDisc(version, profiles, why) && why.find("4-byte") != std::string::npos);
}

static fs::path MakeInstalled(const fs::path& game) {
    fs::remove_all(game);
    WriteFile(game / "trespass.exe", "old");
    WriteFile(game / "SAVEGAME.000", "my save");
    WriteFile(game / "data" / "old.scn", "old level");
    return game;
}

static void TestInstaller(const fs::path& dir) {
    const fs::path iso = dir / "install.iso";
    WriteImage(iso, IsoBuilder().Build("TEST_DISC", TestDisc(true), true));
    const GameProfile profiles[] = {kTestProfile};
    const fs::path game = dir / "game", user = dir / "user";
    InstallHooks hooks;
    int phases = 0;
    hooks.phase = [&](const char*) { ++phases; };

    // Fresh install.
    InstallResult r = InstallFromIso(iso, game, user, hooks, profiles);
    CHECK(r.ok() && r.files == 7 && r.profile == &profiles[0] && phases >= 3);
    CHECK(CheckGameFolder(game, profiles).state == GameState::kReady);
    CHECK(!fs::exists(StagingFolder(game)) && !fs::exists(BackupFolder(game)));
    CHECK(ReadFile(game / "trespass.exe") == "abc" && !fs::exists(game / "setup"));

    // Reinstall over an older one: the old files go, the save is copied to the user folder first.
    MakeInstalled(game);
    r = InstallFromIso(iso, game, user, hooks, profiles);
    CHECK(r.ok() && ReadFile(user / "SAVEGAME.000") == "my save" && !fs::exists(game / "data" / "old.scn"));
    CHECK(CheckGameFolder(game, profiles).state == GameState::kReady);

    // An executable no profile knows: refused after the copy, the current installation untouched.
    MakeInstalled(game);
    const GameProfile wrong[] = {kWrongHashProfile};
    r = InstallFromIso(iso, game, user, hooks, wrong);
    CHECK(r.status == InstallStatus::kValidationFailed && ReadFile(game / "trespass.exe") == "old");
    CHECK(!fs::exists(StagingFolder(game)));

    // Another disc: refused before anything is copied.
    WriteImage(dir / "wrong.iso", IsoBuilder().Build("SOME_GAME", {{"game.exe", "x"}}, true));
    r = InstallFromIso(dir / "wrong.iso", game, user, hooks, profiles);
    CHECK(r.status == InstallStatus::kWrongDisc && ReadFile(game / "trespass.exe") == "old");
    WriteFile(dir / "notiso.iso", "hello");
    r = InstallFromIso(dir / "notiso.iso", game, user, hooks, profiles);
    CHECK(r.status == InstallStatus::kNotAnImage);

    // Cancel during the copy.
    InstallHooks cancel = hooks;
    cancel.progress = [](uint64_t done, uint64_t) { return done < 4; };
    r = InstallFromIso(iso, game, user, cancel, profiles);
    CHECK(r.status == InstallStatus::kCancelled && ReadFile(game / "trespass.exe") == "old");
    CHECK(!fs::exists(StagingFolder(game)));

    // The game folder cannot be moved aside (a file in it is open): rollback.
    HANDLE held = CreateFileW((game / "trespass.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    r = InstallFromIso(iso, game, user, hooks, profiles);
    CloseHandle(held);
    CHECK(r.status == InstallStatus::kActivateFailed && ReadFile(game / "trespass.exe") == "old");
    CHECK(fs::exists(game / "data" / "old.scn") && !fs::exists(StagingFolder(game)) && !fs::exists(BackupFolder(game)));

    // An image inside the game folder is refused (reinstalling would remove it).
    fs::copy_file(iso, game / "inside.iso");
    r = InstallFromIso(game / "inside.iso", game, user, hooks, profiles);
    CHECK(r.status == InstallStatus::kInsideGameFolder);

    // Game file states.
    CHECK(CheckGameFolder(dir / "nothing", profiles).state == GameState::kMissing);
    CHECK(CheckGameFolder(game, profiles).state == GameState::kInvalid);
    r = InstallFromIso(iso, game, user, hooks, profiles);
    CHECK(r.ok());
    fs::remove(game / "data" / "menu" / "tpassintro.smk");
    const GameCheck missing = CheckGameFolder(game, profiles);
    CHECK(missing.state == GameState::kInvalid && missing.detail.find("tpassintro.smk") != std::string::npos);
}

static void TestUserFiles(const fs::path& dir) {
    const fs::path old = dir / "oldgame", user = dir / "carry";
    WriteFile(old / "SAVEGAME.000", "a");
    WriteFile(old / "SAVEGAME.001", "b");
    WriteFile(old / JPT_REGISTRY_FILE, "Gore=dword:00000000\n");
    WriteFile(old / "trespass.exe", "x");
    WriteFile(user / "SAVEGAME.001", "newer");
    CHECK(jpt_carry_user_files(old.string().c_str(), user.string().c_str(), nullptr) == 2);
    CHECK(ReadFile(user / "SAVEGAME.000") == "a" && ReadFile(user / "SAVEGAME.001") == "newer");
    CHECK(fs::exists(user / JPT_REGISTRY_FILE) && !fs::exists(user / "trespass.exe") && fs::exists(old / "SAVEGAME.000"));
    CHECK(jpt_is_user_file("savegame.003") && jpt_is_user_file("recomp-registry.txt"));
    CHECK(!jpt_is_user_file("trespass.exe") && !jpt_is_user_file("be-130.swp"));
}

static void TestRealDisc() {
    char path[1024];
    if (!GetEnvironmentVariableA("TRESPASSER_TEST_ISO", path, sizeof path)) {
        std::printf("real disc: skipped (TRESPASSER_TEST_ISO not set)\n");
        return;
    }
    IsoImage iso;
    std::string why;
    CHECK(iso.Open(path, why));
    const GameProfile* profile = IdentifyDisc(iso, KnownProfiles(), why);
    CHECK(profile != nullptr);
    if (!profile) {
        std::printf("real disc: %s\n", why.c_str());
        return;
    }
    const std::vector<PlannedFile> plan = PlanInstall(iso, *profile, why);
    uint64_t bytes = 0;
    for (const PlannedFile& f : plan) bytes += f.source->size;
    CHECK(plan.size() == profile->manifest.size());
    bool sizes = true;
    for (const PlannedFile& f : plan) {
        bool found = false;
        for (const ManifestEntry& e : profile->manifest)
            found = found || (_stricmp(e.path, f.installed.c_str()) == 0 && e.size == f.source->size);
        sizes = sizes && found;
    }
    CHECK(sizes);
    std::printf("real disc: volume \"%s\", %s, %zu files to install, %llu bytes, profile %s\n", iso.volume_id().c_str(),
                iso.joliet() ? "Joliet" : "ISO names", plan.size(), static_cast<unsigned long long>(bytes),
                profile->label);
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
    TestPlayGate();
    TestIsoReader(dir);
    TestIdentity(dir);
    TestInstaller(dir);
    TestUserFiles(dir);
    TestMigration(dir);
    TestPages();
    TestAssets();
    TestRealDisc();
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("launcher_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

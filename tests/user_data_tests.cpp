// The portable user data root: selection, --user_data_root and its environment variable, an unwritable root (an error,
// never another location), and migration of the earlier flat user folder (rename, verified copy, failures). Scratch
// folders only.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "jpt_config.h"
#include "user_data.h"

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

static void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static fs::path Fresh(const fs::path& base, const char* name) {
    const fs::path dir = base / name;
    fs::remove_all(dir);
    fs::create_directories(dir / "port");
    return dir;
}

// The earlier layout: settings, the game's registry store, saves and save pictures side by side.
static fs::path MakeLegacy(const fs::path& dir) {
    const fs::path legacy = dir / "port" / "user";
    WriteFile(legacy / JPT_SETTINGS_FILE, "renderer = d3d11\n");
    WriteFile(legacy / "recomp-registry.txt", "Gore=dword:00000002\n");
    WriteFile(legacy / "SAVEGAME.003", "save three");
    WriteFile(legacy / "previews" / "SAVEGAME.003.jpg", "picture");
    return legacy;
}

static void CheckMigrated(const fs::path& root) {
    CHECK(ReadFile(root / "launcher" / JPT_SETTINGS_FILE) == "renderer = d3d11\n");
    CHECK(ReadFile(root / "saves" / "recomp-registry.txt") == "Gore=dword:00000002\n");
    CHECK(ReadFile(root / "saves" / "SAVEGAME.003") == "save three");
    CHECK(ReadFile(root / "saves" / "previews" / "SAVEGAME.003.jpg") == "picture");
    CHECK(!fs::exists(root / "saves" / JPT_SETTINGS_FILE));
}

static bool Inside(const fs::path& path, const fs::path& folder) {
    const fs::path relative = path.lexically_relative(folder);
    return !relative.empty() && *relative.begin() != "..";
}

// Nothing was created in the scratch folder besides the named children.
static bool Everything(const fs::path& dir, const std::set<std::string>& children) {
    std::set<std::string> found;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
        found.insert(entry.path().filename().string());
    }
    return found == children;
}

// The text without the scratch folder's own path (which is below the system's temporary folder).
static std::string Relative(std::string text, const fs::path& dir) {
    const std::string prefix = dir.string();
    for (size_t at; (at = text.find(prefix)) != std::string::npos;) {
        text.erase(at, prefix.size());
    }
    return text;
}

static bool NamesSystemFolder(const std::string& text) {
    for (const char* name : {"AppData", "LocalAppData", "Documents", "Temp"}) {
        if (text.find(name) != std::string::npos) return true;
    }
    return false;
}

static void TestPortableSelected(const fs::path& base) {
    const fs::path dir = Fresh(base, "portable");
    const UserDataSelection s = SelectUserData({dir / "port", {}, {dir / "port" / "user"}});
    CHECK(s.outcome == UserDataOutcome::kPortable);
    CHECK(s.root == dir / "port" / "user_data" && fs::is_directory(s.root));
    CHECK(!fs::exists(s.root / ".write_test"));
    CHECK(Inside(s.root, dir / "port") && Everything(dir, {"port"}));
    const UserDataMessage m = DescribeUserData(s);
    CHECK(!m.warning && m.text.find("User data: portable path selected") == 0);
    const UserDataPaths p = UserDataLayout(s.root);
    CHECK(p.settings_file == s.root / "launcher" / JPT_SETTINGS_FILE && p.saves == s.root / "saves");
    CHECK(p.cache == s.root / "cache");
}

static void TestExplicitRoot(const fs::path& base) {
    const fs::path dir = Fresh(base, "explicit");
    const UserDataSelection s = SelectUserData({dir / "port", dir / "dev", {dir / "port" / "user"}});
    CHECK(s.outcome == UserDataOutcome::kExplicit && s.root == dir / "dev" && fs::is_directory(dir / "dev"));
    CHECK(!fs::exists(dir / "port" / "user_data"));
    CHECK(DescribeUserData(s).text.find("--user_data_root") != std::string::npos);
    // A development root still takes over the earlier repository folder, once.
    MakeLegacy(dir);
    fs::remove_all(dir / "dev");
    const UserDataSelection moved = SelectUserData({dir / "port", dir / "dev", {dir / "port" / "user"}});
    CHECK(moved.outcome == UserDataOutcome::kMigrated && moved.root == dir / "dev");
    CheckMigrated(dir / "dev");
    CHECK(!fs::exists(dir / "port" / "user") && !fs::exists(dir / "port" / "user_data"));
}

// The port folder cannot be written: an explicit failure that names --user_data_root, and nothing is created anywhere.
static void TestPortableNotWritable(const fs::path& base) {
    const fs::path dir = Fresh(base, "readonly");
    const fs::path portable = dir / "port" / "user_data";
    UserDataFileOps ops;
    ops.can_write = [](const fs::path&) { return false; };
    const UserDataSelection s = SelectUserData({dir / "port", {}, {dir / "port" / "user"}}, ops);
    CHECK(s.outcome == UserDataOutcome::kUnwritable && s.root == portable && !s.explicit_root);
    CHECK(!fs::exists(portable) && Everything(dir, {"port"}) && fs::is_empty(dir / "port"));
    const UserDataMessage m = DescribeUserData(s);
    CHECK(m.warning && m.text.find("ERROR") != std::string::npos && m.text.find("not writable") != std::string::npos);
    CHECK(m.text.find("--user_data_root") != std::string::npos &&
          m.text.find("TRESPASSER_USER_DATA_ROOT") != std::string::npos);
    CHECK(!NamesSystemFolder(Relative(m.text, dir)));
    CHECK(std::string(UserDataFolderHelp(s.outcome)).find("--user_data_root") != std::string::npos);
    // Old data is not moved either: it stays exactly where it was.
    const fs::path legacy = MakeLegacy(dir);
    const UserDataSelection old = SelectUserData({dir / "port", {}, {legacy}}, ops);
    CHECK(old.outcome == UserDataOutcome::kUnwritable && old.root == portable && !fs::exists(portable));
    CHECK(ReadFile(legacy / "SAVEGAME.003") == "save three");
}

// The port folder cannot be written but --user_data_root can: that root is used and the port folder stays untouched.
static void TestExplicitWhenPortableNotWritable(const fs::path& base) {
    const fs::path dir = Fresh(base, "explicit_writable");
    const fs::path portable = dir / "port" / "user_data";
    UserDataFileOps ops;
    ops.can_write = [portable](const fs::path& folder) { return folder != portable; };
    const UserDataSelection s = SelectUserData({dir / "port", dir / "chosen", {}}, ops);
    CHECK(s.outcome == UserDataOutcome::kExplicit && s.root == dir / "chosen" && s.explicit_root);
    CHECK(!fs::exists(portable) && fs::is_empty(dir / "port"));
    // The same through the environment variable, which the command line beats.
    _putenv_s("TRESPASSER_USER_DATA_ROOT", (dir / "from_env").string().c_str());
    const fs::path from_env = ExplicitUserDataRoot(nullptr);
    CHECK(from_env == dir / "from_env" && ExplicitUserDataRoot("") == dir / "from_env");
    CHECK(ExplicitUserDataRoot((dir / "from_cli").string().c_str()) == dir / "from_cli");
    const UserDataSelection e = SelectUserData({dir / "port", from_env, {}}, ops);
    CHECK(e.outcome == UserDataOutcome::kExplicit && e.root == dir / "from_env" && e.explicit_root);
    CHECK(!fs::exists(portable) && fs::is_empty(dir / "port"));
    _putenv_s("TRESPASSER_USER_DATA_ROOT", "");
    CHECK(ExplicitUserDataRoot(nullptr).empty());
}

// An explicit root that cannot be written is an error too, not a reason to look elsewhere.
static void TestExplicitNotWritable(const fs::path& base) {
    const fs::path dir = Fresh(base, "explicit_readonly");
    UserDataFileOps ops;
    ops.can_write = [](const fs::path&) { return false; };
    const UserDataSelection s = SelectUserData({dir / "port", dir / "chosen", {}}, ops);
    CHECK(s.outcome == UserDataOutcome::kUnwritable && s.root == dir / "chosen" && s.explicit_root);
    CHECK(!fs::exists(dir / "port" / "user_data") && !fs::exists(dir / "chosen"));
    const UserDataMessage m = DescribeUserData(s);
    CHECK(m.warning && m.text.find("--user_data_root folder") != std::string::npos);
}

// The real write test: a file where the folder should be cannot become a user data root.
static void TestRealWriteTest(const fs::path& base) {
    const fs::path dir = Fresh(base, "real");
    WriteFile(dir / "port" / "user_data", "a file where the folder should be");
    const UserDataSelection s = SelectUserData({dir / "port", {}, {}});
    CHECK(s.outcome == UserDataOutcome::kUnwritable && s.root == dir / "port" / "user_data");
}

// The code that chooses and uses the root asks for no system folder at all.
static void TestNoSystemFolderInSource() {
    for (const char* file : {"launcher/user_data.cpp", "launcher/user_data.h", "launcher/launcher.cpp"}) {
        const std::string text = ReadFile(fs::path(JPT_SOURCE_DIR) / file);
        CHECK(!text.empty());
        for (const char* token : {"LocalAppData", "SHGetKnownFolderPath", "GetTempPath", "temp_directory_path", "APPDATA",
                                  "KnownFolder", "CSIDL", "SHGetFolderPath"}) {
            CHECK(text.find(token) == std::string::npos);
        }
    }
}

static void TestMigrationRename(const fs::path& base) {
    const fs::path dir = Fresh(base, "rename");
    MakeLegacy(dir);
    int renames = 0;
    UserDataFileOps ops;
    ops.rename = [&renames](const fs::path& from, const fs::path& to, std::error_code& ec) {
        ++renames;
        fs::rename(from, to, ec);
    };
    ops.copy_file = [](const fs::path&, const fs::path&, std::error_code& ec) {
        ec = std::make_error_code(std::errc::io_error);  // a move must not copy
    };
    const UserDataSelection s = SelectUserData({dir / "port", {}, {dir / "port" / "user"}}, ops);
    CHECK(s.outcome == UserDataOutcome::kMigrated && s.root == dir / "port" / "user_data" && renames >= 1);
    CheckMigrated(s.root);
    CHECK(!fs::exists(dir / "port" / "user") && !fs::exists(dir / "port" / "user_data.migrating"));
    CHECK(DescribeUserData(s).text.find("migrated legacy data") != std::string::npos);
}

// The rename across folders fails (as across drives): a verified copy becomes user_data.
static void TestMigrationCopy(const fs::path& base) {
    const fs::path dir = Fresh(base, "copy");
    const fs::path legacy = MakeLegacy(dir);
    UserDataFileOps ops;
    ops.rename = [&legacy](const fs::path& from, const fs::path& to, std::error_code& ec) {
        if (from == legacy) {
            ec = std::make_error_code(std::errc::cross_device_link);
            return;
        }
        fs::rename(from, to, ec);
    };
    const UserDataSelection s = SelectUserData({dir / "port", {}, {legacy}}, ops);
    CHECK(s.outcome == UserDataOutcome::kMigrated);
    CheckMigrated(s.root);
    CHECK(!fs::exists(legacy) && !fs::exists(dir / "port" / "user_data.migrating"));
}

// A copy that does not match is never activated; the earlier folder stays and is used.
static void TestMigrationVerify(const fs::path& base) {
    const fs::path dir = Fresh(base, "verify");
    const fs::path legacy = MakeLegacy(dir);
    UserDataFileOps ops;
    ops.rename = [&legacy](const fs::path& from, const fs::path& to, std::error_code& ec) {
        if (from == legacy) {
            ec = std::make_error_code(std::errc::cross_device_link);
            return;
        }
        fs::rename(from, to, ec);
    };
    ops.copy_file = [](const fs::path&, const fs::path& to, std::error_code& ec) {
        std::ofstream(to, std::ios::binary) << "x";  // short
        ec.clear();
    };
    const UserDataSelection s = SelectUserData({dir / "port", {}, {legacy}}, ops);
    CHECK(s.outcome == UserDataOutcome::kMigrationFailed && s.root == legacy && s.flat);
    CHECK(s.detail.find("does not match") != std::string::npos);
    CHECK(!fs::exists(dir / "port" / "user_data") && !fs::exists(dir / "port" / "user_data.migrating"));
    CHECK(ReadFile(legacy / "SAVEGAME.003") == "save three");
    const UserDataPaths p = UserDataLayout(s.root, s.flat);
    CHECK(p.settings_file == legacy / JPT_SETTINGS_FILE && p.saves == legacy);
    CHECK(DescribeUserData(s).warning);
}

static void TestMigrationFailure(const fs::path& base) {
    const fs::path dir = Fresh(base, "failure");
    const fs::path legacy = MakeLegacy(dir);
    UserDataFileOps ops;
    ops.rename = [&legacy](const fs::path& from, const fs::path& to, std::error_code& ec) {
        if (from == legacy) {
            ec = std::make_error_code(std::errc::permission_denied);
            return;
        }
        fs::rename(from, to, ec);
    };
    ops.copy_file = [](const fs::path&, const fs::path&, std::error_code& ec) {
        ec = std::make_error_code(std::errc::no_space_on_device);
    };
    const UserDataSelection s = SelectUserData({dir / "port", {}, {legacy}}, ops);
    CHECK(s.outcome == UserDataOutcome::kMigrationFailed && s.root == legacy);
    CHECK(!fs::exists(dir / "port" / "user_data") && !fs::exists(dir / "port" / "user_data.migrating"));
    CHECK(ReadFile(legacy / JPT_SETTINGS_FILE) == "renderer = d3d11\n");
    // The next start tries again and succeeds.
    const UserDataSelection again = SelectUserData({dir / "port", {}, {legacy}});
    CHECK(again.outcome == UserDataOutcome::kMigrated);
    CheckMigrated(again.root);
}

static void TestBothExist(const fs::path& base) {
    const fs::path dir = Fresh(base, "both");
    const fs::path legacy = MakeLegacy(dir);
    WriteFile(dir / "port" / "user_data" / "saves" / "SAVEGAME.000", "newer");
    const UserDataSelection s = SelectUserData({dir / "port", {}, {legacy}});
    CHECK(s.outcome == UserDataOutcome::kBothExist && s.root == dir / "port" / "user_data");
    CHECK(ReadFile(legacy / "SAVEGAME.003") == "save three" && !fs::exists(s.root / "saves" / "SAVEGAME.003"));
    CHECK(DescribeUserData(s).warning);
}

// Only empty folders (for instance created by a run that failed early) count as no data.
static void TestEmptyPortable(const fs::path& base) {
    const fs::path dir = Fresh(base, "empty");
    const fs::path legacy = MakeLegacy(dir);
    fs::create_directories(dir / "port" / "user_data" / "launcher");
    fs::create_directories(dir / "port" / "user_data" / "saves");
    const UserDataSelection s = SelectUserData({dir / "port", {}, {legacy}});
    CHECK(s.outcome == UserDataOutcome::kMigrated);
    CheckMigrated(s.root);
}

static void TestLegacyKept(const fs::path& base) {
    const fs::path dir = Fresh(base, "kept");
    const fs::path legacy = MakeLegacy(dir);
    UserDataFileOps ops;
    ops.rename = [&legacy](const fs::path& from, const fs::path& to, std::error_code& ec) {
        if (from == legacy) {
            ec = std::make_error_code(std::errc::cross_device_link);
            return;
        }
        fs::rename(from, to, ec);
    };
    ops.remove_all = [&legacy](const fs::path& path, std::error_code& ec) {
        if (path == legacy) {
            ec = std::make_error_code(std::errc::permission_denied);
            return;
        }
        fs::remove_all(path, ec);
    };
    const UserDataSelection s = SelectUserData({dir / "port", {}, {legacy}}, ops);
    CHECK(s.outcome == UserDataOutcome::kMigratedLegacyKept && DescribeUserData(s).warning);
    CheckMigrated(s.root);
}

// The second legacy candidate (the development repository's user folder) is used when the first has nothing.
static void TestSecondLegacy(const fs::path& base) {
    const fs::path dir = Fresh(base, "second");
    WriteFile(dir / "repo" / "user" / "SAVEGAME.001", "dev save");
    const UserDataSelection s = SelectUserData({dir / "port", {}, {dir / "port" / "user", dir / "repo" / "user"}});
    CHECK(s.outcome == UserDataOutcome::kMigrated && s.legacy == dir / "repo" / "user");
    CHECK(ReadFile(s.root / "saves" / "SAVEGAME.001") == "dev save");
}

// trespasser.ini left beside the saves (a move interrupted after its rename) is put in place at the next start.
static void TestSettingsPlaced(const fs::path& base) {
    const fs::path dir = Fresh(base, "placed");
    WriteFile(dir / "port" / "user_data" / "saves" / JPT_SETTINGS_FILE, "vsync = 0\n");
    const UserDataSelection s = SelectUserData({dir / "port", {}, {}});
    CHECK(s.outcome == UserDataOutcome::kPortable);
    CHECK(ReadFile(s.root / "launcher" / JPT_SETTINGS_FILE) == "vsync = 0\n");
    CHECK(!fs::exists(s.root / "saves" / JPT_SETTINGS_FILE));
}

// A trespasser.ini of the removed texture-pack option still loads: its line is ignored and the rest is read.
static void TestObsoleteSettingIgnored(const fs::path& base) {
    const fs::path ini = Fresh(base, "obsolete_setting") / "trespasser.ini";
    WriteFile(ini, "settings_version = 3\nhigh_resolution_textures = nnhd_full\nvsync = 0\nrenderer = software\n");
    jpt_config_t config;
    CHECK(jpt_config_load(ini.string().c_str(), &config) == 1);
    CHECK(config.vsync == 0 && config.hardware == 0);
    CHECK(!jpt_config_valid("high_resolution_textures", "nnhd_full") && !jpt_config_valid("high_resolution_textures", "off"));
    jpt_config_t defaults;
    jpt_config_defaults(&defaults);
    CHECK(!jpt_config_apply(&defaults, "high_resolution_textures", "nnhd_light") && defaults.msaa == config.msaa);
    for (int i = 0; i < JPT_SETTING_COUNT; ++i) CHECK(std::string(jpt_settings[i].key) != "high_resolution_textures");
    CHECK(jpt_config_migrate(ini.string().c_str()) == JPT_MIGRATED_NOTHING);
    CHECK(ReadFile(ini).find("vsync = 0") != std::string::npos);
}

static void TestHelpTexts() {
    CHECK(std::string(UserDataFolderHelp(UserDataOutcome::kPortable)) ==
          "Saves and settings are stored in the portable user_data folder");
    CHECK(std::string(UserDataFolderHelp(UserDataOutcome::kExplicit)).find("--user_data_root") != std::string::npos);
    CHECK(PortFolder() == fs::path([] {
              wchar_t me[MAX_PATH];
              GetModuleFileNameW(nullptr, me, MAX_PATH);
              return fs::path(me).parent_path();
          }()));
}

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const fs::path base = fs::path(temp) / ("jpt_user_data_tests_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(base);
    TestPortableSelected(base);
    TestExplicitRoot(base);
    TestPortableNotWritable(base);
    TestExplicitWhenPortableNotWritable(base);
    TestExplicitNotWritable(base);
    TestRealWriteTest(base);
    TestNoSystemFolderInSource();
    TestMigrationRename(base);
    TestMigrationCopy(base);
    TestMigrationVerify(base);
    TestMigrationFailure(base);
    TestBothExist(base);
    TestEmptyPortable(base);
    TestLegacyKept(base);
    TestSecondLegacy(base);
    TestSettingsPlaced(base);
    TestObsoleteSettingIgnored(base);
    TestHelpTexts();
    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("user_data_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

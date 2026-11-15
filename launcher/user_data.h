// Everything the port writes for the player lives in one portable folder, <port folder>\user_data (docs/user-data.md).
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace jpt {

enum class UserDataOutcome {
    kPortable,             // <port folder>\user_data
    kMigrated,             // an older user folder was moved into the root in use
    kMigratedLegacyKept,   // migrated, but the older folder could not be removed
    kBothExist,            // the root in use holds data and an older folder does too; the older one is left alone
    kMigrationFailed,      // the older folder is used for this run and migration is retried at the next start
    kUnwritable,           // the root cannot be written; nothing is chosen instead and nothing is written
    kExplicit,             // --user_data_root
};

struct UserDataRequest {
    std::filesystem::path port_folder;
    std::filesystem::path explicit_root;  // --user_data_root, empty when not given
    // Folders of the earlier flat layout (settings, registry store and saves side by side), first match wins.
    std::vector<std::filesystem::path> legacy;
};

struct UserDataSelection {
    UserDataOutcome outcome = UserDataOutcome::kPortable;
    std::filesystem::path root;
    std::filesystem::path portable;
    std::filesystem::path legacy;  // the older folder involved, if any
    std::string detail;            // why a failed step failed
    bool explicit_root = false;
    // The earlier flat layout is still in use (kMigrationFailed): settings and saves sit together in root.
    bool flat = false;
};

// The file system calls a migration makes, replaceable to test failures.
struct UserDataFileOps {
    std::function<void(const std::filesystem::path&, const std::filesystem::path&, std::error_code&)> rename =
        [](const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
            std::filesystem::rename(from, to, ec);
        };
    std::function<void(const std::filesystem::path&, const std::filesystem::path&, std::error_code&)> copy_file =
        [](const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::none, ec);
        };
    std::function<void(const std::filesystem::path&, std::error_code&)> remove_all =
        [](const std::filesystem::path& path, std::error_code& ec) { std::filesystem::remove_all(path, ec); };
    std::function<bool(const std::filesystem::path&)> can_write;  // empty: a real write test
};

// Picks the user data root. Data of the earlier layout moves into it before anything reads or writes it; a failed
// move leaves the older folder untouched and in use.
UserDataSelection SelectUserData(const UserDataRequest& request, const UserDataFileOps& ops = {});

// --user_data_root when given, else the TRESPASSER_USER_DATA_ROOT environment variable, else empty.
std::filesystem::path ExplicitUserDataRoot(const char* command_line_value);

struct UserDataMessage {
    bool warning = false;
    std::string text;
};
UserDataMessage DescribeUserData(const UserDataSelection& selection);

// The About page's explanation of where saves and settings are.
const char* UserDataFolderHelp(UserDataOutcome outcome);

// The folder that holds the program. A Linux AppImage would use the folder holding the AppImage file instead.
std::filesystem::path PortFolder();

// The layout below the root.
struct UserDataPaths {
    std::filesystem::path root;
    std::filesystem::path launcher;  // trespasser.ini
    std::filesystem::path saves;     // the game's Installed Directory: saves, registry store, swap files, screenshots
    std::filesystem::path cache;
    std::filesystem::path settings_file;
};
UserDataPaths UserDataLayout(const std::filesystem::path& root, bool flat = false);

}  // namespace jpt

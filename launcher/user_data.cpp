#include "user_data.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "jpt_config.h"
#include "jpt_launcher.h"
#include "jpt_port.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr char kPortableFolder[] = "user_data";
constexpr char kStagingSuffix[] = ".migrating";
constexpr char kLauncherFolder[] = "launcher";
constexpr char kSavesFolder[] = "saves";

std::string Reason(const char* step, const std::error_code& ec) {
    return ec ? std::string(step) + ": " + ec.message() : std::string(step);
}

// True for anything but folders; also when the folder cannot be read, so that it is never mistaken for empty.
bool HoldsFiles(const fs::path& folder) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (it->symlink_status(entry_ec).type() != fs::file_type::directory || entry_ec) {
            return true;
        }
    }
    return bool(ec);
}

bool HasData(const fs::path& folder) {
    std::error_code ec;
    return !folder.empty() && fs::is_directory(folder, ec) && HoldsFiles(folder);
}

bool CanWrite(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec || !fs::is_directory(folder, ec)) {
        return false;
    }
    const fs::path probe = folder / ".write_test";
    {
        std::ofstream file(probe, std::ios::binary | std::ios::trunc);
        file << 'x';
        file.flush();
        if (!file) {
            return false;
        }
    }
    fs::remove(probe, ec);
    return true;
}

bool SameFolder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) {
        return fs::equivalent(a, b, ec);
    }
    return fs::absolute(a, ec).lexically_normal() == fs::absolute(b, ec).lexically_normal();
}

struct TreeEntry {
    fs::file_type type = fs::file_type::none;
    std::uintmax_t size = 0;
    bool operator==(const TreeEntry&) const = default;
};
using TreeListing = std::map<fs::path, TreeEntry>;

bool ListTree(const fs::path& root, TreeListing& listing, std::error_code& ec) {
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::file_status status = it->symlink_status(ec);
        if (ec) {
            return false;
        }
        TreeEntry entry{status.type(), 0};
        if (fs::is_regular_file(status)) {
            entry.size = it->file_size(ec);
            if (ec) {
                return false;
            }
        }
        listing[it->path().lexically_relative(root)] = entry;
    }
    return !ec;
}

bool CopyTree(const fs::path& from, const fs::path& to, const UserDataFileOps& ops, std::error_code& ec) {
    fs::create_directories(to, ec);
    if (ec) {
        return false;
    }
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path target = to / it->path().filename();
        const fs::file_status status = it->symlink_status(ec);
        if (!ec) {
            if (fs::is_directory(status)) {
                CopyTree(it->path(), target, ops, ec);
            } else if (fs::is_regular_file(status)) {
                ops.copy_file(it->path(), target, ec);
            } else {
                ec = std::make_error_code(std::errc::not_supported);
            }
        }
        // increment() clears the code, so a failed entry has to stop the loop here.
        if (ec) {
            return false;
        }
    }
    return !ec;
}

std::string CopyVerified(const fs::path& from, const fs::path& to, const UserDataFileOps& ops) {
    std::error_code ec;
    if (!CopyTree(from, to, ops, ec)) {
        return Reason("cannot copy the data", ec);
    }
    TreeListing original;
    TreeListing copy;
    if (!ListTree(from, original, ec) || !ListTree(to, copy, ec)) {
        return Reason("cannot verify the copy", ec);
    }
    return original == copy ? std::string() : "the copy does not match the original";
}

// The earlier layout kept trespasser.ini beside the saves.
void PlaceSettings(const fs::path& root, const UserDataFileOps& ops) {
    std::error_code ec;
    const fs::path flat = root / kSavesFolder / JPT_SETTINGS_FILE;
    const fs::path placed = root / kLauncherFolder / JPT_SETTINGS_FILE;
    if (!fs::exists(flat, ec) || fs::exists(placed, ec)) {
        return;
    }
    fs::create_directories(placed.parent_path(), ec);
    ops.rename(flat, placed, ec);
}

// Empty when `target` holds the complete data, the flat folder having become target\saves. Only a direct move or a
// verified copy touches the older folder.
std::string MoveIntoPlace(const fs::path& legacy, const fs::path& target, const UserDataFileOps& ops) {
    std::error_code ec;
    const fs::file_status existing = fs::symlink_status(target, ec);
    if (fs::exists(existing)) {
        if (!fs::is_directory(existing)) {
            return "the user data folder exists and is not a folder";
        }
        ops.remove_all(target, ec);
        if (ec) {
            return Reason("cannot clear the empty user data folder", ec);
        }
    }
    fs::create_directories(target, ec);
    if (!ec) {
        ops.rename(legacy, target / kSavesFolder, ec);
        if (!ec) {
            PlaceSettings(target, ops);
            return {};
        }
        std::error_code cleanup;
        ops.remove_all(target, cleanup);
    }

    fs::path staging = target;
    staging += kStagingSuffix;
    ops.remove_all(staging, ec);
    if (ec) {
        return Reason("cannot clear an earlier migration attempt", ec);
    }
    std::string failure = CopyVerified(legacy, staging / kSavesFolder, ops);
    if (failure.empty()) {
        PlaceSettings(staging, ops);
        ops.rename(staging, target, ec);
        if (!ec) {
            return {};
        }
        failure = Reason("cannot activate the copied data", ec);
    }
    ops.remove_all(staging, ec);
    return failure;
}

}  // namespace

UserDataSelection SelectUserData(const UserDataRequest& request, const UserDataFileOps& ops) {
    UserDataSelection selection;
    selection.portable = request.port_folder / kPortableFolder;
    const bool explicit_root = !request.explicit_root.empty();
    selection.explicit_root = explicit_root;
    const fs::path target = explicit_root ? request.explicit_root : selection.portable;
    selection.root = target;

    fs::path legacy;
    for (const fs::path& candidate : request.legacy) {
        if (!SameFolder(candidate, target) && HasData(candidate)) {
            legacy = candidate;
            break;
        }
    }
    selection.legacy = legacy;
    if (!(ops.can_write ? ops.can_write(target) : CanWrite(target))) {
        selection.outcome = UserDataOutcome::kUnwritable;
        return selection;
    }
    const bool target_has_data = HasData(target);

    if (!legacy.empty() && !target_has_data) {
        selection.detail = MoveIntoPlace(legacy, target, ops);
        if (!selection.detail.empty()) {
            selection.outcome = UserDataOutcome::kMigrationFailed;
            selection.root = legacy;
            selection.flat = true;
            return selection;
        }
        selection.outcome = UserDataOutcome::kMigrated;
        std::error_code ec;
        if (fs::exists(legacy, ec)) {
            ops.remove_all(legacy, ec);
            if (ec || fs::exists(legacy, ec)) {
                selection.outcome = UserDataOutcome::kMigratedLegacyKept;
                selection.detail = Reason("cannot remove the old folder", ec);
            }
        }
        return selection;
    }

    PlaceSettings(target, ops);
    if (explicit_root) {
        selection.outcome = legacy.empty() ? UserDataOutcome::kExplicit : UserDataOutcome::kBothExist;
    } else {
        selection.outcome = legacy.empty() ? UserDataOutcome::kPortable : UserDataOutcome::kBothExist;
    }
    return selection;
}

fs::path ExplicitUserDataRoot(const char* command_line_value) {
    if (command_line_value && *command_line_value) {
        return fs::path(command_line_value);
    }
    const char* environment = std::getenv("TRESPASSER_USER_DATA_ROOT");
    return environment && *environment ? fs::path(environment) : fs::path();
}

UserDataMessage DescribeUserData(const UserDataSelection& selection) {
    const std::string root = selection.root.string();
    const std::string legacy = selection.legacy.string();
    switch (selection.outcome) {
    case UserDataOutcome::kPortable:
        return {false, "User data: portable path selected: " + root};
    case UserDataOutcome::kMigrated:
        return {false, "User data: migrated legacy data from " + legacy + " to " + root};
    case UserDataOutcome::kMigratedLegacyKept:
        return {true, "User data: migrated legacy data from " + legacy + " to " + root +
                          "; the old folder was not removed (" + selection.detail + ")"};
    case UserDataOutcome::kBothExist:
        return {true, "User data: " + root + " and legacy data in " + legacy + " both exist; " + root +
                          " selected, the legacy folder is left alone"};
    case UserDataOutcome::kMigrationFailed:
        return {true, "User data: migrating legacy data from " + legacy + " failed (" + selection.detail +
                          "); using the legacy folder for this run, migration is retried at the next start"};
    case UserDataOutcome::kUnwritable:
        if (selection.explicit_root) {
            return {true, "User data: ERROR: the --user_data_root folder " + root +
                              " is not writable. Nothing was written. Choose a folder that can be written"};
        }
        return {true, "User data: ERROR: the port folder is not writable, so " + root +
                          " cannot be created. Nothing was written, and no other location is used. Start with "
                          "--user_data_root <a writable folder> (or set TRESPASSER_USER_DATA_ROOT), or move the "
                          "program to a writable folder"};
    case UserDataOutcome::kExplicit:
        return {false, "User data: --user_data_root selected: " + root};
    }
    return {};
}

const char* UserDataFolderHelp(UserDataOutcome outcome) {
    switch (outcome) {
    case UserDataOutcome::kMigrationFailed:
        return "Saves and settings are stored in the previous user folder, because moving "
               "them into the portable user_data folder failed. It is tried again at the next start";
    case UserDataOutcome::kUnwritable:
        return "The user data folder is not writable, so nothing is saved. Start with --user_data_root <a writable "
               "folder>, or move the program to a writable folder";
    case UserDataOutcome::kExplicit:
        return "Saves and settings are stored in the folder given by --user_data_root";
    default:
        return "Saves and settings are stored in the portable user_data folder";
    }
}

fs::path PortFolder() {
#ifndef _WIN32
    // An AppImage runs from a read-only mount that is gone once it exits: its user_data goes beside the AppImage file.
    char self[JPT_PORT_PATH_MAX] = {};
    char folder[JPT_PORT_PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);
    if (jpt_port_folder(length > 0 ? self : "", std::getenv("APPIMAGE"), folder, sizeof folder)) {
        return fs::path(folder);
    }
    return fs::current_path();
#else
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return fs::current_path();
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return fs::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

UserDataPaths UserDataLayout(const fs::path& root, bool flat) {
    UserDataPaths p;
    p.root = root;
    p.launcher = flat ? root : root / kLauncherFolder;
    p.saves = flat ? root : root / kSavesFolder;
    p.cache = root / "cache";
    p.settings_file = p.launcher / JPT_SETTINGS_FILE;
    return p;
}

}  // namespace jpt

namespace {

bool CopyPath(const fs::path& path, char* out) {
    std::error_code ec;
    const std::string text = fs::absolute(path, ec).lexically_normal().string();
    if (ec || text.size() >= JPT_USER_PATH_CAP) {
        return false;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return true;
}

}  // namespace

extern "C" int jpt_user_data_select(const char* explicit_root, const char* legacy_dir, jpt_user_data_t* out) {
    std::memset(out, 0, sizeof *out);
    jpt::UserDataRequest request;
    request.port_folder = jpt::PortFolder();
    request.explicit_root = jpt::ExplicitUserDataRoot(explicit_root);
    request.legacy.push_back(request.port_folder / "user");
    if (legacy_dir && *legacy_dir) {
        request.legacy.push_back(fs::path(legacy_dir));
    }
    const jpt::UserDataSelection selection = jpt::SelectUserData(request);
    const jpt::UserDataPaths paths = jpt::UserDataLayout(selection.root, selection.flat);
    const jpt::UserDataMessage message = jpt::DescribeUserData(selection);
    out->outcome = static_cast<int>(selection.outcome);
    out->flat = selection.flat;
    out->warning = message.warning;
    out->writable = selection.outcome != jpt::UserDataOutcome::kUnwritable;
    std::snprintf(out->message, sizeof out->message, "%s", message.text.c_str());
    if (out->writable) {
        std::error_code ec;
        fs::create_directories(paths.launcher, ec);
        fs::create_directories(paths.saves, ec);
    }
    return CopyPath(paths.root, out->root) && CopyPath(paths.launcher, out->launcher_dir) &&
           CopyPath(paths.saves, out->saves_dir) && CopyPath(paths.cache, out->cache_dir) &&
           CopyPath(paths.settings_file, out->settings_file);
}

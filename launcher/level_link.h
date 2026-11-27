// Custom level folders the player links instead of importing: their files stay where they are and are only read. The
// package's records (this link, the manifest, the runtime map, guides and listing) live in user_data/custom_levels/<package>.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "level_analysis.h"

namespace jpt {

struct PackageLink {
    std::filesystem::path root;
    std::vector<std::string> scope; // the parts of root that are the package, '/' relative; empty: all of it
    std::string active_fm;          // a fan mission of a tp_mod.ini tree, relative to root
    std::string name;               // as the player sees it
    std::string identity;           // of its scenes, for Relocate Folder
};

std::string PackageLinkText(const PackageLink& link);
bool ParsePackageLink(const std::string& text, PackageLink& link);
bool ReadPackageLink(const std::filesystem::path& records, PackageLink& link);
// Whole or not at all.
bool WritePackageLink(const std::filesystem::path& records, const PackageLink& link);

// The folder is there, a folder and no link.
bool LinkAvailable(const PackageLink& link);
// Whether two links name the same files: one folder, however it is spelled, and the same parts of it.
bool SameLink(const PackageLink& a, const PackageLink& b);
// Of the scenes, by path whatever the case: what a moved folder must still have.
std::string LinkIdentity(const std::vector<PackageFile>& files);
// The game opens files by ANSI paths on Windows: whether it can spell the folder.
bool GameCanOpen(const std::filesystem::path& folder);
// A symbolic link, or any reparse point on Windows (a junction, a mount point).
bool IsReparsePoint(const std::filesystem::path& path, const std::filesystem::file_status& status);

struct LinkCandidate {
    PackageLink link;
    std::string path; // relative to the chosen folder, "." for itself
};

// The packages below a folder: each folder of level files, and each fan mission of a tp_mod.ini tree with the tree's shared
// folders. Archives are not linked; notes says what was left out.
std::vector<LinkCandidate> FindLinkCandidates(const std::filesystem::path& folder, std::vector<std::string>& notes);

} // namespace jpt

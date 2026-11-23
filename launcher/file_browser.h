#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace jpt {

struct FolderEntry {
    std::string name;
    bool directory = false;
    uint64_t size = 0;
};

struct FolderListing {
    std::filesystem::path folder;
    std::vector<FolderEntry> entries;
    bool readable = false;
};

struct Place {
    std::string label;
    std::filesystem::path path;
};

FolderListing ListFolder(const std::filesystem::path& folder, std::string_view extension, bool show_hidden);
std::vector<Place> StandardPlaces(const std::filesystem::path& program_folder);
std::filesystem::path StartingFolder(const std::vector<Place>& places, std::string_view extension);
std::string FormatSize(uint64_t bytes);

// "native", "builtin", or "" from JPT_FILE_CHOOSER.
std::string_view FileChooserPreference();

bool UseBuiltInFileChooser(bool is_windows, bool is_gamescope, bool native_picker_available,
                           std::string_view preference);

}  // namespace jpt

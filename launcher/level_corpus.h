// Every level package below a folder, read as the import reads one: nothing is installed, run or changed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "custom_levels.h"

namespace jpt {

struct CorpusLimits {
    size_t max_depth = 12;
    size_t max_folders = 20000;
    size_t max_packages = 4096;
    std::map<std::string, std::filesystem::path> extracted; // an archive the import does not read -> the folder with its files
    ArchiveLimits archive;
};

struct CorpusPackage {
    std::string path; // relative to the root, '/'
    std::string duplicate_of; // an earlier package with the same contents
    AnalysisResult result;
    bool extracted = false;
    std::string unread; // why it is not analysed: an archive of another kind
};

// Levels with the same problem, whatever the package.
struct CorpusProblem {
    std::string signature; // "MISSING_REQUIRED_TERRAIN", or with its subject
    bool blocks = false;
    size_t levels = 0, packages = 0;
    std::vector<std::string> examples;
};

struct CorpusReport {
    std::vector<CorpusPackage> packages; // by path
    std::vector<CorpusProblem> categories, problems; // most levels first
    std::map<std::string, size_t> totals;
    std::map<int, size_t> scene_versions;
    std::map<int, size_t> actions; // of ATX and Trespasser CE: the levels with them
    std::vector<std::string> notes; // what a limit left out
};

CorpusReport ScanCorpus(const std::filesystem::path& root, const std::filesystem::path& data_dir, const CorpusLimits& limits = {},
                        const std::function<void(const std::string&)>& progress = {});
std::string CorpusText(const CorpusReport& report);
std::string CorpusJson(const CorpusReport& report);

} // namespace jpt

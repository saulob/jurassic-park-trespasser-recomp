#include <cstdio>
#include <filesystem>
#include <fstream>

#include "custom_levels.h"
#include "jpt_launcher.h"
#include "level_corpus.h"

int jpt_analyze_custom_level(const char* path, const char* data_dir, int json) {
    const jpt::AnalysisResult result = jpt::AnalyzeLevelSource(std::filesystem::path(path), data_dir ? std::filesystem::path(data_dir) : std::filesystem::path());
    const bool ok = result.import.status == jpt::ImportStatus::kInstalled;
    const std::string& text = json ? result.json : result.report;
    std::fwrite(text.data(), 1, text.size(), stdout);
    if (!json && !result.import.log.empty()) std::fputs("\nImport log\n", stdout);
    for (const std::string& line : result.import.log)
        if (!json) std::fprintf(stdout, "  %s\n", line.c_str());
    if (!json || text.empty())
        std::fprintf(json ? stderr : stdout, "\nResult: %s%s%s\n", ok ? "can be installed" : jpt::ImportStatusTitle(result.import.status),
                     result.import.detail.empty() ? "" : ": ", result.import.detail.c_str());
    std::fflush(stdout);
    return ok ? 0 : 1;
}

static bool WriteReport(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    std::fprintf(stderr, "%s %s\n", out ? "wrote" : "could not write", file.string().c_str());
    return bool(out);
}

int jpt_analyze_custom_levels(const char* root, const char* data_dir, const char* report_dir, int json) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return std::fprintf(stderr, "%s is not a folder\n", root), 2;
    const jpt::CorpusReport report = jpt::ScanCorpus(root, data_dir ? std::filesystem::path(data_dir) : std::filesystem::path(), {},
                                                     [](const std::string& path) { std::fprintf(stderr, "%s\n", path.c_str()); });
    const std::string text = jpt::CorpusText(report), structured = jpt::CorpusJson(report);
    const std::string& shown = json ? structured : text;
    std::fwrite(shown.data(), 1, shown.size(), stdout);
    std::fflush(stdout);
    if (!report_dir) return 0;
    const std::filesystem::path folder = report_dir;
    std::filesystem::create_directories(folder, ec);
    const bool written = WriteReport(folder / "custom_levels_report.txt", text) && WriteReport(folder / "custom_levels_report.json", structured);
    return written ? 0 : 1;
}

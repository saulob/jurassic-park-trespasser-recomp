// Developer tool: converts the text of the TresCom translation downloads (extracted folders) into the files embedded in the
// port (assets/translations). Each source is recognised by name, size and SHA-256; nothing else of a package is read.
//   translation_pack <folder with the extracted downloads> <output folder>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "game_files.h"
#include "jpt_l10n.h"

namespace fs = std::filesystem;

namespace {

struct Source {
    const char* name;
    uint64_t size;
    const char* sha256;
    const char* pack;  // "stream" or "effects" for a TPA
};

struct Package {
    const char* language;
    int content;
    int codepage;
    std::vector<Source> sources;
    bool swapped_e = false; // the file has every è as ê and every ê as è
};

const std::vector<Package> kPackages = {
    {"pt-BR", JPT_L10N_GAME_TEXT, 1252,
     {{"game_strings.txt", 9631, "5c99602f611fb34aca60a63a524c65b75b300ba62eaf6d94efd4c40a4ad396b9", nullptr}}},
    {"fr", JPT_L10N_GAME_TEXT, 1252,
     {{"game_strings.txt", 10133, "d279fe6debd671310db8d89a780fed43d9f4b832bf06d46657babbcb69db8413", nullptr}}, true},
    {"de", JPT_L10N_GAME_TEXT, 1252,
     {{"game_strings.txt", 10078, "3d1c2851189b0b93886038fb61fdc3341d1a55a614517b6b56c699864a6cb6a4", nullptr}}},
    {"es", JPT_L10N_GAME_TEXT, 1252,
     {{"game_strings.txt", 10314, "4ebeef7731001beb0e4db6505ccb47fd8c32dbc429e67c83d4b852c9ef6c2500", nullptr}}},
    {"ru", JPT_L10N_GAME_TEXT, 1251,
     {{"game_strings.txt", 9910, "8ffa0f59a6960e2565a48afdd232dd11608d72da991f063e3b68585954c9a2c1", nullptr}}},
    {"ru", JPT_L10N_SUBTITLES, 1251,
     {{"stream.tpa", 206426307, "6b63d6def3c54fbf0b0d0261c00a0769a830394b7aa1753a0b72755ccd015c56", "stream"},
      {"effects.tpa", 18194487, "c1d3f907e89985884a2e6ca2e9f948949855ac2b7cf0b712058ab283324cbbbf", "effects"}}},
    {"it", JPT_L10N_SUBTITLES, 1252,
     {{"stream.tpa", 223765381, "e5cdae29422ba9004e8b9c1e4439cf316881b3cc45f1e96d143816a31e78fb81", "stream"}}},
};

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

bool ReadAll(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return bool(in);
}

void SwapGraveAndCircumflexE(std::string& text) {
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != '\xC3') continue;
        char& next = text[i + 1];
        if (next == '\xA8' || next == '\x88') next = static_cast<char>(next + 2);
        else if (next == '\xAA' || next == '\x8A') next = static_cast<char>(next - 2);
        ++i;
    }
}

std::string Escaped(const std::string& text) {
    std::string out(text.size() * 2 + 1, '\0');
    const size_t n = jpt_l10n_escape(text.c_str(), out.data(), out.size());
    out.resize(n == size_t(-1) ? 0 : n);
    return out;
}

std::string FindSource(const fs::path& folder, const Source& source) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (Lower(it->path().filename().string()) != source.name || !it->is_regular_file(ec)) continue;
        if (it->file_size(ec) != source.size) continue;
        if (jpt::Sha256File(it->path()) == source.sha256) return it->path().string();
    }
    return {};
}

void CollectText(void* ctx, unsigned id, const char* utf8) {
    static_cast<std::vector<std::pair<unsigned, std::string>>*>(ctx)->emplace_back(id, utf8);
}

struct Caption {
    std::string pack;
    uint32_t sample = 0;
    std::vector<std::pair<uint32_t, std::string>> lines;
};

struct CaptionScan {
    const char* pack;
    int codepage;
    std::vector<Caption>* out;
};

void CollectLine(void* ctx, uint32_t units, const char* text, size_t n) {
    auto* line = static_cast<std::pair<Caption*, int>*>(ctx);
    std::string utf8(n * 3 + 1, '\0');
    const size_t written = jpt_cp_to_utf8(line->second, text, n, utf8.data(), utf8.size());
    utf8.resize(written == size_t(-1) ? 0 : written);
    line->first->lines.emplace_back(units, utf8);
}

void CollectCaption(void* ctx, uint32_t sample, const uint8_t* block, size_t n) {
    auto* scan = static_cast<CaptionScan*>(ctx);
    Caption caption;
    caption.pack = scan->pack;
    caption.sample = sample;
    std::pair<Caption*, int> line(&caption, scan->codepage);
    if (jpt_caption_lines(block, n, CollectLine, &line) > 0) scan->out->push_back(std::move(caption));
}

int ReadAt(void* ctx, uint64_t offset, void* buf, size_t n) {
    auto* in = static_cast<std::ifstream*>(ctx);
    in->clear();
    in->seekg(static_cast<std::streamoff>(offset));
    in->read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    return in->good() && static_cast<size_t>(in->gcount()) == n;
}

bool PackStrings(const Package& package, const std::string& path, std::string& out, int& entries) {
    std::string bytes;
    std::vector<std::pair<unsigned, std::string>> items;
    jpt_gs_stats_t stats{};
    if (!ReadAll(path, bytes) || !jpt_gs_parse(bytes.data(), bytes.size(), package.codepage, 1, CollectText, &items, &stats))
        return false;
    std::sort(items.begin(), items.end());
    if (package.swapped_e)
        for (auto& item : items) SwapGraveAndCircumflexE(item.second);
    out = std::string(JPT_L10N_STRINGS_HEADER) + "\n";
    for (const auto& [id, text] : items) out += std::to_string(id) + "\t" + Escaped(text) + "\n";
    entries = stats.entries;
    std::printf("  %zu strings (%d duplicate, %d invalid lines, %d sentence spaces restored)\n", items.size(), stats.duplicates,
                stats.invalid, stats.normalized);
    return true;
}

bool PackSubtitles(const Package& package, const std::vector<std::pair<const Source*, std::string>>& files, std::string& out,
                   int& samples) {
    std::vector<Caption> captions;
    for (const auto& [source, path] : files) {
        std::ifstream in(path, std::ios::binary);
        std::error_code ec;
        CaptionScan scan{source->pack, package.codepage, &captions};
        jpt_tpa_stats_t stats{};
        if (!in || !jpt_tpa_captions(ReadAt, &in, fs::file_size(path, ec), CollectCaption, &scan, &stats)) return false;
        std::printf("  %s: %d captioned of %d samples (%d repeated IDs, %d malformed)\n", source->name, stats.captioned,
                    stats.samples, stats.duplicates, stats.malformed);
    }
    out = std::string(JPT_L10N_SUBTITLES_HEADER) + "\n";
    char sample[16];
    for (const Caption& caption : captions) {
        std::snprintf(sample, sizeof sample, "%08x", caption.sample);
        out += "S\t" + caption.pack + "\t" + sample + "\n";
        for (const auto& [units, text] : caption.lines) out += "C\t" + std::to_string(units) + "\t" + Escaped(text) + "\n";
    }
    samples = int(captions.size());
    return samples > 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: translation_pack <folder with the extracted downloads> <output folder>\n");
        return 2;
    }
    const fs::path folder = argv[1], output = argv[2];
    std::error_code ec;
    fs::create_directories(output, ec);
    int failures = 0, written = 0;
    for (const Package& package : kPackages) {
        const bool subtitles = package.content == JPT_L10N_SUBTITLES;
        std::vector<std::pair<const Source*, std::string>> files;
        for (const Source& source : package.sources) {
            const std::string found = FindSource(folder, source);
            if (!found.empty()) files.emplace_back(&source, found);
        }
        std::printf("%s %s: ", package.language, subtitles ? "subtitles" : "game text");
        if (files.size() != package.sources.size()) {
            std::printf("source not found (name, size and SHA-256 must match)\n");
            ++failures;
            continue;
        }
        std::printf("\n");
        std::string out;
        int count = 0;
        const bool ok = subtitles ? PackSubtitles(package, files, out, count) : PackStrings(package, files.front().second, out, count);
        const fs::path target = output / (std::string(package.language) + "." + (subtitles ? JPT_L10N_SUBTITLES_ASSET : JPT_L10N_STRINGS_ASSET) + ".txt");
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!ok || !file) {
            std::printf("  failed\n");
            ++failures;
            continue;
        }
        std::printf("  %s (%zu bytes)\n", target.filename().string().c_str(), out.size());
        ++written;
    }
    std::printf("%d files written, %d failed\n", written, failures);
    return failures ? 1 : 0;
}

// Custom levels: the package import (ZIP, 7z, folder) with its limits, refusals and rollback, the listing, README and
// removal, and the overlay an active package answers the game's file lookups through. Scratch folders only.
//   custom_level_tests                        the synthetic checks
//   custom_level_tests --packages DIR         imports every archive and folder of DIR into a scratch folder and reports it
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "archive_reader.h"
#include "custom_levels.h"
#include "jpt_custom_level.h"
#include "jpt_szdd.h"
#include "jpt_vpath.h"
#include "localization/ui_text.h"
#include "updater/third_party/miniz/miniz.h"
#include "utf8.h"

#ifdef _WIN32
#include <fcntl.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" {
int custom_level_files_start(const char* game_dir);
int custom_level_file(const char* path, char* out, size_t cap);
int custom_level_write_refused(const char* path);
int custom_level_lz_open(const char* path, int for_reading);
int custom_level_lz_read(int handle, char* buffer, int wanted, int* result);
int custom_level_lz_seek(int handle, long offset, int origin, int* result);
int custom_level_lz_close(int handle);
void host_log(const char*, const char*, ...) {}
int __stdcall jpt_dsound_create(void*, void**, void*) { return -1; }
}
#endif

namespace fs = std::filesystem;
using namespace jpt;

static int g_checks, g_failures;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

// The same five files as LZMA and as LZMA2, one solid block.
static const unsigned char kLzma7z[] = {
    0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04, 0x87, 0x99, 0x23, 0x30, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1F, 0x17, 0x91, 0x8A, 0x00, 0x5F, 0x2E, 0x95, 0xC2, 0x2F, 0xDE, 0xEC, 0xF0, 0xCB, 0xC2, 0xC3, 0xF5, 0x8F, 0x1E, 0x81,
    0xAC, 0x55, 0x1B, 0x50, 0xF2, 0x0C, 0x00, 0x7D, 0x33, 0xC2, 0x39, 0xB0, 0x36, 0xB3, 0x5D, 0xA0, 0xFF, 0xDC, 0x3B, 0x93, 0x7C, 0x50, 0xB5, 0x5E,
    0xEB, 0x61, 0xB0, 0x94, 0xED, 0x04, 0x6B, 0x80, 0xAE, 0xB1, 0x2F, 0x92, 0xAE, 0x44, 0xA6, 0x16, 0x0A, 0xDB, 0x33, 0xD5, 0x66, 0x63, 0xBE, 0x3F,
    0xDF, 0xFA, 0x6A, 0xB0, 0xB7, 0xAD, 0x5D, 0xB1, 0xAD, 0x89, 0x53, 0x34, 0x21, 0x79, 0xBD, 0x45, 0xDD, 0x3B, 0x90, 0x7E, 0xBF, 0x19, 0x9F, 0xAE,
    0xEF, 0xBC, 0xF5, 0x7B, 0xB2, 0x60, 0xF0, 0xFE, 0x0D, 0xE0, 0x18, 0x2B, 0xB2, 0x34, 0x91, 0x44, 0x93, 0x3E, 0xDB, 0xA7, 0x99, 0xFF, 0xFF, 0xC1,
    0x2F, 0x7C, 0x00, 0x01, 0x04, 0x06, 0x00, 0x01, 0x09, 0x73, 0x00, 0x07, 0x0B, 0x01, 0x00, 0x01, 0x23, 0x03, 0x01, 0x01, 0x05, 0x5D, 0x00, 0x00,
    0x01, 0x00, 0x0C, 0x89, 0x64, 0x00, 0x08, 0x0D, 0x05, 0x09, 0x86, 0x0B, 0x81, 0x30, 0x78, 0x81, 0x90, 0x0A, 0x01, 0xD4, 0x9C, 0xA4, 0x3F, 0x86,
    0x28, 0x06, 0xFD, 0xE7, 0x8D, 0x03, 0x81, 0xFE, 0x65, 0x30, 0x08, 0x53, 0x73, 0xF6, 0xF2, 0x00, 0x00, 0x05, 0x05, 0x11, 0x80, 0xC5, 0x00, 0x4A,
    0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2F, 0x00, 0x6A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65,
    0x00, 0x2E, 0x00, 0x73, 0x00, 0x63, 0x00, 0x6E, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2F,
    0x00, 0x6A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2E, 0x00, 0x67, 0x00, 0x72, 0x00, 0x66, 0x00, 0x00, 0x00, 0x4A,
    0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2F, 0x00, 0x6A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65,
    0x00, 0x2D, 0x00, 0x31, 0x00, 0x33, 0x00, 0x30, 0x00, 0x2E, 0x00, 0x70, 0x00, 0x69, 0x00, 0x64, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x75, 0x00, 0x6E,
    0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2F, 0x00, 0x6A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2D, 0x00, 0x31,
    0x00, 0x33, 0x00, 0x30, 0x00, 0x2E, 0x00, 0x73, 0x00, 0x77, 0x00, 0x70, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C,
    0x00, 0x65, 0x00, 0x2F, 0x00, 0x52, 0x00, 0x65, 0x00, 0x61, 0x00, 0x64, 0x00, 0x6D, 0x00, 0x65, 0x00, 0x2E, 0x00, 0x74, 0x00, 0x78, 0x00, 0x74,
    0x00, 0x00, 0x00, 0x00, 0x00,
};
static const unsigned char kLzma27z[] = {
    0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04, 0xB6, 0xA8, 0x8C, 0xD5, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xA0, 0x1E, 0x1E, 0xDF, 0xE0, 0x09, 0x63, 0x00, 0x6C, 0x5D, 0x00, 0x5F, 0x2E, 0x95, 0xC2, 0x2F, 0xDE, 0xEC, 0xF0, 0xCB,
    0xC2, 0xC3, 0xF5, 0x8F, 0x1E, 0x81, 0xAC, 0x55, 0x1B, 0x50, 0xF2, 0x0C, 0x00, 0x7D, 0x33, 0xC2, 0x39, 0xB0, 0x36, 0xB3, 0x5D, 0xA0, 0xFF, 0xDC,
    0x3B, 0x93, 0x7C, 0x50, 0xB5, 0x5E, 0xEB, 0x61, 0xB0, 0x94, 0xED, 0x04, 0x6B, 0x80, 0xAE, 0xB1, 0x2F, 0x92, 0xAE, 0x44, 0xA6, 0x16, 0x0A, 0xDB,
    0x33, 0xD5, 0x66, 0x63, 0xBE, 0x3F, 0xDF, 0xFA, 0x6A, 0xB0, 0xB7, 0xAD, 0x5D, 0xB1, 0xAD, 0x89, 0x53, 0x34, 0x21, 0x79, 0xBD, 0x45, 0xDD, 0x3B,
    0x90, 0x7E, 0xBF, 0x19, 0x9F, 0xAE, 0xEF, 0xBC, 0xF5, 0x7B, 0xB2, 0x60, 0xF0, 0xFE, 0x0D, 0xE0, 0x18, 0x2B, 0xB2, 0x34, 0x91, 0x44, 0x93, 0x3D,
    0xE5, 0x6E, 0xA2, 0x00, 0x01, 0x04, 0x06, 0x00, 0x01, 0x09, 0x74, 0x00, 0x07, 0x0B, 0x01, 0x00, 0x01, 0x21, 0x21, 0x01, 0x08, 0x0C, 0x89, 0x64,
    0x00, 0x08, 0x0D, 0x05, 0x09, 0x86, 0x0B, 0x81, 0x30, 0x78, 0x81, 0x90, 0x0A, 0x01, 0xD4, 0x9C, 0xA4, 0x3F, 0x86, 0x28, 0x06, 0xFD, 0xE7, 0x8D,
    0x03, 0x81, 0xFE, 0x65, 0x30, 0x08, 0x53, 0x73, 0xF6, 0xF2, 0x00, 0x00, 0x05, 0x05, 0x11, 0x80, 0xC5, 0x00, 0x4A, 0x00, 0x75, 0x00, 0x6E, 0x00,
    0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2F, 0x00, 0x6A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2E, 0x00, 0x73, 0x00,
    0x63, 0x00, 0x6E, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2F, 0x00, 0x6A, 0x00, 0x75, 0x00,
    0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2E, 0x00, 0x67, 0x00, 0x72, 0x00, 0x66, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x75, 0x00, 0x6E, 0x00,
    0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2F, 0x00, 0x6A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2D, 0x00, 0x31, 0x00,
    0x33, 0x00, 0x30, 0x00, 0x2E, 0x00, 0x70, 0x00, 0x69, 0x00, 0x64, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00,
    0x65, 0x00, 0x2F, 0x00, 0x6A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2D, 0x00, 0x31, 0x00, 0x33, 0x00, 0x30, 0x00,
    0x2E, 0x00, 0x73, 0x00, 0x77, 0x00, 0x70, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x75, 0x00, 0x6E, 0x00, 0x67, 0x00, 0x6C, 0x00, 0x65, 0x00, 0x2F, 0x00,
    0x52, 0x00, 0x65, 0x00, 0x61, 0x00, 0x64, 0x00, 0x6D, 0x00, 0x65, 0x00, 0x2E, 0x00, 0x74, 0x00, 0x78, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00,
};

using Files = std::vector<std::pair<std::string, std::string>>;

static std::string Scene(const std::string& world) {
    std::string bytes("\xBE\xBA\xCE\x0A", 4);
    const std::string path = "c:\\tres cd\\data\\" + world;
    bytes += std::string(80, '\0') + path + std::string(256 - path.size(), '\0') + std::string(1200, '\0') + "Header";
    return bytes;
}

static void Write(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

static std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static void WriteTree(const fs::path& root, const Files& files) {
    for (const auto& [name, bytes] : files) Write(root / PathFromUtf8(name), bytes);
}

static void WriteZip(const fs::path& file, const Files& files, mz_uint32 attributes = 0) {
    mz_zip_archive zip{};
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto& [name, bytes] : files) {
        mz_zip_writer_add_mem_ex_v2(&zip, name.c_str(), bytes.data(), bytes.size(), nullptr, 0, 6, 0, 0, nullptr, nullptr, 0, nullptr, 0);
    }
    void* data = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &data, &size);
    std::string bytes(static_cast<const char*>(data), size);
    mz_free(data);
    mz_zip_writer_end(&zip);
    if (attributes) {
        // The external attributes of every central directory entry, with a Unix origin.
        for (size_t at = bytes.find("PK\1\2"); at != std::string::npos; at = bytes.find("PK\1\2", at + 4)) {
            bytes[at + 5] = 3;
            for (int i = 0; i < 4; ++i) bytes[at + 38 + size_t(i)] = char(attributes >> (8 * i));
        }
    }
    Write(file, bytes);
}

// A 7z archive whose files are stored one block each.
static std::string SevenZip(const Files& files, uint32_t attributes = 0) {
    const auto number = [](uint64_t v) {
        std::string out;
        if (v < 0x80) return out + char(v);
        if (v < 0x4000) return out + char(0x80 | (v >> 8)) + char(v);
        return out + char(0xE0) + char(v) + char(v >> 8) + char(v >> 16);
    };
    const auto u32 = [](uint32_t v) {
        std::string out;
        for (int i = 0; i < 4; ++i) out += char(v >> (8 * i));
        return out;
    };
    const auto crc = [](const std::string& s) { return uint32_t(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(s.data()), s.size())); };
    std::string data, sizes, folders, crcs, names(1, '\0'), attribute_list;
    for (const auto& [name, bytes] : files) {
        data += bytes;
        sizes += number(bytes.size());
        folders += std::string("\x01\x01\x00", 3);
        crcs += u32(crc(bytes));
        for (const char c : name) names += std::string(1, c) + '\0';
        names += std::string(2, '\0');
        attribute_list += u32(attributes);
    }
    const std::string zero(1, '\0'), count = number(files.size());
    std::string header = "\x01\x04\x06" + number(0) + count + "\x09" + sizes + zero;
    header += std::string("\x07\x0B") + count + zero + folders + "\x0C" + sizes + "\x0A\x01" + crcs + zero + zero;
    header += std::string("\x05") + count + "\x11" + number(names.size()) + names;
    if (attributes) header += std::string("\x15") + number(attribute_list.size() + 2) + "\x01" + zero + attribute_list;
    header += zero + zero;
    const auto u64 = [&u32](uint64_t v) { return u32(uint32_t(v)) + u32(uint32_t(v >> 32)); };
    const std::string start = u64(data.size()) + u64(header.size()) + u32(crc(header));
    return std::string("7z\xBC\xAF\x27\x1C\x00\x04", 8) + u32(crc(start)) + start + data + header;
}

static const fs::path& Scratch() {
    static const fs::path root = [] {
        const fs::path base = fs::temp_directory_path() / "jpt_custom_level_tests";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base);
        return fs::canonical(base);
    }();
    return root;
}

static fs::path Fresh(const char* name) {
    const fs::path dir = Scratch() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

static Files Level(const std::string& stem) {
    return {{stem + ".scn", Scene(stem + ".grf")}, {stem + ".grf", "world of " + stem}, {stem + "-130.pid", "pid"},
            {stem + "-130.swp", "swap of " + stem}, {stem + ".wtd", "terrain"}};
}

// Nothing of a failed import stays behind.
static bool Untouched(const fs::path& user) {
    std::error_code ec;
    return !fs::exists(CustomLevelsDir(user), ec) || fs::is_empty(CustomLevelsDir(user), ec);
}

static ImportResult FromZip(const fs::path& user, const Files& files, const char* name = "level.zip", mz_uint32 attributes = 0) {
    const fs::path zip = Scratch() / "in" / name;
    WriteZip(zip, files, attributes);
    return ImportLevelArchive(zip, user, {});
}

static void TestImportKinds() {
    const fs::path user = Fresh("kinds");
    Files files = Level("nested");
    for (auto& [name, bytes] : files) name = "Some Level/data/" + name;
    files.push_back({"Some Level/data/anim/Door.asa", "version 2"});
    files.push_back({"Some Level/data/texhw/map/A.dds", "dds"});
    files.push_back({"Some Level/Read Me.txt", "hello"});
    const ImportResult zip = FromZip(user, files, "Some Level v1.0.zip");
    CHECK(zip.ok() && zip.package == "Some Level v1.0" && zip.files == files.size() && zip.skipped == 0);
    for (const auto& [name, bytes] : files) CHECK(Read(CustomLevelsDir(user) / zip.package / PathFromUtf8(name)) == bytes);

    for (const auto& [name, data, size] : {std::tuple("lzma.7z", kLzma7z, sizeof kLzma7z), std::tuple("lzma2.7z", kLzma27z, sizeof kLzma27z)}) {
        const fs::path other = Fresh("sevenzip");
        const fs::path archive = Scratch() / "in" / name;
        Write(archive, std::string(reinterpret_cast<const char*>(data), size));
        const ImportResult result = ImportLevelArchive(archive, other, {});
        CHECK(result.ok() && result.files == 5);
        const fs::path dir = CustomLevelsDir(other) / result.package / "Jungle";
        CHECK(Read(dir / "jungle-130.swp").size() == 400 && Read(dir / "jungle.scn") == Scene("jungle.grf") + '\0');
        const std::vector<LevelPackage> listed = ListLevelPackages(other, {});
        CHECK(listed.size() == 1 && listed[0].readme == "Jungle/Readme.txt" && listed[0].mentions_atx && !listed[0].mentions_ce);
        CHECK(listed.size() == 1 && listed[0].scenes.size() == 1 && listed[0].scenes[0].valid && listed[0].scenes[0].missing.empty());
    }
    {
        const fs::path other = Fresh("stored");
        Write(Scratch() / "in" / "stored.zip", SevenZip(Level("stored")));
        const ImportResult result = ImportLevelArchive(Scratch() / "in" / "stored.zip", other, {});
        CHECK(result.ok() && result.package == "stored");
    }
    {
        const fs::path other = Fresh("folders"), source = Fresh("in folder");
        WriteTree(source / "Lost Level", Level("lost"));
        const ImportResult result = ImportLevelFolder(source / "Lost Level", other, {});
        CHECK(result.ok() && result.package == "Lost Level");
        CHECK(Read(CustomLevelsDir(other) / "Lost Level" / "lost.grf") == "world of lost");
        CHECK(Read(source / "Lost Level" / "lost.grf") == "world of lost");
        WriteTree(source / "Parent" / "data", Level("inner"));
        CHECK(ImportLevelFolder(source / "Parent" / "data", other, {}).package == "Parent");
        CHECK(ImportLevelFolder(CustomLevelsDir(other), other, {}).status == ImportStatus::kUnsafe);
        CHECK(ImportLevelFolder(other, other, {}).status == ImportStatus::kUnsafe);
        CHECK(ImportLevelFolder(source / "none", other, {}).status == ImportStatus::kUnreadable);
    }
}

static void TestSafety() {
    const fs::path user = Fresh("safety");
    const auto with = [](const std::string& name) {
        Files files = Level("a");
        files.push_back({name, "x"});
        return files;
    };
    // miniz refuses to write absolute names: the names are put in afterwards, at the same length.
    const auto renamed = [&user](const std::string& name) {
        const fs::path zip = Scratch() / "in" / "bad.zip";
        Files files = Level("a");
        const std::string stand_in(name.size(), 'Q');
        files.push_back({stand_in, "x"});
        WriteZip(zip, files);
        std::string bytes = Read(zip);
        for (size_t at = bytes.find(stand_in); at != std::string::npos; at = bytes.find(stand_in, at + 1)) bytes.replace(at, name.size(), name);
        Write(zip, bytes);
        return ImportLevelArchive(zip, user, {});
    };
    for (const char* name : {"../evil.txt", "data/../../evil.txt", "/evil.txt", "C:\\evil.txt", "C:evil.txt", "\\\\server\\evil.txt", "..\\evil.txt",
                             "a/con.txt", "a/nul", "a/evil.txt ", "a/evil.txt.", "a/ev:il.txt", "a/ev|il.txt", "a//evil.txt"}) {
        const ImportResult result = renamed(name);
        CHECK(result.status == ImportStatus::kUnsafe);
        if (result.status != ImportStatus::kUnsafe) std::printf("  %s -> %s\n", name, ImportStatusTitle(result.status));
        CHECK(Untouched(user));
        Write(Scratch() / "in" / "bad.7z", SevenZip(with(name)));
        CHECK(ImportLevelArchive(Scratch() / "in" / "bad.7z", user, {}).status == ImportStatus::kUnsafe);
        CHECK(Untouched(user));
    }
    CHECK(!fs::exists(Scratch() / "evil.txt") && !fs::exists(user / "evil.txt"));
    CHECK(FromZip(user, Level("link"), "link.zip", 0120777u << 16).status == ImportStatus::kUnsafe);
    Write(Scratch() / "in" / "link.7z", SevenZip(Level("link"), 0x8000u | 0120777u << 16));
    CHECK(ImportLevelArchive(Scratch() / "in" / "link.7z", user, {}).status == ImportStatus::kUnsafe);
    Write(Scratch() / "in" / "link.7z", SevenZip(Level("link"), 0x400u));
    CHECK(ImportLevelArchive(Scratch() / "in" / "link.7z", user, {}).status == ImportStatus::kUnsafe);

    CHECK(FromZip(user, with("A.SCN")).status == ImportStatus::kUnsafe);
    CHECK(FromZip(user, with("a.scn/b.txt")).status == ImportStatus::kUnsafe);
    Write(Scratch() / "in" / "twice.7z", SevenZip(with("A.grf")));
    CHECK(ImportLevelArchive(Scratch() / "in" / "twice.7z", user, {}).status == ImportStatus::kUnsafe);
    std::string deep;
    for (int i = 0; i < 20; ++i) deep += "d/";
    CHECK(FromZip(user, with(deep + "x.txt")).status == ImportStatus::kUnsafe);

    CHECK(FromZip(user, {{"readme.txt", "no level"}}).status == ImportStatus::kNoLevel);
    CHECK(FromZip(user, {{"fake.scn", std::string(200, 'x')}, {"fake.grf", "x"}}).status == ImportStatus::kNoLevel);
    CHECK(Untouched(user));

    Files programs = Level("mixed");
    for (const char* name : {"tpass.exe", "a3d.dll", "run.bat", "install.cmd", "x.ps1", "x.vbs", "x.js", "x.lnk", "x.com", "x.scr", "x.msi", "atx.asi",
                             "x.sh", "x.so", "x.reg", "page.html", "crash.dmp", "noext"})
        programs.push_back({name, "MZ"});
    const ImportResult mixed = FromZip(user, programs, "mixed.zip");
    CHECK(mixed.ok() && mixed.files == 5 && mixed.skipped == 18);
    CHECK(!fs::exists(CustomLevelsDir(user) / "mixed" / "tpass.exe"));
#ifndef _WIN32
    const fs::path source = Fresh("linked");
    WriteTree(source, Level("linked"));
    std::error_code ec;
    fs::create_symlink(user, source / "out.txt", ec);
    CHECK(ec || ImportLevelFolder(source, user, {}).status == ImportStatus::kUnsafe);
#endif
}

static void TestLimits() {
    const fs::path user = Fresh("limits");
    const fs::path zip = Scratch() / "in" / "limit.zip", seven = Scratch() / "in" / "limit.7z";
    Files files = Level("limit");
    files.push_back({"big.txt", std::string(5000, 'b')});
    WriteZip(zip, files);
    Write(seven, SevenZip(files));
    const auto status = [&user](const fs::path& archive, const std::function<void(ArchiveLimits&)>& change) {
        ArchiveLimits limits;
        change(limits);
        return ImportLevelArchive(archive, user, {}, limits).status;
    };
    for (const fs::path& archive : {zip, seven}) {
        CHECK(status(archive, [](ArchiveLimits& l) { l.max_file_bytes = 4999; }) == ImportStatus::kTooLarge);
        CHECK(status(archive, [](ArchiveLimits& l) { l.max_total_bytes = 5010; }) == ImportStatus::kTooLarge);
        CHECK(status(archive, [](ArchiveLimits& l) { l.max_entries = 3; }) == ImportStatus::kTooLarge);
        CHECK(status(archive, [](ArchiveLimits& l) { l.max_archive_bytes = 100; }) == ImportStatus::kTooLarge);
        CHECK(Untouched(user));
    }
    CHECK(status(seven, [](ArchiveLimits& l) { l.max_block_bytes = 1000; }) == ImportStatus::kTooLarge);
    CHECK(Untouched(user));
}

static void TestRollback() {
    const fs::path user = Fresh("rollback");
    CHECK(FromZip(user, Level("kept"), "kept.zip").ok());
    const auto only_kept = [&user] {
        size_t entries = 0;
        for (const auto& entry : fs::directory_iterator(CustomLevelsDir(user))) {
            (void)entry;
            ++entries;
        }
        return entries == 1 && Read(CustomLevelsDir(user) / "kept" / "kept.grf") == "world of kept";
    };
    Files files = Level("broken");
    files.push_back({"broken.bmp", std::string(20000, 'p') + "tail"});
    const fs::path zip = Scratch() / "in" / "broken.zip", seven = Scratch() / "in" / "broken.7z";
    WriteZip(zip, files);
    Write(seven, SevenZip(files));
    for (const fs::path& archive : {zip, seven}) {
        std::string bytes = Read(archive);
        for (size_t cut : {size_t(8), bytes.size() / 2, bytes.size() - 3}) {
            Write(archive, bytes.substr(0, cut));
            const ImportResult result = ImportLevelArchive(archive, user, {});
            CHECK(!result.ok() && only_kept());
        }
        std::string changed = bytes;
        const size_t at = changed.find("swap of broken");
        if (at != std::string::npos) changed[at] ^= 1;
        else changed[60] ^= 0x55;
        Write(archive, changed);
        CHECK(!ImportLevelArchive(archive, user, {}).ok() && only_kept());
        Write(archive, bytes);
    }
    ImportHooks cancel;
    cancel.progress = [](uint64_t done, uint64_t) { return done < 100; };
    CHECK(ImportLevelArchive(zip, user, cancel).status == ImportStatus::kCancelled && only_kept());
    CHECK(ImportLevelArchive(seven, user, cancel).status == ImportStatus::kCancelled && only_kept());
    Write(Scratch() / "in" / "x.rar", "Rar!\x1A\x07\x01 and more bytes");
    CHECK(ImportLevelArchive(Scratch() / "in" / "x.rar", user, {}).status == ImportStatus::kUnsupported);
    CHECK(ImportLevelArchive(Scratch() / "in" / "absent.zip", user, {}).status == ImportStatus::kUnreadable && only_kept());

    Write(CustomLevelsDir(user) / ".import-0" / "half.scn", "x");
    Write(CustomLevelsDir(user) / ".removing-old" / "half.scn", "x");
    CHECK(ListLevelPackages(user, {}).size() == 1);
    CleanLevelStaging(user);
    CHECK(only_kept());
}

static void TestDuplicatesAndIsolation() {
    const fs::path user = Fresh("duplicates");
    CHECK(FromZip(user, Level("one"), "one.zip").package == "one");
    const ImportResult again = FromZip(user, Level("one"), "renamed copy.zip");
    CHECK(again.status == ImportStatus::kAlreadyInstalled && again.package == "one");
    CHECK(FromZip(user, Level("two"), "one.zip").package == "one-2");
    CHECK(Read(CustomLevelsDir(user) / "one" / "one.grf") == "world of one" && !fs::exists(CustomLevelsDir(user) / "one" / "two.grf"));
    for (const char* name : {"con.zip", "___.zip", "\xC3\xA9t\xC3\xA9.zip"}) {
        const fs::path zip = Scratch() / "in" / PathFromUtf8(name);
        WriteZip(Scratch() / "in" / "plain.zip", Level(std::string("n") + std::to_string(std::strlen(name))));
        std::error_code ec;
        fs::copy_file(Scratch() / "in" / "plain.zip", zip, fs::copy_options::overwrite_existing, ec);
        if (ec) continue;
        const ImportResult result = ImportLevelArchive(zip, user, {});
        CHECK(result.status == ImportStatus::kInstalled || result.status == ImportStatus::kAlreadyInstalled);
        CHECK(jpt_custom_level_id_valid(result.package.c_str()));
    }
    const fs::path source = Fresh("several");
    WriteTree(source / "First", Level("first"));
    WriteTree(source / "Second", Level("second"));
    CHECK(ImportLevelFolder(source, user, {}).status == ImportStatus::kSeveralPackages);
    CHECK(ImportLevelFolder(source / "First", user, {}).ok());
    const fs::path downloads = Fresh("downloads");
    WriteTree(downloads / "Third", Level("third"));
    Write(downloads / "other.zip", "PK");
    CHECK(ImportLevelFolder(downloads, user, {}).status == ImportStatus::kSeveralPackages);
    Files both = Level("north");
    const Files south = Level("south");
    both.insert(both.end(), south.begin(), south.end());
    CHECK(FromZip(user, both, "both.zip").ok());
    for (const LevelPackage& package : ListLevelPackages(user, {}))
        if (package.id == "both") CHECK(package.scenes.size() == 2 && package.scenes[0].file == "north.scn" && package.scenes[1].file == "south.scn");
}

static void TestListingAndRemoval() {
    const fs::path user = Fresh("listing"), game = Fresh("game data");
    Files lost = {{"LostJungle.scn", Scene("lostjungle.grf")}, {"LostJungle.grf", "w"}, {"LostJungle-130.pid", "p"}, {"LostJungle-130.swp", "s"},
                  {"LJ.wtd", "t"}, {"LJreadme.txt", "IF ATX IS INSTALLED"}, {"LOSTJUNGLE_MAP.bmp", "b"}};
    CHECK(FromZip(user, lost, "LostJungle_ver00-7_Release.zip").ok());
    CHECK(FromZip(user, {{"part.scn", Scene("part.grf")}, {"part.grf", "w"}, {"part-130.pid", "p"}}, "part.zip").ok());
    CHECK(FromZip(user, {{"reuse.scn", Scene("be.grf")}}, "reuse.zip").ok());
    CHECK(FromZip(user, {{"good.scn", Scene("good.grf")}, {"good.grf", "w"}, {"bad.scn", std::string(100, 'x')}}, "pair.zip").ok());
    std::vector<LevelPackage> listed = ListLevelPackages(user, game);
    CHECK(listed.size() == 4);
    if (listed.size() != 4) return;
    CHECK(listed[0].id == "LostJungle_ver00-7_Release" && listed[0].readme == "LJreadme.txt" && listed[0].mentions_atx);
    CHECK(listed[0].scenes.size() == 1 && listed[0].scenes[0].loadable && listed[0].scenes[0].missing.empty());
    CHECK(listed[1].id == "pair" && !listed[1].scenes[0].valid && listed[1].scenes[1].valid && listed[1].readme.empty());
    CHECK(listed[2].id == "part" && listed[2].scenes[0].loadable && listed[2].scenes[0].missing == std::vector<std::string>{"part-130.swp"});
    CHECK(listed[3].id == "reuse" && !listed[3].scenes[0].loadable && listed[3].scenes[0].missing == std::vector<std::string>{"be.grf"});
    Write(game / "be.GRF", "w");
    listed = ListLevelPackages(user, game);
    CHECK(listed[3].scenes[0].loadable && listed[3].scenes[0].missing.empty());

    const fs::path saves = user / "saves" / "custom_levels" / "part";
    Write(saves / "SAVEGAME.000", "save");
    std::string error;
    for (const char* id : {"", ".", "..", "../saves", "part/..", "absent", ".import-0"}) CHECK(!DeleteLevelPackage(user, id, error));
    CHECK(DeleteLevelPackage(user, "part", error));
    CHECK(!fs::exists(CustomLevelsDir(user) / "part") && Read(saves / "SAVEGAME.000") == "save");
    CHECK(ListLevelPackages(user, game).size() == 3 && Read(CustomLevelsDir(user) / "reuse" / "reuse.scn") == Scene("be.grf"));
}

static void TestReadme() {
    CHECK(ChooseReadme({"a.scn", "complex readme.txt", "Rain_storm.asa"}) == "complex readme.txt");
    CHECK(ChooseReadme({"LJreadme.txt", "LJ.wtd"}) == "LJreadme.txt");
    CHECK(ChooseReadme({"MistyJungle_readme.txt"}) == "MistyJungle_readme.txt");
    CHECK(ChooseReadme({"deep/README.txt", "changes.txt", "README.md", "INSTALL.txt"}) == "README.md");
    CHECK(ChooseReadme({"INSTALL.TXT", "notes.txt"}) == "INSTALL.TXT");
    CHECK(ChooseReadme({"instrucoes.txt"}) == "instrucoes.txt" && ChooseReadme({"LEIAME.txt"}) == "LEIAME.txt");
    CHECK(ChooseReadme({"README"}) == "README" && ChooseReadme({"read.me"}) == "read.me" && ChooseReadme({"story.txt"}) == "story.txt");
    for (const char* name : {"changes.txt", "LICENSE.txt", "credits.txt", "readme.exe", "readme.bat", "level.scn", "readme.html", "noext"})
        CHECK(ChooseReadme({name}).empty());
    CHECK(ChooseReadme({}).empty());

    CHECK(DecodeText("plain\r\ntext\rend") == "plain\ntext\nend");
    CHECK(DecodeText("\xEF\xBB\xBF" "caf\xC3\xA9") == "caf\xC3\xA9");
    CHECK(DecodeText("caf\xC3\xA9") == "caf\xC3\xA9");
    CHECK(DecodeText("caf\xE9 \x93quoted\x94") == "caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D");
    CHECK(DecodeText(std::string("\xFF\xFE" "c\0a\0f\0\xE9\0\r\0\n\0", 14)) == "caf\xC3\xA9\n");
    CHECK(DecodeText(std::string("\xFE\xFF" "\0c\0\xE9", 6)) == "c\xC3\xA9");
    CHECK(DecodeText(std::string("a\0b\x1B[31mc\td", 11)) == "ab[31mc\td");
    CHECK(DecodeText("").empty());
    const fs::path dir = Fresh("readme");
    const std::string full(300000, 'z');
    Write(dir / "long.txt", full);
    std::string text;
    CHECK(ReadLevelText(dir / "long.txt", text) && text == full);
    CHECK(!ReadLevelText(dir / "absent.txt", text) && !ReadLevelText(dir, text));
    CHECK(IsUtf8("") && !IsUtf8("\xC0\x80") && !IsUtf8("\xE2\x82"));
}

static void TestReadmeOpening() {
    const fs::path user = Fresh("readme_open"), game = Fresh("readme_open_game");
    CHECK(FromZip(user, {{"a.scn", Scene("a.grf")}, {"a.grf", "w"}, {"README.txt", "hello"}, {"tool.exe", "x"}}, "pkg.zip").ok());
    const std::vector<LevelPackage> listed = ListLevelPackages(user, game);
    CHECK(listed.size() == 1 && listed[0].readme == "README.txt");
    if (listed.size() != 1) return;
    fs::path file;
    CHECK(ResolveReadme(user, listed[0], file) && file == listed[0].dir / "README.txt");

    const fs::path outside = CustomLevelsDir(user) / "outside.txt";
    Write(outside, "x");
    Write(listed[0].dir / "readme.exe", "x");
    const auto refused = [&](const std::function<void(LevelPackage&)>& change) {
        LevelPackage package = listed[0];
        change(package);
        fs::path out;
        return !ResolveReadme(user, package, out);
    };
    CHECK(refused([](LevelPackage& p) { p.readme.clear(); }));
    CHECK(refused([](LevelPackage& p) { p.readme = "absent.txt"; }));
    CHECK(refused([](LevelPackage& p) { p.readme = "../outside.txt"; }));
    CHECK(refused([&](LevelPackage& p) { p.readme = Utf8(outside); }));
    CHECK(refused([](LevelPackage& p) { p.readme = "a.scn"; }));
    CHECK(refused([](LevelPackage& p) { p.readme = "readme.exe"; }));
    CHECK(refused([](LevelPackage& p) { p.readme = "."; }));
    CHECK(refused([&](LevelPackage& p) { p.dir = user; }));
    CHECK(refused([](LevelPackage& p) { p.id = ".."; }));
    CHECK(refused([](LevelPackage& p) { p.id = "other"; }));
}

static void TestTranslations() {
    SetLauncherLanguage("pt-BR");
    for (int status = 0; status <= int(ImportStatus::kWriteFailed); ++status) {
        const char* english = ImportStatusTitle(static_cast<ImportStatus>(status));
        CHECK(english[0] && std::string(Tr(english)) != english);
    }
    for (const char* english : {"Reading the package...", "Copying the level files...", "Checking the level files...", "Custom Levels"})
        CHECK(std::string(Tr(english)) != english);
    for (const char* english : {"not an installed package", "a file of the package is in use"}) CHECK(TrMessage(english) != english);
    SetLauncherLanguage("en");
}

static std::string Found(const char* relative) {
    char host[1024];
    return jpt_custom_level_find(relative, host, sizeof host) ? Read(fs::path(host)) : std::string("-");
}

static bool Activate(const fs::path& user, const char* package, const char* scene) {
    jpt_custom_level_request_t request{};
    char error[256] = "";
    std::snprintf(request.package, sizeof request.package, "%s", package);
    std::snprintf(request.scene, sizeof request.scene, "%s", scene);
    return jpt_custom_level_activate(user.string().c_str(), &request, error, sizeof error) != 0;
}

static void TestOverlay() {
    const fs::path user = Fresh("overlay"), packages = CustomLevelsDir(user);
    WriteTree(packages / "Lost", {{"data/LostJungle.scn", "scene"}, {"data/LostJungle.grf", "world"}, {"data/LJ.wtd", "terrain"},
                                  {"data/LostJungle-130.pid", "pid"}, {"data/LostJungle-130.swp", "swap"}, {"data/Rain_storm.asa", "rain"},
                                  {"data/anim/Door.asa", "door"}, {"data/texhw/map/x.dds", "dds"}, {"data/menu/mainscreen.ddf", "menu"},
                                  {"data/Stream.tpa", "audio"}, {"extra/Far.wtd", "far"}, {"one/same.wtd", "1"}, {"two/same.wtd", "2"},
                                  {"other/Second Level.scn", "second"}, {"other/second.grf", "second world"}});
    WriteTree(packages / "Other", {{"Secret.grf", "secret"}, {"Other.scn", "scene"}});
    CHECK(Found("LostJungle.grf") == "-");
    CHECK(!Activate(user, "Absent", "a.scn") && !Activate(user, "Lost", "absent.scn") && !Activate(user, "..", "x.scn") &&
          !Activate(user, "Lost/../Other", "Other.scn") && !jpt_custom_level_active());
    CHECK(Activate(user, "Lost", "data/LostJungle.scn"));
    CHECK(std::string(jpt_custom_level_scene()) == "LostJungle.scn" && std::string(jpt_custom_level_package()) == "Lost");
    CHECK(Found("LOSTJUNGLE.SCN") == "scene" && Found("lostjungle.grf") == "world" && Found("lj.WTD") == "terrain");
    CHECK(Found("LostJungle-130.pid") == "pid" && Found("lostjungle-130.swp") == "swap");
    CHECK(Found("anim/door.asa") == "door" && Found("anim\\Door.ASA") == "door" && Found("anim/rain_storm.asa") == "rain");
    CHECK(Found("texhw\\map\\x.dds") == "dds" && Found("Stream.tpa") == "audio" && Found("far.wtd") == "far" && Found("same.wtd") == "-");
    CHECK(Found("menu\\mainscreen.ddf") == "-" && Found("be.grf") == "-" && Found("BE.SCN") == "-" && Found("Secret.grf") == "-");
    CHECK(Found("..\\Other\\Secret.grf") == "-" && Found("../../x") == "-" && Found("") == "-" && Found("C:\\x.grf") == "-" && Found("a//b.grf") == "-");
    char host[1024];
    CHECK(jpt_custom_level_find("LostJungle-130.spz", host, sizeof host) && !fs::exists(host));
    CHECK(jpt_custom_level_find("anim/Door.asb", host, sizeof host) && !fs::exists(host));
    CHECK(!jpt_custom_level_find("be-130.spz", host, sizeof host));
    CHECK(jpt_custom_level_scene_count() == 2);
    const std::string second = jpt_custom_level_scene_at(0) == std::string("LostJungle.scn") ? jpt_custom_level_scene_at(1) : jpt_custom_level_scene_at(0);
    CHECK(second == "LEVEL2.SCN" && Found(second.c_str()) == "second" && Found("second.grf") == "second world");
    CHECK(Activate(user, "Other", "Other.scn") && Found("LostJungle.grf") == "-" && Found("secret.grf") == "secret");
    jpt_custom_level_deactivate();
    CHECK(Found("Secret.grf") == "-" && jpt_custom_level_scene_count() == 0 && !jpt_custom_level_scene()[0]);

    char name[JPT_CUSTOM_LEVEL_SCENE_CAP], saves[512];
    CHECK(jpt_custom_level_scene_name("MistyJungle.scn", 0, name, sizeof name) && std::string(name) == "MistyJungle.scn");
    CHECK(jpt_custom_level_scene_name("twelve_chars.scn", 0, name, sizeof name) && std::string(name) == "twelve_chars.scn");
    CHECK(jpt_custom_level_scene_name("thirteen_char.scn", 4, name, sizeof name) && std::string(name) == "LEVEL5.SCN");
    CHECK(jpt_custom_level_scene_name("my level.scn", 0, name, sizeof name) && std::string(name) == "LEVEL1.SCN");
    CHECK(jpt_custom_level_saves_dir("saves", "Lost", saves, sizeof saves) && fs::path(saves) == fs::path("saves") / "custom_levels" / "Lost");
    CHECK(!jpt_custom_level_saves_dir("saves", "..", saves, sizeof saves) && !jpt_custom_level_saves_dir("saves", "Lost", saves, 8));
    for (const char* id : {"", ".hidden", "a/b", "a\\b", "a:b", "trailing.", "trailing ", "..", "caf\xC3\xA9"}) CHECK(!jpt_custom_level_id_valid(id));
    CHECK(jpt_custom_level_id_valid("Some Level v1.0_x-2") && !jpt_custom_level_id_valid(std::string(49, 'a').c_str()));
}

static void TestGuestPaths() {
    const fs::path user = Fresh("guest"), game = Fresh("guest game");
    WriteTree(game, {{"Data/BE.SCN", "beach"}, {"Data/be-130.spz", "beach swap"}, {"Data/Effects.tpa", "effects"}, {"guest.img", "img"}});
    WriteTree(CustomLevelsDir(user) / "Jungle", {{"Jungle.scn", "scene"}, {"jungle.grf", "world"}, {"j.wtd", "terrain"}});
    jpt_vpath_t* v = jpt_vpath_new();
    char host[1024];
    int overlaid = 0;
    const auto read = [&](const char* guest) {
        return jpt_vpath_resolve_read(v, guest, host, sizeof host, &overlaid) == JPT_VP_OK ? Read(fs::path(host)) : std::string("-");
    };
    jpt_vpath_mount(v, 'C', game.string().c_str());
    CHECK(read("C:\\data\\jungle.grf") == "-" && !jpt_vpath_write_refused(v, "C:\\data\\x.asb"));
    CHECK(Activate(user, "Jungle", "Jungle.scn"));
    jpt_vpath_set_overlay(v, 'C', "data", jpt_custom_level_find);
    CHECK(read("C:\\data\\JUNGLE.SCN") == "scene" && overlaid && read("c:/DATA/Jungle.GRF") == "world" && overlaid);
    CHECK(read("C:\\data\\be.scn") == "beach" && !overlaid && read("data\\effects.TPA") == "effects" && !overlaid);
    CHECK(read("C:\\jungle.grf") == "-" && read("C:\\data\\..\\guest.img") == "img");
    CHECK(jpt_vpath_chdir(v, "C:\\data") && read("j.wtd") == "terrain" && overlaid && read("..\\data\\j.wtd") == "terrain");
    CHECK(read("C:\\data\\..\\..\\x") == "-");
    CHECK(jpt_vpath_resolve(v, "C:\\data\\jungle.grf", 0, host, sizeof host) == JPT_VP_NOT_FOUND);
    for (const char* path : {"C:\\data\\rain.asb", "data.asb", "C:\\DATA\\Map\\jungle", "C:/data/jungle.gop", "C:\\data"})
        CHECK(jpt_vpath_write_refused(v, path));
    for (const char* path : {"C:\\x.txt", "C:\\database\\x", "D:\\SAVEGAME.000"}) CHECK(!jpt_vpath_write_refused(v, path));
    jpt_vpath_set_overlay(v, 'C', "data", nullptr);
    CHECK(read("C:\\data\\jungle.grf") == "-");
    jpt_vpath_free(v);
    jpt_custom_level_deactivate();
}

static void TestSzdd() {
    const uint8_t packed[] = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33, 'A', 0, 7, 0, 0, 0, 0x07, 'a', 'b', 'c', 0xF0, 0xF1};
    size_t size = 0;
    uint8_t* out = jpt_szdd_expand(packed, sizeof packed, &size);
    CHECK(out && size == 7 && !std::memcmp(out, "abcabca", 7));
    std::free(out);
    CHECK(!jpt_szdd_is(packed, 10) && !jpt_szdd_expand(reinterpret_cast<const uint8_t*>("plain bytes of a file"), 21, &size));
}

#ifdef _WIN32
static void TestWindowsHost() {
    const fs::path user = Fresh("windows"), game = Fresh("windows game");
    const std::string long_name(60, 'p');
    WriteTree(game, {{"data/be.scn", "beach"}});
    const std::string swap(20000, 's');
    const std::string szdd("SZDD\x88\xF0\x27\x33" "A\0\x07\0\0\0\x07" "abc\xF0\xF1", 20);
    WriteTree(CustomLevelsDir(user) / "Jungle", {{"deep/Jungle.scn", szdd}, {"deep/jungle.grf", "world"}, {"deep/j.wtd", "terrain"}, {"deep/Jungle-130.swp", swap}});
    char out[MAX_PATH], buffer[64];
    int result = 0;
    const std::string data = (game / "data").string();
    CHECK(!custom_level_file((data + "\\jungle.grf").c_str(), out, sizeof out) && !custom_level_write_refused((data + "\\x.asb").c_str()));
    const HMODULE msvcirt = LoadLibraryA("msvcirt.dll");
    CHECK(Activate(user, "Jungle", "deep/Jungle.scn"));
    CHECK((custom_level_files_start(game.string().c_str()) != 0) == (msvcirt != nullptr));
    CHECK(custom_level_file((data + "\\JUNGLE.grf").c_str(), out, sizeof out) && Read(out) == "world");
    CHECK(custom_level_file((data + "/jungle.grf").c_str(), out, sizeof out) && Read(out) == "world");
    CHECK(!custom_level_file((data + "\\be.scn").c_str(), out, sizeof out) && !custom_level_file((game.string() + "\\jungle.grf").c_str(), out, sizeof out));
    char before[MAX_PATH];
    GetCurrentDirectoryA(sizeof before, before);
    CHECK(SetCurrentDirectoryA(data.c_str()) && custom_level_file("j.wtd", out, sizeof out) && Read(out) == "terrain");
    CHECK(custom_level_write_refused("rain.asb") && custom_level_write_refused("Map\\jungle") && !custom_level_write_refused("..\\free.txt"));
    SetCurrentDirectoryA(before);
    CHECK(!custom_level_write_refused((game.string() + "\\database\\x").c_str()) && !custom_level_write_refused((user.string() + "\\saves\\x").c_str()));

    const int scene = custom_level_lz_open((data + "\\Jungle.scn").c_str(), 1);
    CHECK(scene > 0 && custom_level_lz_read(scene, buffer, sizeof buffer, &result) && result == 7 && !std::memcmp(buffer, "abcabca", 7));
    CHECK(custom_level_lz_seek(scene, 3, 0, &result) && result == 3 && custom_level_lz_read(scene, buffer, 2, &result) && result == 2 && buffer[0] == 'a');
    CHECK(custom_level_lz_seek(scene, 0, 2, &result) && result == 7 && custom_level_lz_seek(scene, 1, 2, &result) && result < 0);
    CHECK(custom_level_lz_close(scene) && !custom_level_lz_close(scene) && !custom_level_lz_read(scene, buffer, 1, &result));
    const int image = custom_level_lz_open((data + "\\jungle-130.swp").c_str(), 1);
    CHECK(image > 0 && custom_level_lz_seek(image, 0, 2, &result) && result == 20000 && custom_level_lz_seek(image, 19998, 0, &result));
    CHECK(custom_level_lz_read(image, buffer, sizeof buffer, &result) && result == 2 && custom_level_lz_read(image, buffer, sizeof buffer, &result) && result == 0);
    CHECK(custom_level_lz_close(image));
    CHECK(custom_level_lz_open((data + "\\be.scn").c_str(), 1) == -2 && custom_level_lz_open((data + "\\Jungle.scn").c_str(), 0) == -2);
    const fs::path far_save = user / long_name / long_name / "SAVEGAME.000";
    Write(far_save, "a long path");
    const int save = custom_level_lz_open(far_save.string().c_str(), 1);
    CHECK(save > 0 && custom_level_lz_read(save, buffer, sizeof buffer, &result) && result == 11 && custom_level_lz_close(save));
    jpt_custom_level_deactivate();
    CHECK(custom_level_lz_open((far_save.string() + "x").c_str(), 1) == -1);
}
#endif

static int ReportPackages(const fs::path& dir) {
    const fs::path user = Fresh("packages");
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string source = Utf8(entry.path().filename());
        const ImportResult result = entry.is_directory() ? ImportLevelFolder(entry.path(), user, {}) : ImportLevelArchive(entry.path(), user, {});
        std::printf("%-36s %s%s%s%s [%s] %zu file(s), %zu left out\n", source.c_str(), ImportStatusTitle(result.status), result.detail.empty() ? "" : " (",
                    result.detail.c_str(), result.detail.empty() ? "" : ")", result.package.c_str(), result.files, result.skipped);
    }
    for (const LevelPackage& package : ListLevelPackages(user, {})) {
        std::printf("%s: readme \"%s\"%s%s\n", package.id.c_str(), package.readme.c_str(), package.mentions_atx ? ", mentions ATX" : "",
                    package.mentions_ce ? ", mentions Trespasser CE" : "");
        for (const LevelScene& scene : package.scenes) {
            std::printf("  %s valid %d loadable %d missing:", scene.path.c_str(), scene.valid, scene.loadable);
            for (const std::string& name : scene.missing) std::printf(" %s", name.c_str());
            const bool active = Activate(user, package.id.c_str(), scene.path.c_str());
            std::printf("; runs as %s\n", active ? jpt_custom_level_scene() : "REFUSED");
            const std::string stem = scene.file.substr(0, scene.file.size() - 4);
            for (const std::string& name : {scene.file, stem + ".grf", stem + "-130.spz", stem + "-130.swp", stem + "-130.pid", std::string("LJ.wtd"), stem + ".wtd",
                                            std::string("anim/Rain_storm.asa"), std::string("anim/anim_MayaDoor.asa")}) {
                char host[1024];
                const bool found = jpt_custom_level_find(name.c_str(), host, sizeof host) != 0;
                std::printf("    data/%-28s %s\n", name.c_str(), !found ? "original game" : fs::exists(host) ? Utf8(fs::path(host).lexically_relative(CustomLevelsDir(user))).c_str() : "hidden");
            }
        }
        std::string error;
        jpt_custom_level_deactivate();
        std::printf("  removed %d %s\n", DeleteLevelPackage(user, package.id, error), error.c_str());
    }
    std::error_code ec;
    fs::remove_all(Scratch(), ec);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--packages")) return ReportPackages(argv[2]);
    TestImportKinds();
    TestSafety();
    TestLimits();
    TestRollback();
    TestDuplicatesAndIsolation();
    TestListingAndRemoval();
    TestReadme();
    TestReadmeOpening();
    TestTranslations();
    TestOverlay();
    TestGuestPaths();
    TestSzdd();
#ifdef _WIN32
    TestWindowsHost();
#endif
    std::error_code ec;
    fs::remove_all(Scratch(), ec);
    std::printf("custom_level_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

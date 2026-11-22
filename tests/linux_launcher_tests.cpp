// The launcher's portable core on Linux, headless: the PE string-table reader that replaces Win32's resource functions, the
// disc image reader over POSIX files (Joliet names in UTF-16), the part-file writer, SHA-256 of a file, the port folder inside
// an AppImage, the version lookup with POSIX separators and the file picker probe. Scratch folders only; no window, no GPU.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "file_picker.h"
#include "game_files.h"
#include "game_install.h"
#include "iso9660.h"
#include "jpt_game_version.h"
#include "jpt_port.h"
#include "jpt_string_table.h"
#include "os_file.h"
#include "pe_resources.h"
#include "user_data.h"

namespace fs = std::filesystem;
using namespace jpt;

static int g_checks, g_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(cond)) {                                                             \
            g_failures++;                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                          \
    } while (0)

static fs::path Scratch(const char* name) {
    const char* tmp = std::getenv("TMPDIR");
    fs::path dir = fs::path(tmp && *tmp ? tmp : "/tmp") / (std::string("jpt_linux_launcher_") + name + "_" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static void Put16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}

static void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + size_t(i)] = static_cast<uint8_t>(v >> (8 * i));
}

// A PE32 file whose only section holds a resource tree: RT_STRING (6) -> one directory per block -> one language -> data.
static std::vector<uint8_t> PeWithStrings(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& blocks) {
    const size_t pe = 0x80, optional = pe + 24, sections = optional + 224, raw = 0x400, rva = 0x1000;
    const size_t type_dir = 0, name_dir = 16 + 8, lang_dirs = name_dir + 16 + 8 * blocks.size(),
                 entries = lang_dirs + blocks.size() * (16 + 8);
    size_t data = entries + blocks.size() * 16;
    std::vector<uint8_t> image(raw + data + 4096, 0);
    image[0] = 'M';
    image[1] = 'Z';
    Put32(image, 0x3C, static_cast<uint32_t>(pe));
    std::memcpy(&image[pe], "PE\0\0", 4);
    Put16(image, pe + 6, 1);
    Put16(image, pe + 20, 224);
    Put16(image, optional, 0x10B);
    Put32(image, optional + 96 + 2 * 8, static_cast<uint32_t>(rva));
    Put32(image, optional + 96 + 2 * 8 + 4, static_cast<uint32_t>(data + 4096));
    Put32(image, sections + 8, static_cast<uint32_t>(data + 4096));
    Put32(image, sections + 12, static_cast<uint32_t>(rva));
    Put32(image, sections + 16, static_cast<uint32_t>(data + 4096));
    Put32(image, sections + 20, static_cast<uint32_t>(raw));
    auto at = [&](size_t offset) { return raw + offset; };
    Put16(image, at(type_dir) + 14, 1);
    Put32(image, at(type_dir) + 16, 6);
    Put32(image, at(type_dir) + 20, 0x80000000u | static_cast<uint32_t>(name_dir));
    Put16(image, at(name_dir) + 14, static_cast<uint32_t>(blocks.size()));
    for (size_t i = 0; i < blocks.size(); ++i) {
        const size_t lang = lang_dirs + i * (16 + 8), leaf = entries + i * 16;
        Put32(image, at(name_dir) + 16 + i * 8, blocks[i].first);
        Put32(image, at(name_dir) + 16 + i * 8 + 4, 0x80000000u | static_cast<uint32_t>(lang));
        Put16(image, at(lang) + 14, 1);
        Put32(image, at(lang) + 16, 0x409);
        Put32(image, at(lang) + 20, static_cast<uint32_t>(leaf));
        Put32(image, at(leaf), static_cast<uint32_t>(rva + data));
        Put32(image, at(leaf) + 4, static_cast<uint32_t>(blocks[i].second.size()));
        std::memcpy(&image[at(data)], blocks[i].second.data(), blocks[i].second.size());
        data += (blocks[i].second.size() + 3) & ~size_t(3);
    }
    return image;
}

// One block of the string table: sixteen length-prefixed UTF-16 strings.
static std::vector<uint8_t> StringBlock(const std::vector<std::u16string>& strings) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < 16; ++i) {
        const std::u16string empty;
        const std::u16string& s = i < strings.size() ? strings[i] : empty;
        out.push_back(static_cast<uint8_t>(s.size()));
        out.push_back(static_cast<uint8_t>(s.size() >> 8));
        for (char16_t c : s) {
            out.push_back(static_cast<uint8_t>(c));
            out.push_back(static_cast<uint8_t>(c >> 8));
        }
    }
    return out;
}

struct Strings {
    std::vector<std::pair<uint32_t, std::string>> rows;
};

static void CollectString(void* user, uint32_t id, const char* bytes, uint32_t length) {
    static_cast<Strings*>(user)->rows.emplace_back(id, std::string(bytes, length));
}

static void TestPeResources() {
    const std::vector<uint8_t> pe = PeWithStrings({{1, StringBlock({u"Alpha", u"", u"Beta é"})}, {3, StringBlock({u"Gamma Ж"})}});
    const uint8_t* data = nullptr;
    size_t size = 0;
    CHECK(FindPeResource(pe, 6, 1, data, size) && size == StringBlock({u"Alpha", u"", u"Beta é"}).size());
    CHECK(FindPeResource(pe, 6, 3, data, size));
    CHECK(!FindPeResource(pe, 6, 2, data, size));
    CHECK(!FindPeResource(pe, 7, 1, data, size));
    CHECK(!FindPeResource(std::vector<uint8_t>(64, 0), 6, 1, data, size));
    CHECK(!FindPeResource(std::vector<uint8_t>{'M', 'Z'}, 6, 1, data, size));

    std::vector<uint8_t> table;
    std::string error;
    CHECK(DeriveStringTable(pe, fs::path("/nonexistent"), table, error) && error.empty());
    Strings read;
    CHECK(jpt_string_table_read(table.data(), table.size(), CollectString, &read) == 3);
    CHECK(read.rows.size() == 3);
    if (read.rows.size() == 3) {
        CHECK(read.rows[0].first == 0 && read.rows[0].second == "Alpha");
        CHECK(read.rows[1].first == 2 && read.rows[1].second == std::string("Beta \xE9"));
        CHECK(read.rows[2].first == 32 && read.rows[2].second == "Gamma ?");
    }

    std::vector<uint8_t> broken = pe;
    broken[0x3C] = 0xFF;
    broken[0x3D] = 0xFF;
    CHECK(!FindPeResource(broken, 6, 1, data, size));
    CHECK(!DeriveStringTable(std::vector<uint8_t>{'M', 'Z', 'x'}, fs::path("/nonexistent"), table, error) && !error.empty());
    CHECK(!DeriveStringTable(std::vector<uint8_t>{'n', 'o', 'p', 'e'}, fs::path("/nonexistent"), table, error) && !error.empty());
}

static void TestPartFile() {
    const fs::path dir = Scratch("part");
    const fs::path target = dir / "a.bin";
    std::error_code ec;
    {
        PartFileWriter w;
        CHECK(w.Open(target) && w.Write("hello", 5) && fs::exists(dir / "a.bin.part") && !fs::exists(target));
        CHECK(w.Commit() && fs::exists(target) && !fs::exists(dir / "a.bin.part") && fs::file_size(target, ec) == 5);
    }
    {
        PartFileWriter w;
        CHECK(w.Open(target) && w.Write("new content", 11) && w.Commit() && fs::file_size(target, ec) == 11);
    }
    {
        PartFileWriter w;
        CHECK(w.Open(target) && w.Write("dropped", 7));
    }
    CHECK(!fs::exists(dir / "a.bin.part") && fs::file_size(target, ec) == 11);
    {
        PartFileWriter w;
        CHECK(!w.Open(dir / "missing" / "b.bin"));
    }
    FileReader r;
    char buffer[8] = {};
    uint64_t bytes = 0;
    CHECK(!r.Open(dir / "none") && r.Open(target) && r.Size(bytes) && bytes == 11);
    CHECK(r.ReadAt(4, buffer, 7) && std::string(buffer, 7) == "content" && !r.ReadAt(8, buffer, 7));
    r.Close();
    CHECK(!r.IsOpen());
    fs::remove_all(dir, ec);
}

static void TestSha256() {
    const fs::path dir = Scratch("sha");
    const fs::path file = dir / "abc";
    std::error_code ec;
    std::ofstream(file, std::ios::binary) << "abc";
    CHECK(Sha256File(file) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256File(dir / "missing").empty());
    fs::remove_all(dir, ec);
}

class IsoWriter {
public:
    struct File {
        std::u16string name;
        std::string content;
    };

    // A Joliet disc with one folder level: the files sit in the root.
    static std::vector<uint8_t> Build(const std::string& volume, const std::vector<File>& files) {
        const uint32_t root_primary = 19, root_joliet = 20;
        uint32_t next = 21;
        std::vector<uint32_t> extents;
        for (const File& f : files) {
            extents.push_back(next);
            next += static_cast<uint32_t>((f.content.size() + 2047) / 2048) + (f.content.empty() ? 1 : 0);
        }
        std::vector<uint8_t> image(size_t(next) * 2048, 0);
        Descriptor(image, 16, 1, volume, root_primary, false);
        Descriptor(image, 17, 2, volume, root_joliet, true);
        image[18 * 2048] = 255;
        std::memcpy(&image[18 * 2048 + 1], "CD001", 5);
        image[18 * 2048 + 6] = 1;
        for (uint32_t joliet = 0; joliet < 2; ++joliet) {
            uint8_t* p = &image[size_t(joliet ? root_joliet : root_primary) * 2048];
            size_t pos = Record(p, std::string(1, '\0'), joliet ? root_joliet : root_primary, 2048, true);
            pos += Record(p + pos, std::string(1, '\1'), joliet ? root_joliet : root_primary, 2048, true);
            for (size_t i = 0; i < files.size(); ++i) {
                std::string name;
                if (joliet) {
                    for (char16_t c : files[i].name) {
                        name.push_back(static_cast<char>(c >> 8));
                        name.push_back(static_cast<char>(c & 0xFF));
                    }
                } else {
                    name = "FILE" + std::to_string(i) + ".BIN;1";
                }
                pos += Record(p + pos, name, extents[i], static_cast<uint32_t>(files[i].content.size()), false);
            }
        }
        for (size_t i = 0; i < files.size(); ++i) std::memcpy(&image[size_t(extents[i]) * 2048], files[i].content.data(), files[i].content.size());
        return image;
    }

private:
    static void Both32(uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            p[i] = static_cast<uint8_t>(v >> (8 * i));
            p[7 - i] = static_cast<uint8_t>(v >> (8 * i));
        }
    }

    static size_t Record(uint8_t* p, const std::string& name, uint32_t extent, uint32_t size, bool dir) {
        const size_t len = (33 + name.size() + 1) & ~size_t(1);
        p[0] = static_cast<uint8_t>(len);
        Both32(p + 2, extent);
        Both32(p + 10, size);
        p[25] = dir ? 2 : 0;
        p[28] = 1;
        p[31] = 1;
        p[32] = static_cast<uint8_t>(name.size());
        std::memcpy(p + 33, name.data(), name.size());
        return len;
    }

    static void Descriptor(std::vector<uint8_t>& image, uint32_t sector, uint8_t type, const std::string& volume, uint32_t root, bool joliet) {
        uint8_t* d = &image[size_t(sector) * 2048];
        d[0] = type;
        std::memcpy(d + 1, "CD001", 5);
        d[6] = 1;
        std::memset(d + 40, ' ', 32);
        std::memcpy(d + 40, volume.data(), volume.size());
        if (joliet) std::memcpy(d + 88, "%/E", 3);
        Record(d + 156, std::string(1, '\0'), root, 2048, true);
    }
};

static void TestIso() {
    const fs::path dir = Scratch("iso");
    std::error_code ec;
    const std::string payload(5000, 'q');
    const std::vector<uint8_t> image = IsoWriter::Build(
        "TRESPASSER", {{u"plain.txt", "one"}, {u"café.bin", payload}, {std::u16string(u"smile") + u"\xD83D\xDE00" + u".dat", "emoji"}});
    const fs::path file = dir / "disc.iso";
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    }
    IsoImage iso;
    std::string error;
    CHECK(iso.Open(file, error) && error.empty());
    CHECK(iso.joliet() && iso.volume_id() == "TRESPASSER" && iso.files().size() == 3);
    CHECK(iso.Find("plain.txt") && iso.Find("PLAIN.TXT") && iso.Find("caf\xC3\xA9.bin"));
    CHECK(iso.Find("smile\xF0\x9F\x98\x80.dat"));
    std::vector<uint8_t> bytes;
    const IsoFile* cafe = iso.Find("caf\xC3\xA9.bin");
    CHECK(cafe && iso.ReadAll(*cafe, bytes, error) && std::string(bytes.begin(), bytes.end()) == payload);
    uint64_t seen = 0;
    std::string sha;
    const fs::path out = dir / "out.bin";
    CHECK(cafe && iso.Extract(*cafe, out, [&](uint64_t n) { seen += n; return true; }, error, &sha) && seen == payload.size());
    CHECK(sha.size() == 64 && fs::file_size(out, ec) == payload.size() && !fs::exists(dir / "out.bin.part"));
    CHECK(cafe && !iso.Extract(*cafe, dir / "cancelled.bin", [](uint64_t) { return false; }, error) && error == "cancelled" &&
          !fs::exists(dir / "cancelled.bin") && !fs::exists(dir / "cancelled.bin.part"));
    CHECK(cafe && !iso.Extract(*cafe, dir / "missing" / "x.bin", nullptr, error) && !error.empty());

    IsoImage none;
    CHECK(!none.Open(dir / "nothing.iso", error) && !error.empty());
    {
        std::ofstream(dir / "junk.iso", std::ios::binary) << std::string(40000, 'x');
    }
    IsoImage junk;
    CHECK(!junk.Open(dir / "junk.iso", error));
    fs::remove_all(dir, ec);
}

static void SetEnv(const char* name, const char* value) {
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}

static void TestPortFolder() {
    const fs::path dir = Scratch("port");
    const std::string saved = std::getenv("APPIMAGE") ? std::getenv("APPIMAGE") : "";
    const fs::path image = dir / "Games" / "JPT.AppImage";
    SetEnv("APPIMAGE", image.c_str());
    CHECK(PortFolder() == dir / "Games");
    UserDataRequest request;
    request.port_folder = PortFolder();
    std::error_code ec;
    fs::create_directories(dir / "Games", ec);
    const UserDataSelection chosen = SelectUserData(request);
    CHECK(chosen.outcome == UserDataOutcome::kPortable && chosen.root == dir / "Games" / "user_data");
    CHECK(fs::is_directory(chosen.root, ec));
    SetEnv("APPIMAGE", "");
    CHECK(PortFolder() == fs::read_symlink("/proc/self/exe", ec).parent_path());
    if (saved.empty()) unsetenv("APPIMAGE");
    else setenv("APPIMAGE", saved.c_str(), 1);
    fs::remove_all(dir, ec);
}

static void Touch(const fs::path& file) {
    std::ofstream(file) << "x";
}

static void TestVersions() {
    const fs::path dir = Scratch("versions");
    std::error_code ec;
    const std::string root = dir.string();
    char out[1024];
    CHECK(jpt_game_version_folder(root.c_str(), jpt_game_version_by_id("en-116"), out, sizeof out) &&
          std::string(out) == root + "/versions/en-116");
    CHECK(jpt_game_version_saves_dir("U", "U/saves", jpt_game_version_by_id("en-116"), out, sizeof out) && std::string(out) == "U/saves");
    CHECK(jpt_game_version_saves_dir("U", "U/saves", jpt_game_version_by_id("ru-116"), out, sizeof out) &&
          std::string(out) == "U/versions/ru-116/saves");
    CHECK(!jpt_game_version_unavailable_here(jpt_game_version_by_id("en-116")) && !jpt_game_version_unavailable_here(jpt_game_version_by_id("ru-116")));
    CHECK(jpt_game_version_unavailable_here(jpt_game_version_by_id("es-117")) && jpt_game_version_unavailable_here(jpt_game_version_by_id("ptbr-117")));
    const jpt_game_version_t* chosen = nullptr;
    CHECK(!jpt_game_version_resolve(root.c_str(), "", 1, out, sizeof out, &chosen));
    for (const char* id : {"es-117", "ru-116"}) {
        fs::create_directories(dir / "versions" / id, ec);
        Touch(dir / "versions" / id / JPT_GUEST_IMAGE);
    }
    CHECK(jpt_game_version_resolve(root.c_str(), "es-117", 1, out, sizeof out, &chosen) && chosen == jpt_game_version_by_id("ru-116"));
    CHECK(jpt_game_version_resolve(root.c_str(), "ru-116", 1, out, sizeof out, &chosen) && chosen == jpt_game_version_by_id("ru-116"));
    CHECK(std::string(out) == root + "/versions/ru-116");
    fs::remove_all(dir, ec);
}

static void TestFilePicker() {
    const fs::path dir = Scratch("picker");
    const std::string saved = std::getenv("PATH") ? std::getenv("PATH") : "";
    std::error_code ec;
    setenv("PATH", dir.c_str(), 1);
    CHECK(!FilePickerAvailable());
    Touch(dir / "zenity");
    CHECK(!FilePickerAvailable());
    fs::permissions(dir / "zenity", fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add, ec);
    CHECK(FilePickerAvailable());
    fs::remove(dir / "zenity", ec);
    Touch(dir / "kdialog");
    fs::permissions(dir / "kdialog", fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add, ec);
    CHECK(FilePickerAvailable());
    setenv("PATH", "", 1);
    CHECK(!FilePickerAvailable());
    setenv("PATH", saved.c_str(), 1);
    fs::remove_all(dir, ec);
}

int main() {
    TestPeResources();
    TestPartFile();
    TestSha256();
    TestIso();
    TestPortFolder();
    TestVersions();
    TestFilePicker();
    std::printf("linux_launcher_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

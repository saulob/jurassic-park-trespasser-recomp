// The launcher's portable core on Linux, headless: the PE string-table reader that replaces Win32's resource functions, the
// disc image reader over POSIX files (Joliet names in UTF-16), the part-file writer, SHA-256 of a file, the port folder inside
// an AppImage, the version lookup with POSIX separators, a game folder imported with another folder completing it and the file
// picker probe. Scratch folders only; no window, no GPU.
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "displays.h"
#include "file_browser.h"
#include "file_picker.h"
#include "game_files.h"
#include "game_install.h"
#include "incident_report.h"
#include "iso9660.h"
#include "jpt_game_version.h"
#include "jpt_hash.h"
#include "jpt_port.h"
#include "jpt_string_table.h"
#include "options.h"
#include "os_file.h"
#include "pe_resources.h"
#include "trg/layout.h"
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
    CHECK(jpt_game_version_playable(jpt_game_version_by_id("en-116")) && jpt_game_version_playable(jpt_game_version_by_id("ru-116")) &&
          jpt_game_version_playable(jpt_game_version_by_id("es-117")) && jpt_game_version_playable(jpt_game_version_by_id("ptbr-117")));
    const jpt_game_version_t* chosen = nullptr;
    CHECK(!jpt_game_version_resolve(root.c_str(), "", 1, out, sizeof out, &chosen));
    for (const char* id : {"en-116", "ru-116", "es-117", "ptbr-117"}) {
        fs::create_directories(dir / "versions" / id, ec);
        Touch(dir / "versions" / id / JPT_GUEST_IMAGE);
    }
    for (const char* id : {"en-116", "ru-116", "es-117", "ptbr-117"})
        CHECK(jpt_game_version_resolve(root.c_str(), id, 1, out, sizeof out, &chosen) && chosen == jpt_game_version_by_id(id));
    CHECK(jpt_game_version_resolve(root.c_str(), "unknown", 1, out, sizeof out, &chosen) && chosen == jpt_game_version_by_id("en-116"));
    fs::remove_all(dir, ec);
}

static std::string ShaOf(const std::string& text) {
    char hex[65];
    jpt_sha256_buffer_hex(text.data(), text.size(), hex);
    return hex;
}

static void WriteText(const fs::path& file, const std::string& text) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << text;
}

static std::string ReadText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// An installation and the disc copied to a folder, with names in other cases and both read only: recognised by their files,
// the one completing the other, copied without changing either.
static void TestFolderInstall() {
    const fs::path dir = Scratch("folders");
    std::error_code ec;
    const std::pair<const char*, const char*> paths[] = {
        {"setup/tpassp6.exe", JPT_GUEST_IMAGE}, {"setup/MSVCIRT.DLL", "MSVCIRT.DLL"}, {"data/be.scn", "data/be.scn"}, {"data/menu/loader.ddf", "data/menu/loader.ddf"}};
    const std::string contents[] = {"guest image", "runtime", "level", "identity"};
    std::vector<std::string> digests;
    std::vector<InstallFile> files;
    for (const std::string& content : contents) digests.push_back(ShaOf(content));
    for (size_t i = 0; i < 4; ++i)
        files.push_back({SourceKind::kDisc, paths[i].first, paths[i].second, contents[i].size(), digests[i].c_str(), i == 0 ? FileRole::kGuestImage : FileRole::kData});
    const DiscIdentity discs[] = {{"Folder test", "TEST"}};
    const GameProfile profiles[] = {{&jpt_game_versions[0], "data/menu/loader.ddf", digests[3].c_str(), discs, nullptr, files}};

    const fs::path installed = dir / "Trespasser Install", disc = dir / "CD Copy", root = dir / "game";
    WriteText(installed / "TRESPASS.EXE", "guest image");
    WriteText(installed / "msvcirt.dll", "runtime");
    WriteText(installed / "MENU" / "LOADER.DDF", "identity");
    WriteText(disc / "setup" / "tpassp6.exe", "guest image");
    WriteText(disc / "setup" / "MSVCIRT.DLL", "runtime");
    WriteText(disc / "data" / "be.scn", "level");
    WriteText(disc / "data" / "menu" / "loader.ddf", "identity");
    for (const fs::path& top : {installed, disc})
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(top))
            if (e.is_regular_file()) fs::permissions(e.path(), fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove, ec);

    InstallResult r = InstallFromFolders({installed}, root, InstallHooks{}, profiles);
    CHECK(r.status == InstallStatus::kIncompleteInstallation && r.missing == std::vector<std::string>{"data/be.scn"} && !fs::exists(root));
    r = InstallFromFolders({installed, disc}, root, InstallHooks{}, profiles);
    const fs::path target = root / "versions" / "en-116";
    CHECK(r.status == InstallStatus::kInstalled && r.profile == &profiles[0] && r.dir == target);
    CHECK(ReadText(target / JPT_GUEST_IMAGE) == "guest image" && ReadText(target / "data" / "be.scn") == "level" && !fs::exists(target / "TRESPASS.EXE"));
    CHECK(ReadText(installed / "TRESPASS.EXE") == "guest image" && ReadText(disc / "data" / "be.scn") == "level");
    CHECK(InstallFromFolders({disc}, root, InstallHooks{}, profiles).status == InstallStatus::kUpToDate);
    CHECK(InstallFromFolders({dir / "absent"}, root, InstallHooks{}, profiles).status == InstallStatus::kNotAnInstallation);
    for (const fs::path& top : {installed, disc})
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(top))
            if (e.is_regular_file()) fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, ec);
    fs::remove_all(dir, ec);
}

static void TestVersionCompleteness() {
    const fs::path base = Scratch("completeness") / "Jurassic Park Trespasser" / "game";
    std::error_code ec;
    for (const GameProfile& p : KnownProfiles()) {
        bool has_intro = false;
        for (const InstallFile& f : p.files)
            if (std::strcmp(f.installed, "data/menu/tpassintro.smk") == 0) has_intro = true;
        CHECK(has_intro);
    }

    const GameProfile* en = ProfileById(KnownProfiles(), "en-116");
    CHECK(en != nullptr);
    const fs::path ver_dir = base / JPT_VERSIONS_DIR / "en-116";
    fs::create_directories(ver_dir, ec);
    Touch(ver_dir / JPT_GUEST_IMAGE);
    {
        const GameCheck incomplete = CheckGameFolder(ver_dir, *en);
        CHECK(incomplete.state == GameState::kInvalid);
        const std::vector<InstalledVersion> found = FindInstalledVersions(base);
        CHECK(found.size() == 1 && found[0].check.state == GameState::kInvalid);
        CHECK(ChooseVersion(found, "en-116") == nullptr);
    }

    for (const InstallFile& f : en->files) {
        const fs::path target = ver_dir / f.installed;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary);
        if (f.size > 0) {
            out.seekp(static_cast<std::streamoff>(f.size - 1));
            out.put('\0');
        }
    }
    {
        const GameCheck ready = CheckGameFolder(ver_dir, *en);
        CHECK(ready.state == GameState::kReady);
        const std::vector<InstalledVersion> found = FindInstalledVersions(base);
        const InstalledVersion* chosen = ChooseVersion(found, "en-116");
        CHECK(chosen != nullptr && std::strcmp(chosen->profile->id(), "en-116") == 0);
    }

    fs::remove(ver_dir / "data/menu/tpassintro.smk", ec);
    {
        const GameCheck missing_intro = CheckGameFolder(ver_dir, *en);
        CHECK(missing_intro.state == GameState::kInvalid);
        CHECK(missing_intro.detail == "missing data/menu/tpassintro.smk");
        const std::vector<InstalledVersion> found = FindInstalledVersions(base);
        CHECK(ChooseVersion(found, "en-116") == nullptr);
    }
    fs::remove_all(base.parent_path().parent_path(), ec);
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

static void TestFileBrowser() {
    const fs::path dir = Scratch("browser");
    std::error_code ec;
    fs::create_directories(dir / "BetaDir", ec);
    fs::create_directories(dir / "alphaDir", ec);
    fs::create_directories(dir / ".hiddenDir", ec);
    Touch(dir / "trespasser.ISO");
    Touch(dir / "notes.txt");
    Touch(dir / ".secret.iso");

    const FolderListing iso_only = ListFolder(dir, ".iso", false);
    CHECK(iso_only.readable && iso_only.entries.size() == 3);
    if (iso_only.entries.size() == 3) {
        CHECK(iso_only.entries[0].directory && iso_only.entries[0].name == "alphaDir");
        CHECK(iso_only.entries[1].directory && iso_only.entries[1].name == "BetaDir");
        CHECK(!iso_only.entries[2].directory && iso_only.entries[2].name == "trespasser.ISO");
    }
    const FolderListing all_visible = ListFolder(dir, "", false);
    CHECK(all_visible.readable && all_visible.entries.size() == 4);
    const FolderListing with_hidden = ListFolder(dir, ".iso", true);
    CHECK(with_hidden.readable && with_hidden.entries.size() == 5);
    CHECK(StartingFolder({{"Empty", dir / "alphaDir"}, {"Discs", dir}}, ".iso") == dir);
    CHECK(FormatSize(512) == "1 KB" && FormatSize(2ull << 20) == "2.0 MB" && FormatSize(2ull << 30) == "2.0 GB");

    CHECK(!UseBuiltInFileChooser(true, false, true, ""));
    CHECK(!UseBuiltInFileChooser(false, false, true, ""));
    CHECK(UseBuiltInFileChooser(false, false, false, ""));
    CHECK(UseBuiltInFileChooser(false, true, true, ""));
    CHECK(UseBuiltInFileChooser(false, false, true, "builtin"));
    CHECK(!UseBuiltInFileChooser(false, true, false, "native"));
    fs::remove_all(dir, ec);
}

static void TestSteamDeck1280x800() {
    const int scales[] = {100, 150, 225};
    for (int pct : scales) {
        const float scale = float(pct) / 100.0f;
        Display deck;
        deck.bounds = {0, 0, int(1280 * scale), int(800 * scale)};
        deck.usable = {0, 0, int(1280 * scale), int(760 * scale)};
        deck.scale = scale;
        int w = int(1280 * scale), h = int(720 * scale);
        FitWindowSize(w, h, deck);
        CHECK(w > 0 && w <= deck.usable.w && h > 0 && h <= deck.usable.h);
    }
    const float header = trg::layout::HeaderHeight(800.0f, 1.0f);
    const float body = 800.0f - header - trg::layout::kBodyGap - trg::layout::kFooterHeight - trg::layout::kMargin;
    const int page_count = static_cast<int>(std::size(kPageOrder));
    CHECK(trg::layout::SidebarHeight(page_count) <= body);
    CHECK(trg::layout::SidebarWidth(1280.0f, 1.0f) + 600.0f <= 1280.0f);
    jpt_monitor_t m{};
    m.bounds = {0, 0, 1280, 800};
    m.work = {0, 0, 1280, 800};
    CHECK(WindowSizeFits(jpt_window_sizes[0], m) && WindowSizeFits(jpt_window_sizes[1], m));
    CHECK(!WindowSizeFits(jpt_window_sizes[3], m));
    CHECK(FormatLinuxOperatingSystem("SteamOS", "Valve", "Jupiter") == "Steam Deck: SteamOS");
    CHECK(FormatLinuxOperatingSystem("SteamOS", "Valve", "Galileo") == "Steam Deck: SteamOS");
    CHECK(FormatLinuxOperatingSystem("Ubuntu 24.04", "LENOVO", "ThinkPad") == "Linux: Ubuntu 24.04");
}

static int ImportDirectory(const fs::path& images, const fs::path& game_root) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(images, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (extension == ".iso") found.push_back(entry.path());
    }
    if (ec || found.empty()) {
        std::fprintf(stderr, "import: no disc images in %s\n", images.string().c_str());
        return 2;
    }
    std::sort(found.begin(), found.end());
    int failed = 0;
    for (const fs::path& iso : found) {
        IsoImage image;
        std::string why;
        if (image.Open(iso, why)) {
            const DiscMatch match = IdentifyDisc(image, KnownProfiles(), why);
            if (match.profile && fs::is_regular_file(VersionFolder(game_root, *match.profile) / JPT_GUEST_IMAGE)) {
                std::printf("IMPORT|%s|Already installed|%s|%s is already installed\n", iso.filename().string().c_str(),
                            match.profile->id(), match.profile->label());
                continue;
            }
        }
        InstallHooks hooks;
        hooks.phase = [](const char* phase) { std::fprintf(stderr, "  %s\n", phase); };
        hooks.log = [](const std::string& line) { std::fprintf(stderr, "  %s\n", line.c_str()); };
        const InstallResult result = InstallFromIso(iso, game_root, hooks);
        std::printf("IMPORT|%s|%s|%s|%s\n", iso.filename().string().c_str(), InstallStatusTitle(result.status),
                    result.profile ? result.profile->id() : "-", result.detail.c_str());
        if (result.ok()) continue;
        if (result.status == InstallStatus::kWrongDisc || result.status == InstallStatus::kUnsupportedBuild ||
            result.status == InstallStatus::kNotAnImage)
            continue;
        failed = 1;
    }
    return failed;
}

int main(int argc, char** argv) {
    if (argc == 4 && !std::strcmp(argv[1], "--import-dir")) return ImportDirectory(argv[2], argv[3]);
    if (argc != 1 && !(argc == 2 && !std::strcmp(argv[1], "game"))) {
        std::fprintf(stderr, "usage: linux_launcher_tests [--import-dir IMAGE_DIR GAME_DIR]\n");
        return 2;
    }
    TestPeResources();
    TestPartFile();
    TestSha256();
    TestIso();
    TestPortFolder();
    TestVersions();
    TestVersionCompleteness();
    TestFolderInstall();
    TestFilePicker();
    TestFileBrowser();
    TestSteamDeck1280x800();
    std::printf("linux_launcher_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

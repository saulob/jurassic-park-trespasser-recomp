// Updater tests: semantic versions, release parsing, SHA-256, ZIP path safety, staging validation and rollback.
// Run: update_tests

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "update_config.h"
#include "updater/github_release.h"
#include "updater/install_transaction.h"
#include "updater/json.h"
#include "updater/sha256.h"
#include "updater/third_party/miniz/miniz.h"
#include "updater/update_job.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"
#include "updater/update_zip.h"
#include "updater/url_policy.h"
#include "updater/version.h"

using namespace trg::update;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("update_tests-" + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(content.data(), std::streamsize(content.size()));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string MakeZip(const std::vector<std::pair<std::string, std::string>>& entries) {
  mz_zip_archive archive{};
  if (mz_zip_writer_init_heap(&archive, 0, 0) == MZ_FALSE) {
    return {};
  }
  for (const auto& [name, content] : entries) {
    if (mz_zip_writer_add_mem(&archive, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION) ==
        MZ_FALSE) {
      mz_zip_writer_end(&archive);
      return {};
    }
  }
  void* data = nullptr;
  size_t size = 0;
  if (mz_zip_writer_finalize_heap_archive(&archive, &data, &size) == MZ_FALSE) {
    mz_zip_writer_end(&archive);
    return {};
  }
  std::string result(static_cast<const char*>(data), size);
  mz_free(data);
  mz_zip_writer_end(&archive);
  return result;
}

void TestVersions() {
  std::printf("semantic versions\n");
  const auto v060 = ParseVersion("v0.6.0");
  const auto v060_plain = ParseVersion("0.6.0");
  const auto v0610 = ParseVersion("0.6.10");
  const auto v070 = ParseVersion("0.7.0");
  const auto v100 = ParseVersion("1.0.0");
  CHECK(v060 && v060_plain && v0610 && v070 && v100);
  CHECK(*v060 == *v060_plain);
  CHECK(!(*v0610 < *v060));
  CHECK(*v060 < *v0610);
  CHECK(*v0610 < *v070);
  CHECK(*v070 < *v100);
  CHECK(v060->CoreString() == "0.6.0");
  CHECK(v100->ToString() == "1.0.0");

  const auto rc = ParseVersion("0.3.2-rc1");
  const auto rc2 = ParseVersion("0.3.2-rc.2");
  const auto rc10 = ParseVersion("0.3.2-rc.10");
  const auto stable = ParseVersion("0.3.2");
  CHECK(rc && rc2 && rc10 && stable);
  CHECK(*rc < *stable);
  CHECK(*rc2 < *rc10);
  const auto with_build = ParseVersion("0.3.2+abc");
  CHECK(with_build && with_build->build == "abc");
  CHECK(with_build && SemanticVersion::Compare(*with_build, *stable) == 0);
  CHECK(ParseVersion("0.3.2-rc1+build.5")->prerelease.size() == 1);

  CHECK(!ParseVersion("1.0"));
  CHECK(!ParseVersion("1.0.0.0"));
  CHECK(!ParseVersion("01.0.0"));
  CHECK(!ParseVersion("1.0.0-"));
  CHECK(!ParseVersion("v"));
  CHECK(ParseVersion("V0.6.1").has_value());
}

void TestReleases() {
  std::printf("release JSON\n");
  constexpr const char* kJson = R"({
    "tag_name": "v0.7.0",
    "draft": false,
    "prerelease": false,
    "html_url": "https://github.com/saulob/jurassic-park-trespasser-recomp/releases/tag/v0.7.0",
    "assets": [
      {"name": "JurassicParkTrespasserRecompiled-v0.7.0-windows-x86.zip",
       "browser_download_url": "https://github.com/example/release.zip",
       "size": 123456,
       "digest": "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
      {"name": "JurassicParkTrespasserRecompiled-v0.7.0-windows-x86.zip.sha256",
       "browser_download_url": "https://github.com/example/release.zip.sha256",
       "size": 107,
       "digest": "sha256:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"}
    ]
  })";

  std::string error;
  const std::optional<Release> release = ParseRelease(kJson, &error);
  CHECK(release.has_value());
  CHECK(error.empty());
  if (!release) {
    return;
  }
  CHECK(release->tag == "v0.7.0");
  CHECK(release->version.CoreString() == "0.7.0");
  CHECK(IsInstallableRelease(*release));
  CHECK(release->html_url.rfind("https://", 0) == 0);
  CHECK(release->assets.size() == 2);
  const ReleaseAsset* asset = FindAsset(*release, "JurassicParkTrespasserRecompiled-v0.7.0-windows-x86.zip");
  CHECK(asset != nullptr);
  CHECK(asset && asset->size == 123456);
  CHECK(asset && asset->sha256() ==
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK(FindAsset(*release, "JurassicParkTrespasserRecompiled-v0.7.1-windows-x86.zip") == nullptr);
  CHECK(FindAsset(*release, "JurassicParkTrespasserRecompile-v0.7.0-windows-x86.zip") == nullptr);
  CHECK(FindAsset(*release, "jurassicparktrespasserrecompiled-v0.7.0-windows-x86.zip") == nullptr);
  CHECK(FindAsset(*release, "JurassicParkTrespasserRecompiled-v0.7.0-windows-x86.7z") == nullptr);

  const std::optional<Release> draft = ParseRelease(R"({
    "tag_name":"v0.7.1","draft":true,"prerelease":false,"assets":[]
  })");
  CHECK(draft && !IsInstallableRelease(*draft));
  const std::optional<Release> prerelease = ParseRelease(R"({
    "tag_name":"v0.7.1-rc1","draft":false,"prerelease":true,"assets":[]
  })");
  CHECK(prerelease && !IsInstallableRelease(*prerelease));
  const std::optional<Release> prerelease_tag = ParseRelease(R"({
    "tag_name":"v0.7.1-rc1","draft":false,"prerelease":false,"assets":[]
  })");
  CHECK(prerelease_tag && !IsInstallableRelease(*prerelease_tag));

  CHECK(!ParseRelease(R"({"tag_name": 42})").has_value());
  CHECK(!ParseRelease(R"({"tag_name": "not-a-version"})").has_value());
  CHECK(!ParseRelease("[]").has_value());
  CHECK(!ParseRelease("{").has_value());
  CHECK(!ParseRelease(R"({"tag_name":"v0.7.0","assets":{}})").has_value());
  CHECK(!ParseRelease(R"({"tag_name":"v0.7.0","assets":[{"name":"x"}]})").has_value());

  CHECK(ParseSha256Digest("sha256:abc") == std::nullopt);
  CHECK(ParseSha256Digest("md5:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == std::nullopt);
  CHECK(ParseSha256Digest("SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") ==
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK(ParseSha256File("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789  "
                        "JurassicParkTrespasserRecompiled-v0.7.0-windows-x86.zip\n") ==
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
  CHECK(ParseSha256File("not-a-hash  JurassicParkTrespasserRecompiled-v0.7.0-windows-x86.zip") == std::nullopt);
}

void TestSha256() {
  std::printf("SHA-256\n");
  CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string long_text(1000000, 'a');
  CHECK(Sha256Hex(long_text) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(Sha256Hex("The quick brown fox jumps over the lazy dog") ==
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  CHECK(Sha256Hex("tampered") != Sha256Hex("original"));

  TempDir temp;
  const std::filesystem::path file = temp.path / "hash.bin";
  WriteFile(file, "abc");
  std::string error;
  CHECK(Sha256FileHex(file, error) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(error.empty());
}

void TestZipAndStaging() {
  std::printf("ZIP extraction and staging\n");
  TempDir temp;
  std::string error;

  const std::filesystem::path wrapped_archive = temp.path / "wrapped.zip";
  const std::string wrapped = MakeZip({
      {"JurassicParkTrespasserRecompiled-v0.7.0-windows-x86/trespasser_recomp.exe", "launcher"},
      {"JurassicParkTrespasserRecompiled-v0.7.0-windows-x86/assets/launcher/header-art.png", "art"},
      {"JurassicParkTrespasserRecompiled-v0.7.0-windows-x86/licenses/THIRD-PARTY-LICENSES.txt", "license"},
  });
  CHECK(!wrapped.empty());
  WriteFile(wrapped_archive, wrapped);
  const std::filesystem::path wrapped_staging = temp.path / "wrapped-staging";
  CHECK(ExtractZip(wrapped_archive, wrapped_staging, {}, error));
  std::filesystem::path wrapped_root;
  CHECK(ResolveStagingRoot(wrapped_staging, {"trespasser_recomp.exe"}, wrapped_root, error));
  CHECK(wrapped_root == wrapped_staging / "JurassicParkTrespasserRecompiled-v0.7.0-windows-x86");
  CHECK(ReadFile(wrapped_root / "trespasser_recomp.exe") == "launcher");
  CHECK(ValidateStaging(wrapped_root, {"trespasser_recomp.exe", "assets/launcher/header-art.png"}, error));
  CHECK(ListStagingFiles(wrapped_root, error).size() == 3);

  const std::filesystem::path flat_archive = temp.path / "flat.zip";
  WriteFile(flat_archive, MakeZip({{"trespasser_recomp.exe", "launcher"},
                                   {"assets/launcher/header-art.png", "art"},
                                   {"licenses/THIRD-PARTY-LICENSES.txt", "license"}}));
  const std::filesystem::path flat_staging = temp.path / "flat-staging";
  CHECK(ExtractZip(flat_archive, flat_staging, {}, error));
  std::filesystem::path flat_root;
  CHECK(ResolveStagingRoot(flat_staging, {"trespasser_recomp.exe"}, flat_root, error));
  CHECK(flat_root == flat_staging);

  WriteFile(flat_root / "game" / "be.scn", "game");
  CHECK(!ValidateStaging(flat_root, {"trespasser_recomp.exe"}, error));
  std::filesystem::remove_all(flat_root / "game");

  WriteFile(flat_root / "trespasser.ini", "settings");
  CHECK(!ValidateStaging(flat_root, {"trespasser_recomp.exe"}, error));
  std::filesystem::remove(flat_root / "trespasser.ini");

  WriteFile(flat_root / "logs" / "a.log", "log");
  CHECK(!ValidateStaging(flat_root, {"trespasser_recomp.exe"}, error));
  std::filesystem::remove_all(flat_root / "logs");

  CHECK(!ValidateStaging(flat_root, {"missing.exe"}, error));

  const std::vector<std::string> unsafe_names = {
      "../evil",        "C:/evil",         "C:\\evil",       "\\\\server\\share", "a/../../b",
      "a\\..\\b",       "a//b",            "game/data/be.scn", "user/trespasser.ini",      "user_data/settings",
      "logs/a.log",     "cache/a.spv",     "saves/SAVEGAME.000", "versions/en-116/data/be.scn", "previews/a.png",
      "trespasser.ini", "recomp-registry.txt", "disc.iso",      "game.zip",          "a/b.",
      "con",            "C:evil",
  };
  int rejected = 0;
  for (const std::string& name : unsafe_names) {
    const std::filesystem::path unsafe_archive = temp.path / "unsafe.zip";
    const std::string bytes = MakeZip({{name, "payload"}});
    CHECK(!bytes.empty());
    WriteFile(unsafe_archive, bytes);
    const std::filesystem::path unsafe_staging = temp.path / "unsafe-staging";
    std::filesystem::remove_all(unsafe_staging);
    if (!ExtractZip(unsafe_archive, unsafe_staging, {}, error)) {
      ++rejected;
    }
    CHECK(!std::filesystem::exists(temp.path / "evil"));
  }
  CHECK(rejected == int(unsafe_names.size()));
  CHECK(IsSymlinkZipEntry(0120000u << 16));
  CHECK(!IsSymlinkZipEntry(0100644u << 16));
  CHECK(!IsSymlinkZipEntry(0));

  const std::filesystem::path two_folders = temp.path / "two-folders.zip";
  WriteFile(two_folders, MakeZip({{"one/trespasser_recomp.exe", "one"}, {"two/trespasser_recomp.exe", "two"}}));
  const std::filesystem::path two_staging = temp.path / "two-staging";
  CHECK(ExtractZip(two_folders, two_staging, {}, error));
  std::filesystem::path two_root;
  CHECK(!ResolveStagingRoot(two_staging, {"trespasser_recomp.exe"}, two_root, error));

  const std::filesystem::path dot_archive = temp.path / "dot.zip";
  WriteFile(dot_archive, MakeZip({{"./trespasser_recomp.exe", "launcher"}}));
  const std::filesystem::path dot_staging = temp.path / "dot-staging";
  CHECK(ExtractZip(dot_archive, dot_staging, {}, error));
  CHECK(std::filesystem::exists(dot_staging / "trespasser_recomp.exe"));
}

std::filesystem::path MakeJob(UpdateJob& job, const std::filesystem::path& temp, const std::string& manifest_text) {
  const std::filesystem::path install = temp / "install dir";
  const std::filesystem::path staging = temp / "staging dir";
  const std::filesystem::path backup = temp / "backup dir";
  const std::filesystem::path manifest = temp / "manifest.txt";
  std::filesystem::create_directories(install);
  std::filesystem::create_directories(staging);
  WriteFile(manifest, manifest_text);
  job = {};
  job.parent_pid = 1234;
  job.install_dir = install;
  job.staging_dir = staging;
  job.backup_dir = backup;
  job.manifest_file = manifest;
  job.restart_executable = install / "trespasser_recomp.exe";
  job.log_file = temp / "updater.log";
  return install;
}

void TestJobs() {
  std::printf("update jobs\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "trespasser_recomp.exe\n");
  WriteFile(install / "trespasser_recomp.exe", "old launcher");
  WriteFile(job.staging_dir / "trespasser_recomp.exe", "launcher");
  job.restart_arguments = {"--user=with space", "--launcher", ""};

  std::string error;
  const std::filesystem::path job_file = temp.path / "job.txt";
  CHECK(WriteUpdateJob(job_file, job, error));
  const std::optional<UpdateJob> read = ReadUpdateJob(job_file, error);
  CHECK(read.has_value());
  if (read) {
    CHECK(read->parent_pid == 1234);
    CHECK(read->install_dir == install);
    CHECK(read->restart_arguments == job.restart_arguments);
  }

  CHECK(ValidateInstallJob(job, error));

  UpdateJob inside = job;
  inside.staging_dir = install / "staging";
  std::filesystem::create_directories(inside.staging_dir);
  CHECK(!ValidateInstallJob(inside, error));

  UpdateJob relative = job;
  relative.manifest_file = "manifest.txt";
  CHECK(!ValidateInstallJob(relative, error));

  UpdateJob outside = job;
  outside.restart_executable = temp.path / "elsewhere.exe";
  CHECK(!ValidateInstallJob(outside, error));

  UpdateJob no_manifest = job;
  no_manifest.manifest_file = temp.path / "missing.txt";
  CHECK(!ValidateInstallJob(no_manifest, error));
}

void TestManifest() {
  std::printf("manifest validation\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "../evil\n");
  WriteFile(install / "trespasser_recomp.exe", "old launcher");
  WriteFile(job.staging_dir / "trespasser_recomp.exe", "new launcher");
  std::string error;
  InstallOutcome outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);
  CHECK(ReadFile(install / "trespasser_recomp.exe") == "old launcher");

  WriteFile(job.manifest_file, "user/trespasser.ini\n");
  WriteFile(job.staging_dir / "user" / "trespasser.ini", "replacement");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);
  CHECK(ReadFile(install / "trespasser_recomp.exe") == "old launcher");

  WriteFile(job.manifest_file, "missing.dll\n");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);

  WriteFile(job.manifest_file, "");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);

  WriteFile(job.manifest_file, "trespasser_recomp.exe\nrexruntime.dll\ntrespasser_recomp.exe\n");
  WriteFile(job.staging_dir / "rexruntime.dll", "new runtime");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "trespasser_recomp.exe") == "new launcher");
  CHECK(ReadFile(install / "rexruntime.dll") == "new runtime");
}

void TestSameVersionReplacement() {
  std::printf("same-version replacement\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "app.exe\nrelease-version.txt\n");
  job.restart_executable = install / "app.exe";
  WriteFile(install / "app.exe", "DEVELOPMENT_BUILD");
  WriteFile(install / "release-version.txt", "0.6.0");
  WriteFile(install / "user" / "trespasser.ini", "player settings");
  WriteFile(install / "notes.txt", "keep me");
  WriteFile(job.staging_dir / "app.exe", "PUBLIC_RELEASE");
  WriteFile(job.staging_dir / "release-version.txt", "0.6.0");

  const InstallOutcome outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "app.exe") == "PUBLIC_RELEASE");
  CHECK(ReadFile(install / "release-version.txt") == "0.6.0");
  CHECK(ReadFile(install / "user" / "trespasser.ini") == "player settings");
  CHECK(ReadFile(install / "notes.txt") == "keep me");
}

void TestRollback() {
  std::printf("install and rollback\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "");
  const std::filesystem::path staging = job.staging_dir;
  const std::filesystem::path manifest = job.manifest_file;
  WriteFile(install / "trespasser_recomp.exe", "old launcher");
  WriteFile(install / "vcruntime140.dll", "old runtime");
  WriteFile(install / "user_data" / "launcher" / "trespasser.ini", "player settings");
  WriteFile(install / "user_data" / "saves" / "SAVEGAME.000", "save data");
  WriteFile(install / "game" / "versions" / "en-116" / "data" / "be.scn", "game files");
  WriteFile(install / "logs" / "trespasser_recomp_001.log", "log");
  WriteFile(install / "cache" / "vulkan_pipeline.cache", "shader cache");
  WriteFile(install / "custom-notes.txt", "keep me");
  WriteFile(install / "recomp-registry.txt", "registry");
  WriteFile(install / "saves" / "SAVEGAME.001", "older saves folder");
  WriteFile(install / "versions" / "es-117" / "data" / "be.scn", "spanish game files");
  WriteFile(install / "msvcp-from-an-older-release.dll", "stale runtime");

  auto stage = [&] {
    WriteFile(staging / "trespasser_recomp.exe", "new launcher");
    WriteFile(staging / "trg-updater.exe", "new helper");
    WriteFile(staging / "vcruntime140.dll", "new runtime");
    WriteFile(staging / "assets" / "new" / "data.png", "new nested file");
    WriteFile(manifest, "trespasser_recomp.exe\ntrg-updater.exe\nvcruntime140.dll\nassets/new/data.png\n");
  };
  stage();

  int replacements = 0;
  InstallOptions fail;
  fail.replace = [&replacements](const std::filesystem::path& from, const std::filesystem::path& to) {
    ++replacements;
    if (replacements == 3) {
      return false;
    }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
  };
  InstallOutcome failed = InstallFiles(job, fail, false);
  CHECK(failed.status == InstallStatus::kInstallFailedRolledBack);
  CHECK(ReadFile(install / "trespasser_recomp.exe") == "old launcher");
  CHECK(!std::filesystem::exists(install / "trg-updater.exe"));
  CHECK(ReadFile(install / "vcruntime140.dll") == "old runtime");
  CHECK(ReadFile(install / "user_data" / "launcher" / "trespasser.ini") == "player settings");
  CHECK(ReadFile(install / "user_data" / "saves" / "SAVEGAME.000") == "save data");
  CHECK(ReadFile(install / "game" / "versions" / "en-116" / "data" / "be.scn") == "game files");
  CHECK(ReadFile(install / "logs" / "trespasser_recomp_001.log") == "log");
  CHECK(ReadFile(install / "cache" / "vulkan_pipeline.cache") == "shader cache");
  CHECK(ReadFile(install / "custom-notes.txt") == "keep me");
  CHECK(ReadFile(install / "recomp-registry.txt") == "registry");
  CHECK(ReadFile(install / "saves" / "SAVEGAME.001") == "older saves folder");
  CHECK(ReadFile(install / "versions" / "es-117" / "data" / "be.scn") == "spanish game files");
  CHECK(ReadFile(install / "msvcp-from-an-older-release.dll") == "stale runtime");

  stage();
  replacements = 0;
  const InstallOutcome success = InstallFiles(job, {}, false);
  CHECK(success.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "trespasser_recomp.exe") == "new launcher");
  CHECK(ReadFile(install / "trg-updater.exe") == "new helper");
  CHECK(ReadFile(install / "vcruntime140.dll") == "new runtime");
  CHECK(ReadFile(install / "assets" / "new" / "data.png") == "new nested file");
  CHECK(ReadFile(install / "user_data" / "launcher" / "trespasser.ini") == "player settings");
  CHECK(ReadFile(install / "user_data" / "saves" / "SAVEGAME.000") == "save data");
  CHECK(ReadFile(install / "game" / "versions" / "en-116" / "data" / "be.scn") == "game files");
  CHECK(ReadFile(install / "logs" / "trespasser_recomp_001.log") == "log");
  CHECK(ReadFile(install / "cache" / "vulkan_pipeline.cache") == "shader cache");
  CHECK(ReadFile(install / "custom-notes.txt") == "keep me");
  CHECK(ReadFile(install / "recomp-registry.txt") == "registry");
  CHECK(ReadFile(install / "saves" / "SAVEGAME.001") == "older saves folder");
  CHECK(ReadFile(install / "versions" / "es-117" / "data" / "be.scn") == "spanish game files");
  CHECK(ReadFile(install / "msvcp-from-an-older-release.dll") == "stale runtime");
}

void TestInstallFailures() {
  std::printf("install failures\n");
  {
    TempDir temp;
    UpdateJob job;
    const std::filesystem::path install =
        MakeJob(job, temp.path, "trespasser_recomp.exe\nSDL3.dll\n");
    std::filesystem::create_directories(install / "trespasser_recomp.exe");
    WriteFile(install / "SDL3.dll", "old library");
    WriteFile(job.staging_dir / "trespasser_recomp.exe", "new program");
    WriteFile(job.staging_dir / "SDL3.dll", "new library");
    const InstallOutcome outcome = InstallFiles(job, {}, false);
    CHECK(outcome.status == InstallStatus::kBackupFailed);
    CHECK(!outcome.error.empty());
    CHECK(std::filesystem::is_directory(install / "trespasser_recomp.exe"));
    CHECK(ReadFile(install / "SDL3.dll") == "old library");
  }
  {
    TempDir temp;
    UpdateJob job;
    const std::filesystem::path install =
        MakeJob(job, temp.path, "trespasser_recomp.exe\nSDL3.dll\n");
    WriteFile(install / "trespasser_recomp.exe", "old program");
    WriteFile(install / "SDL3.dll", "old library");
    WriteFile(job.staging_dir / "trespasser_recomp.exe", "new program");
    WriteFile(job.staging_dir / "SDL3.dll", "new library");
    int replacements = 0;
    InstallOptions lose_backup;
    lose_backup.replace = [&](const std::filesystem::path& from, const std::filesystem::path& to) {
      std::error_code ec;
      if (++replacements == 2) {
        std::filesystem::remove_all(job.backup_dir, ec);
        return false;
      }
      std::filesystem::rename(from, to, ec);
      return !ec;
    };
    const InstallOutcome outcome = InstallFiles(job, lose_backup, false);
    CHECK(outcome.status == InstallStatus::kInstallFailedRollbackFailed);
    CHECK(outcome.error.find("could not be restored") != std::string::npos);
    CHECK(std::filesystem::exists(job.staging_dir / "SDL3.dll"));
  }
  {
    TempDir temp;
    UpdateJob job;
    const std::filesystem::path install = MakeJob(job, temp.path, "trespasser_recomp.exe\n");
    WriteFile(install / "trespasser_recomp.exe", "old program");
    WriteFile(job.staging_dir / "trespasser_recomp.exe", "not a program");
    const InstallOutcome outcome = InstallFiles(job, {}, true);
    CHECK(outcome.status == InstallStatus::kRestartFailedRolledBack);
    CHECK(ReadFile(install / "trespasser_recomp.exe") == "old program");
  }
}

void TestPackagingNames() {
  std::printf("packaging names\n");
  const std::string release_text = ReadFile(std::string(JPT_SOURCE_DIR) + "/config/release.json");
  const std::optional<json::Value> release = json::Parse(release_text);
  CHECK(release && release->IsObject());
  const auto field = [&](const char* key) {
    const json::Value* value = release ? release->Find(key) : nullptr;
    return value && value->IsString() ? value->AsString() : std::string();
  };
  const std::string prefix = field("asset_prefix"), repository = field("repository");
  CHECK(!prefix.empty() && !repository.empty() && !field("windows_suffix").empty() && !field("program").empty());

  const std::string config = ReadFile(std::string(JPT_UPDATER_SRC_DIR) + "/update_config.cpp");
  for (const char* macro : {"JPT_RELEASE_REPOSITORY", "JPT_RELEASE_ASSET_PREFIX", "JPT_RELEASE_WINDOWS_SUFFIX", "JPT_RELEASE_LINUX_SUFFIX",
                            "JPT_RELEASE_PROGRAM", "JPT_RELEASE_UPDATER"}) {
    CHECK(config.find(macro) != std::string::npos);
  }
  CHECK(config.find("saulob/") == std::string::npos && config.find("JurassicPark") == std::string::npos);

  const std::string verify = ReadFile(std::string(JPT_SOURCE_DIR) + "/tools/verify-release.ps1");
  const std::string package = ReadFile(std::string(JPT_SOURCE_DIR) + "/tools/package.ps1");
  const std::string cmake = ReadFile(std::string(JPT_SOURCE_DIR) + "/CMakeLists.txt");
  for (const std::string& script : {verify, package}) {
    CHECK(script.find("config\\release.json") != std::string::npos);
    CHECK(script.find(prefix) == std::string::npos && script.find("windows-x86") == std::string::npos);
  }
  CHECK(cmake.find("add_executable(trg-updater WIN32") != std::string::npos);
  CHECK(cmake.find("add_dependencies(trespasser_recomp trg-updater)") != std::string::npos);
  CHECK(cmake.find("generated/jpt_release.h") != std::string::npos);

  const UpdateConfig built = jpt::MakeUpdateConfig(std::filesystem::temp_directory_path());
  const std::optional<SemanticVersion> next = ParseVersion("0.2.0");
#if defined(_WIN32)
  CHECK(next && built.asset_name(*next) == prefix + "-v0.2.0-" + field("windows_suffix") + ".zip");
  CHECK(next && built.checksum_asset_name(*next) == prefix + "-v0.2.0-" + field("windows_suffix") + ".zip.sha256");
  CHECK(built.required_staged_files == std::vector<std::string>({field("program"), field("updater")}));
#else
  CHECK(next && built.asset_name(*next) == prefix + "-v0.2.0-" + field("linux_suffix") + ".AppImage");
#endif
  CHECK(built.releases_url == "https://api.github.com/repos/" + repository + "/releases/latest");
  CHECK(built.download_url_prefix == "https://github.com/" + repository + "/releases/download/");
}

void TestUrlPolicy() {
  std::printf("Update address policy\n");
  const std::vector<std::string> hosts = GitHubReleaseHosts();
  CHECK(UrlHostAllowed("https://api.github.com/repos/saulob/jurassic-park-trespasser-recomp/releases/latest", hosts));
  CHECK(UrlHostAllowed("https://release-assets.githubusercontent.com/github-production-release-asset/1?sig=a", hosts));
  CHECK(UrlHostAllowed("https://objects.githubusercontent.com/x", hosts));
  CHECK(UrlHostAllowed("HTTPS://GitHub.com:443/saulob", hosts));
  CHECK(!UrlHostAllowed("http://github.com/saulob", hosts));
  CHECK(!UrlHostAllowed("https://example.com/github.com", hosts));
  CHECK(!UrlHostAllowed("https://github.com.example.com/x", hosts));
  CHECK(!UrlHostAllowed("https://github.com@example.com/x", hosts));
  CHECK(!UrlHostAllowed("https://github.com:8443/x", hosts));
  CHECK(!UrlHostAllowed("https://github.com\\@example.com/x", hosts));
  CHECK(!UrlHostAllowed("https://", hosts));
  CHECK(!UrlHostAllowed("https://github.com/x", {}));

  const std::string prefix = "https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/";
  CHECK(UrlAllowed(prefix + "v1.1.0/JurassicParkTrespasserRecompiled-v1.1.0-windows-x86.zip", hosts, prefix));
  CHECK(UrlAllowed(prefix + "v1.1.0/JurassicParkTrespasserRecompiled-v1.1.0-windows-x86.zip.sha256", hosts, prefix));
  CHECK(!UrlAllowed("https://github.com/someone/else/releases/download/v1/JurassicParkTrespasserRecompiled-v1-windows-x86.zip", hosts,
                    prefix));
  CHECK(!UrlAllowed("https://objects.githubusercontent.com/JurassicParkTrespasserRecompiled-v1.1.0-windows-x86.zip", hosts, prefix));
  CHECK(!UrlAllowed("http://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v1.1.0/a.zip", hosts, prefix));
  CHECK(UrlAllowed("https://github.com/anything", hosts, ""));
}

void TestPaths() {
  std::printf("path safety\n");
  CHECK(IsSafeRelativePath("trespasser_recomp.exe"));
  CHECK(IsSafeRelativePath("trg-updater.exe"));
  CHECK(IsSafeRelativePath("assets/launcher/header-art.png"));
  CHECK(IsSafeRelativePath("./trespasser_recomp.exe"));
  CHECK(IsSafeRelativePath("JurassicParkTrespasserRecompiled-v0.7.0-windows-x86/trespasser_recomp.exe"));
  CHECK(!IsSafeRelativePath("../trespasser_recomp.exe"));
  CHECK(!IsSafeRelativePath("/absolute"));
  CHECK(!IsSafeRelativePath("C:/trespasser_recomp.exe"));
  CHECK(!IsSafeRelativePath("\\\\server\\share"));
  CHECK(!IsSafeRelativePath("a//b"));
  CHECK(!IsSafeRelativePath("a/./../b"));
  CHECK(IsProtectedRelativePath("game/versions/en-116/data/be.scn"));
  CHECK(IsProtectedRelativePath("game/Processor.dll"));
  for (const char* path : {"user_data\\saves\\SAVEGAME.000", "USER_DATA/Saves/a.sav", "User_Data\\launcher\\trespasser.ini", "SAVES\\SAVEGAME.001",
                           "Logs/trespasser_recomp_001.log", "logs\\x.txt", "a/user_data\\b", "assets\\..\\user_data/x", "Game\\Versions\\en-116\\data\\be.scn",
                           "cache\\vulkan_pipeline.cache", "VERSIONS/ru-116/data/x.tpa", "Trespasser.INI", "Recomp-Registry.txt"}) {
    CHECK(IsProtectedRelativePath(path));
  }
  for (const char* path : {"assets\\launcher\\header-art.png", "licenses\\THIRD-PARTY-LICENSES.txt", "vcruntime140.dll", "README.txt"}) {
    CHECK(!IsProtectedRelativePath(path));
  }
  CHECK(IsProtectedRelativePath("game\\versions\\en-116\\data\\be.scn"));
  CHECK(IsProtectedRelativePath("user_data/launcher/trespasser.ini"));
  CHECK(IsProtectedRelativePath("user_data/saves/SAVEGAME.000"));
  CHECK(IsProtectedRelativePath("logs/a.log"));
  CHECK(IsProtectedRelativePath("cache/vulkan_pipeline.cache"));
  CHECK(IsProtectedRelativePath("user_data/cache/vulkan_pipeline.cache"));
  CHECK(IsProtectedRelativePath("versions/es-117/data/x.tpa"));
  CHECK(IsProtectedRelativePath("screenshots/a.png"));
  CHECK(IsProtectedRelativePath("trespasser.ini"));
  CHECK(IsProtectedRelativePath("recomp-registry.txt"));
  CHECK(IsProtectedRelativePath("user_data/bug_reports.json"));
  CHECK(IsProtectedRelativePath("data/disc.iso"));
  CHECK(!IsProtectedRelativePath("trespasser_recomp.exe"));
  CHECK(!IsProtectedRelativePath("trg-updater.exe"));
  CHECK(!IsProtectedRelativePath("rexruntime.dll"));
  CHECK(!IsProtectedRelativePath("assets/launcher/header-art.png"));
  CHECK(!IsProtectedRelativePath("licenses/THIRD-PARTY-LICENSES.txt"));
  CHECK(!IsProtectedRelativePath("SDL3.dll"));
  CHECK(!IsProtectedRelativePath("libGLESv2.dll"));
  CHECK(!IsProtectedRelativePath("README.txt"));
}

std::vector<std::pair<std::string, std::string>> PortPackage(const std::vector<std::string>& omit,
                                                             const std::vector<std::pair<std::string, std::string>>& extra) {
  const std::string root = "JurassicParkTrespasserRecompiled-v0.2.0-windows-x86/";
  std::vector<std::pair<std::string, std::string>> entries;
  for (const char* file : {"trespasser_recomp.exe", "trg-updater.exe", "SDL3.dll",
                           "libc++.dll", "libGLESv2.dll", "README.txt", "LICENSE.txt", "assets/launcher/header-art.png",
                           "licenses/THIRD-PARTY-LICENSES.txt"}) {
    if (std::find(omit.begin(), omit.end(), file) == omit.end()) {
      entries.emplace_back(root + file, std::string("content of ") + file);
    }
  }
  for (const auto& [name, content] : extra) {
    entries.emplace_back(root + name, content);
  }
  return entries;
}

bool PortPackageAccepted(const std::filesystem::path& temp, const std::vector<std::string>& required,
                         const std::vector<std::string>& omit,
                         const std::vector<std::pair<std::string, std::string>>& extra) {
  static int serial = 0;
  const std::filesystem::path folder = temp / ("package-" + std::to_string(++serial));
  WriteFile(folder / "update.zip", MakeZip(PortPackage(omit, extra)));
  std::string error;
  std::filesystem::path root;
  return ExtractZip(folder / "update.zip", folder / "staging", {}, error) &&
         ResolveStagingRoot(folder / "staging", required, root, error) && ValidateStaging(root, required, error) &&
         !ListStagingFiles(root, error).empty() && error.empty();
}

void TestPortContract() {
  std::printf("Trespasser release contract\n");
  TempDir temp;
  WriteFile(temp.path / "trg-updater.exe", "helper");
  const UpdateConfig config = jpt::MakeUpdateConfig(temp.path);
  CHECK(config.current_version == JPT_PORT_VERSION);
  CHECK(ParseVersion(config.current_version).has_value());
  CHECK(config.releases_url == "https://api.github.com/repos/saulob/jurassic-park-trespasser-recomp/releases/latest");
  CHECK(UrlHostAllowed(config.releases_url, config.allowed_hosts));
  CHECK(config.download_url_prefix == "https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/");
  CHECK(config.allowed_hosts == GitHubReleaseHosts());
  const std::optional<SemanticVersion> next = ParseVersion("v0.2.0+build.7");
  CHECK(!config.restart_arguments.empty() && config.restart_arguments.back() == "--launcher");
  for (const std::string& argument : config.restart_arguments) {
    CHECK(argument != "--skip-launcher");
  }
  CHECK(std::count(config.restart_arguments.begin(), config.restart_arguments.end(), std::string("--launcher")) == 1);
  // The ZIP holds Windows programs; the checks of its contents below apply to the package either platform refuses.
  const std::vector<std::string> required = {"trespasser_recomp.exe", "trg-updater.exe"};
#if defined(_WIN32)
  CHECK(config.package_kind == UpdatePackageKind::kZip);
  CHECK(next && config.asset_name(*next) == "JurassicParkTrespasserRecompiled-v0.2.0-windows-x86.zip");
  CHECK(next && config.checksum_asset_name(*next) == "JurassicParkTrespasserRecompiled-v0.2.0-windows-x86.zip.sha256");
  CHECK(config.restart_executable == temp.path / "trespasser_recomp.exe");
  CHECK(config.install_supported && config.install_unsupported_reason.empty());
  CHECK(config.required_staged_files == required);

  TempDir bare;
  const UpdateConfig without_helper = jpt::MakeUpdateConfig(bare.path);
  CHECK(!without_helper.install_supported && !without_helper.install_unsupported_reason.empty());
#else
  CHECK(config.package_kind == UpdatePackageKind::kAppImage);
  CHECK(next && config.asset_name(*next) == "JurassicParkTrespasserRecompiled-v0.2.0-x86_64.AppImage");
  CHECK(next && config.checksum_asset_name(*next) == "JurassicParkTrespasserRecompiled-v0.2.0-x86_64.AppImage.sha256");
  // Outside an AppImage (tools/build.sh output) nothing can be replaced.
  CHECK(!config.install_supported && !config.install_unsupported_reason.empty() && config.appimage_path.empty());
#endif

  CHECK(PortPackageAccepted(temp.path, required, {}, {}));
  for (const std::string& file : required) {
    CHECK(!PortPackageAccepted(temp.path, required, {file}, {}));
  }
  CHECK(!PortPackageAccepted(temp.path, required, {"trespasser_recomp.exe"},
                             {{"13_sentinels_aegis_rim.exe", "another port"}}));
  const std::vector<std::pair<std::string, std::string>> game_data = {
      {"game/versions/en-116/data/be.scn", "scene"}, {"versions/ru-116/data/a.tpa", "audio"},
      {"disc.iso", "disc image"},                    {"game/Processor.dll", "game"},
      {"user_data/saves/SAVEGAME.000", "save"},      {"user_data/launcher/trespasser.ini", "settings"},
      {"logs/a.txt", "log"},                         {"saves/SAVEGAME.001", "saves"},
      {"previews/a.png", "previews"},                {"recomp-registry.txt", "registry"},
      {"trespasser.ini", "settings"},
  };
  for (const auto& entry : game_data) {
    const bool accepted = PortPackageAccepted(temp.path, required, {}, {entry});
    if (accepted) {
      std::printf("    accepted %s\n", entry.first.c_str());
    }
    CHECK(!accepted);
  }
}

#if !defined(_WIN32)
// A type 2 AppImage as its runtime begins: an ELF program with 'A' 'I' 2 at offset 8, bytes the kernel ignores when it
// starts the program, followed by the file system (here a marker).
std::string AppImageLike(const std::string& marker) {
  std::ifstream in("/bin/true", std::ios::binary);
  std::string program{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (program.size() > 16) {
    program[8] = 'A';
    program[9] = 'I';
    program[10] = '\x02';
  }
  return program + marker;
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool Leftovers(const std::filesystem::path& folder) {
  for (const auto& entry : std::filesystem::directory_iterator(folder)) {
    if (entry.path().filename().string().find(".trg-update-") != std::string::npos) {
      return true;
    }
  }
  return false;
}

// The AppImage is replaced as one file, keeps its mode, starts again from its own path, and is restored when the new
// one cannot start. Only /bin/true is ever run.
void TestAppImageUpdate() {
  std::printf("AppImage update\n");
  TempDir temp;
  const std::filesystem::path installed = temp.path / "beside" / "JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage";
  const std::filesystem::path downloaded = temp.path / "download" / "JurassicParkTrespasserRecompiled-v0.2.0-x86_64.AppImage";
  WriteFile(installed, AppImageLike("OLD"));
  WriteFile(downloaded, AppImageLike("NEW"));
  std::filesystem::permissions(installed, std::filesystem::perms(0750));
  std::filesystem::permissions(downloaded, std::filesystem::perms(0644));
  const std::string old_image = ReadAll(installed);

  unsetenv("APPDIR");
  setenv("APPIMAGE", installed.c_str(), 1);
  std::filesystem::path found;
  CHECK(HasOriginalAppImage(found) && found == std::filesystem::weakly_canonical(installed));
  const UpdateConfig config = jpt::MakeUpdateConfig(temp.path / "mount" / "usr" / "bin");
  CHECK(config.install_supported && config.appimage_path == found && config.restart_executable == found);
  // Never the mount or the extraction folder it runs from, and never a file that is not an AppImage.
  setenv("APPDIR", (temp.path / "beside").c_str(), 1);
  CHECK(!HasOriginalAppImage(found));
  unsetenv("APPDIR");
  WriteFile(temp.path / "AppRun", "#!/bin/sh\nexec usr/bin/13_sentinels_aegis_rim \"$@\"\n");
  setenv("APPIMAGE", (temp.path / "AppRun").c_str(), 1);
  CHECK(!HasOriginalAppImage(found));
  setenv("APPIMAGE", (temp.path / "missing.AppImage").c_str(), 1);
  CHECK(!HasOriginalAppImage(found));
  unsetenv("APPIMAGE");
  CHECK(!HasOriginalAppImage(found));

  std::string error;
  WriteFile(temp.path / "download" / "page.html", "<html>not found</html>");
  CHECK(!ApplyAppImageUpdate(temp.path / "download" / "page.html", installed, {"--launcher"}, error) && !error.empty());
  CHECK(ReadAll(installed) == old_image && !Leftovers(installed.parent_path()));

  WriteFile(temp.path / "download" / "broken.AppImage", std::string("\x7f" "ELF\x02\x01\x01\x00" "AI\x02", 11) + "NEW");
  CHECK(!ApplyAppImageUpdate(temp.path / "download" / "broken.AppImage", installed, {"--launcher"}, error));
  CHECK(ReadAll(installed) == old_image && !Leftovers(installed.parent_path()));

  CHECK(ApplyAppImageUpdate(downloaded, installed, {"--launcher"}, error) && error.empty());
  CHECK(ReadAll(installed) == AppImageLike("NEW") && !Leftovers(installed.parent_path()));
  CHECK((std::filesystem::status(installed).permissions() & std::filesystem::perms::all) == std::filesystem::perms(0751));
}
#endif

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestVersions();
  TestReleases();
  TestSha256();
  TestZipAndStaging();
  TestJobs();
  TestManifest();
  TestSameVersionReplacement();
  TestRollback();
  TestInstallFailures();
  TestPackagingNames();
  TestUrlPolicy();
  TestPaths();
  TestPortContract();
#if !defined(_WIN32)
  TestAppImageUpdate();
#endif
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

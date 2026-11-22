#include "update_config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jpt_release.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"

#ifndef JPT_PORT_VERSION
#define JPT_PORT_VERSION "unknown"
#endif

namespace jpt {
namespace {

constexpr const char* kReleasesUrl = "https://api.github.com/repos/" JPT_RELEASE_REPOSITORY "/releases/latest";
constexpr const char* kDownloadUrlPrefix = "https://github.com/" JPT_RELEASE_REPOSITORY "/releases/download/";
constexpr const char* kAssetStem = JPT_RELEASE_ASSET_PREFIX "-v";

std::optional<std::string> Environment(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
#else
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

bool TestMode() {
    const std::optional<std::string> value = Environment("JPT_UPDATE_TEST");
    return value && *value == "1";
}

std::optional<std::string> TestVariable(const char* name) {
    if (!TestMode()) {
        return std::nullopt;
    }
    return Environment(name);
}

std::vector<std::string> RestartArguments() {
    std::vector<std::string> arguments = trg::update::CurrentProcessArguments();
    arguments.erase(std::remove_if(arguments.begin(), arguments.end(),
                                   [](const std::string& argument) {
                                       return argument == "--skip-launcher" || argument == "--launcher";
                                   }),
                    arguments.end());
    arguments.emplace_back("--launcher");
    return arguments;
}

} // namespace

trg::update::UpdateConfig MakeUpdateConfig(const std::filesystem::path& install_dir) {
    using trg::update::SemanticVersion;
    using trg::update::UpdatePackageKind;

    trg::update::UpdateConfig config;
    config.current_version = JPT_PORT_VERSION;
    config.releases_url = kReleasesUrl;
    config.download_url_prefix = kDownloadUrlPrefix;
    config.user_agent = std::string("Trespasser-Recompiled-Updater/") + JPT_PORT_VERSION;
    config.install_dir = install_dir;
    config.restart_arguments = RestartArguments();

#if defined(_WIN32)
    config.package_kind = UpdatePackageKind::kZip;
    config.asset_name = [](const SemanticVersion& version) {
        return std::string(kAssetStem) + version.CoreString() + "-" JPT_RELEASE_WINDOWS_SUFFIX ".zip";
    };
    config.checksum_asset_name = [](const SemanticVersion& version) {
        return std::string(kAssetStem) + version.CoreString() + "-" JPT_RELEASE_WINDOWS_SUFFIX ".zip.sha256";
    };
    config.restart_executable = install_dir / JPT_RELEASE_PROGRAM;
    // Every later package must keep these, or the installed updater refuses it.
    config.required_staged_files = {JPT_RELEASE_PROGRAM, JPT_RELEASE_UPDATER};
    config.install_supported = std::filesystem::exists(install_dir / JPT_RELEASE_UPDATER);
    if (!config.install_supported) {
        config.install_unsupported_reason =
            "The update helper is missing from this installation. Reinstall this release to enable automatic updates.";
    }
#else
    config.package_kind = UpdatePackageKind::kAppImage;
    config.asset_name = [](const SemanticVersion& version) {
        return std::string(kAssetStem) + version.CoreString() + "-" JPT_RELEASE_LINUX_SUFFIX ".AppImage";
    };
    config.checksum_asset_name = [](const SemanticVersion& version) {
        return std::string(kAssetStem) + version.CoreString() + "-" JPT_RELEASE_LINUX_SUFFIX ".AppImage.sha256";
    };
    // The AppImage file itself ($APPIMAGE), never its read-only mount or an extracted copy, is replaced and restarted.
    std::filesystem::path appimage;
    if (trg::update::HasOriginalAppImage(appimage)) {
        config.appimage_path = appimage;
        config.restart_executable = appimage;
        config.install_supported = true;
    } else {
        config.install_unsupported_reason = "Automatic installation is available in the AppImage build.";
    }
#endif

    config.log = [](std::string_view text) { std::printf("Updater: %.*s\n", int(text.size()), text.data()); };

    if (const std::optional<std::string> version = TestVariable("JPT_UPDATE_TEST_CURRENT_VERSION"); version) {
        config.current_version = *version;
        std::printf("Updater test mode: reporting version %s\n", config.current_version.c_str());
    }
    if (const std::optional<std::string> json = TestVariable("JPT_UPDATE_TEST_RELEASE_JSON"); json) {
        config.release_json_override = trg::update::PathFromUtf8(*json);
    }
    if (const std::optional<std::string> hash = TestVariable("JPT_UPDATE_TEST_ASSET_SHA256"); hash) {
        config.sha256_override = *hash;
    }
    return config;
}

} // namespace jpt

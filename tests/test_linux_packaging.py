#!/usr/bin/env python3
"""Static and headless contracts of the Linux AppImage packaging, read only except for the packaging script's own
self-test and bash syntax checks (both run only on Linux): py -3 -m unittest discover -s tests -p "test_linux_packaging.py" -v

The checks hold what no C suite can: the artifact names come from config/release.json and project VERSION, the AppDir
holds only the release files, the desktop and AppStream metadata are one application, the runtime is pinned and falls
back without FUSE, and the package script audits every ELF, refuses host and static libraries, and writes the four
release files. No game, launcher or window is started.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gate_support as gs  # noqa: E402


def text(path):
    return gs.strip_comments((ROOT / path).read_text(encoding="utf-8", errors="replace"))


def raw(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def release():
    return json.loads(raw("config/release.json"))


def project_version():
    match = re.search(r"^project\(trespasser_recomp VERSION ([0-9]+\.[0-9]+\.[0-9]+)", raw("CMakeLists.txt"), re.M)
    assert match, "CMakeLists.txt has no project VERSION"
    return match.group(1)


def application_id():
    match = re.search(r'#define\s+JPT_APPLICATION_ID\s+"([^"]+)"', raw("src/common/jpt_port.h"))
    assert match, "jpt_port.h defines no JPT_APPLICATION_ID"
    return match.group(1)


def baselines():
    values = {}
    for line in raw("patches/appimage-runtime/baseline.env").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


CATALOG_NAME = "Jurassic_Park_Trespasser_Recompiled"

# code/manager-hint.sh in AppImage/appimage.github.io matches these words in the desktop entry's Name, GenericName and
# Comment; only then is "Not installed" on the screenshot a warning instead of a test failure.
MANAGER_WORDS = {"launcher", "manager", "installer", "downloader", "updater", "marketplace"}


def desktop_entry():
    app_id = application_id()
    desktop = raw("packaging/linux/%s.desktop" % app_id)
    main = desktop.split("[Desktop Action", 1)[0]
    return app_id, desktop, dict(line.split("=", 1) for line in main.splitlines() if "=" in line)


def manager_hint(text):
    spaced = re.sub(r"[-_.]+", " ", re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", text))
    return [word for word in re.findall(r"[A-Za-z]+", spaced) if word.lower() in MANAGER_WORDS]


def normalize_name(text):
    return re.sub(r"[^a-z0-9]", "", text.lower())


class Names(unittest.TestCase):
    def test_artifact_name_comes_from_release_json_and_project_version(self):
        version = project_version()
        config = release()
        package = raw("tools/package-linux.sh")
        self.assertIn('config/release.json', package)
        self.assertIn('"asset_prefix"', package)
        self.assertIn('"linux_suffix"', package)
        self.assertIn("name=$asset_prefix-v$version-$linux_suffix", package)
        self.assertIn('"$name.AppImage"', package)
        self.assertIn('"$name.AppImage.sha256"', package)
        self.assertIn("project\\(trespasser_recomp VERSION", package)
        self.assertNotIn(version, package, "the version is derived, never written in the packaging script")
        self.assertNotIn(config["asset_prefix"], package, "the asset prefix is read from config/release.json")
        self.assertNotIn(config["asset_prefix"], raw("tools/package-release.ps1"), "config/release.json is the one source")
        self.assertNotIn(config["asset_prefix"], raw("tools/package-release-linux.sh"))
        self.assertNotIn(version, raw("tools/package-release.ps1"))
        self.assertNotIn(version, raw("tools/package-release-linux.sh"))
        windows = "%s-v%s-%s" % (config["asset_prefix"], version, config["windows_suffix"])
        linux = "%s-v%s-%s" % (config["asset_prefix"], version, config["linux_suffix"])
        self.assertEqual(linux + ".AppImage", "JurassicParkTrespasserRecompiled-v%s-x86_64.AppImage" % version)

    def test_canonical_release_artifact_is_the_gated_one(self):
        package = raw("tools/package-linux.sh")
        for needle in ("build/linux/bin/$program", "$program.build.json", "tools/linux_artifact.py",
                       "linux_artifact.py\" verify", "test_result", "Release", "glibc_baseline",
                       "ffmpeg", "lgpl_only", "JPT_FFMPEG_REQUIRE_LGPL=1", "tools/test-linux.sh",
                       "--config Release", "--build-dir", "--stage", "--skip-build",
                       "tools/openssl-bootstrap.sh", "OpenSSL $openssl_version"):
            self.assertIn(needle, package, needle)
        self.assertNotIn("build/linux-work/bin", package, "the work product is never the packaged artifact")
        self.assertIn("--skip-build", package)
        self.assertIn("build/linux/bin", raw("docs/LINUX.md"))

    def test_release_contract_has_four_files(self):
        version = project_version()
        config = release()
        windows = "%s-v%s-%s.zip" % (config["asset_prefix"], version, config["windows_suffix"])
        linux = "%s-v%s-%s.AppImage" % (config["asset_prefix"], version, config["linux_suffix"])
        script = raw("tools/package-release.ps1")
        for needle in ("$release.asset_prefix", "$release.windows_suffix", "$release.linux_suffix",
                       "windowsName", "linuxName", ".zip", ".AppImage", ".AppImage.sha256",
                       "package.ps1", "package-release-linux.sh", "wsl.exe", "Test-Artifacts", "Get-Sha256"):
            self.assertIn(needle, script, needle)
        self.assertIn("windowsName.zip", script)
        self.assertIn('"$linuxName.AppImage"', script)
        self.assertIn('"$linuxName.AppImage.sha256"', script)
        self.assertNotRegex(script, r"gh release|git tag|git push|Invoke-WebRequest|upload-artifact")
        linux_script = raw("tools/package-release-linux.sh")
        self.assertIn("dist/*.AppImage", linux_script)
        self.assertIn("sha256sum -c", linux_script)
        self.assertIn("$name.AppImage.sha256", linux_script)
        self.assertTrue(windows.endswith(".zip"))
        self.assertTrue(linux.endswith(".AppImage"))


class AppDir(unittest.TestCase):
    def test_appdir_holds_only_the_release_files(self):
        package = raw("tools/package-linux.sh")
        for needle in ('rm -rf "$appdir"', "mkdir -p \"$dist\" \"$symbols\"",
                       'usr/bin/assets/launcher/header-art.png', 'usr/bin/assets/launcher/sponsor-heart.png',
                       'usr/share/applications', 'usr/share/metainfo', 'usr/share/icons/hicolor/256x256/apps',
                       'usr/share/licenses/$doc_name', 'usr/share/doc/$doc_name/README.txt',
                       'expected_entry', 'forbidden_names', 'forbidden_patterns', 'required_leaves',
                       'forbidden name:', 'unexpected item:', 'utf16_pattern', 'DEBUG_SAULO'):
            self.assertIn(needle, package, needle)
        for name in ("game", "user_data", "logs", "cache", "saves", "downloads", "staging", "mods"):
            self.assertRegex(package, r"forbidden_names='[^']*" + name, name)
        self.assertIn("usr/lib/compat", package)
        self.assertIn("usr/lib/*.so*", package)

    def test_apprun_preserves_the_appimage_contract(self):
        body = raw("packaging/linux/AppRun")
        self.assertTrue(body.startswith("#!/bin/sh"))
        self.assertIn('exec "$HERE/usr/bin/trespasser_recomp" "$@"', body)
        self.assertIn('readlink -f "$0"', body)
        for forbidden in ("/home/deck", "/proc/self/exe", "mkdir", "touch", " rm ", " cp ", " mv ", " cd ",
                          "APPIMAGE=", "APPDIR=", "LD_LIBRARY_PATH", "Wine", "Proton", "Steam"):
            self.assertNotIn(forbidden, body, forbidden)
        self.assertNotIn(b"\r", (ROOT / "packaging/linux/AppRun").read_bytes())
        package = raw("tools/package-linux.sh")
        self.assertIn('install -m755 packaging/linux/AppRun "$appdir/AppRun"', package)

    def test_desktop_and_appstream_are_one_application(self):
        app_id, desktop, entries = desktop_entry()
        self.assertEqual(entries.get("Type"), "Application")
        self.assertEqual(entries.get("Exec"), "AppRun")
        self.assertEqual(entries.get("Icon"), app_id)
        self.assertEqual(entries.get("StartupWMClass"), app_id)
        self.assertEqual(entries.get("Terminal"), "false")
        self.assertIn("Game", entries.get("Categories", ""))
        self.assertIn("Launcher", entries.get("Actions", ""))
        self.assertIn("[Desktop Action Launcher]", desktop)
        self.assertIn("Exec=AppRun --launcher", desktop)

        appdata = ET.fromstring(raw("packaging/linux/%s.appdata.xml" % app_id))
        self.assertEqual(appdata.findtext("id"), app_id)
        self.assertEqual(appdata.findtext("metadata_license"), "CC0-1.0")
        self.assertTrue(appdata.findtext("project_license"))
        self.assertEqual(appdata.findtext("launchable"), "%s.desktop" % app_id)
        self.assertEqual(appdata.find("launchable").get("type"), "desktop-id")
        self.assertEqual(appdata.findtext("icon"), app_id)
        self.assertEqual(appdata.find("icon").get("type"), "stock")
        self.assertEqual(entries.get("Name"), appdata.findtext("name"))
        self.assertIn("bring your own disc", appdata.findtext("summary"))
        description = " ".join("".join(appdata.find("description").itertext()).split())
        self.assertIn("No game files are included", description)
        self.assertIn("@VERSION@", raw("packaging/linux/%s.appdata.xml" % app_id))
        self.assertIn("@DATE@", raw("packaging/linux/%s.appdata.xml" % app_id))
        self.assertIn(app_id, raw("docs/LINUX.md"))
        package = raw("tools/package-linux.sh")
        for needle in ("desktop-file-validate", "appstreamcli validate --no-net", "s/@VERSION@/",
                       "s/@DATE@/", "release version="):
            self.assertIn(needle, package, needle)

    def test_icon_is_the_green_gamepad(self):
        icon = ROOT / "packaging/icons/app-icon-256.png"
        data = icon.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(int.from_bytes(data[16:20], "big"), 256)
        self.assertEqual(int.from_bytes(data[20:24], "big"), 256)
        self.assertTrue((ROOT / "packaging/icons/app-icon.ico").is_file())
        svg = raw("packaging/icons/app-icon.svg")
        self.assertIn("#8CC63F", svg, "the green gamepad accent")
        self.assertNotIn("trespasser-icon", svg)
        self.assertFalse((ROOT / "assets/app/trespasser-icon.png").exists())
        package = raw("tools/package-linux.sh")
        self.assertIn("packaging/icons/app-icon-256.png", package)
        self.assertIn('ln -s "$app_icon" "$appdir/.DirIcon"', package)

    def test_paths_stay_outside_the_read_only_mount(self):
        readme = raw("packaging/README-linux.txt")
        for needle in ("@VERSION@", "not included", "game", "logs", "user_data", "APPIMAGE_EXTRACT_AND_RUN",
                       "FUSE", "Steam Deck", "Wine", "Proton", "read-only", "experimental", "not been validated"):
            self.assertIn(needle, readme, needle)
        package = raw("tools/package-linux.sh")
        for needle in ("APPIMAGE", "never inside", "user_data", "logs", "game", "port folder"):
            self.assertIn(needle, raw("docs/LINUX.md"), needle)
        self.assertIn("--game", readme)
        self.assertIn("--log-dir", readme)
        self.assertIn("--user_data_root", readme)


class Catalog(unittest.TestCase):
    """The AppImage catalog's launcher exception and entry-name rules (code/manager-hint.sh, code/check-name.sh)."""

    def test_desktop_identifies_the_game_launcher(self):
        _, _, entries = desktop_entry()
        self.assertEqual(entries.get("GenericName"), "Game launcher")
        self.assertEqual(manager_hint(entries["GenericName"]), ["launcher"])
        self.assertTrue(entries.get("Comment"))
        self.assertFalse(manager_hint(entries["Name"]), "a game's name should not need the launcher exception")

    def test_catalog_name_follows_the_catalog_rules(self):
        _, _, entries = desktop_entry()
        self.assertLessEqual(CATALOG_NAME.count("-"), 2, "more than two hyphens needs a manual check")
        for forbidden in (" ", ":", "?", "#", "%", "/", "\\"):
            self.assertNotIn(forbidden, CATALOG_NAME)
        self.assertNotRegex(CATALOG_NAME, r"(?i)\.appimage$|x86[-_]64|amd64|aarch64|arm64|armhf|i[36]86")
        self.assertNotRegex(CATALOG_NAME, r"[0-9]+\.[0-9]+")
        self.assertRegex(CATALOG_NAME, r"[A-Z]")
        self.assertEqual(normalize_name(CATALOG_NAME), normalize_name(entries["Name"]))

    def test_launcher_opens_without_the_game_and_does_not_start_it(self):
        self.assertIn('case GameState::kMissing: return "Game not installed";', raw("launcher/game_files.cpp"))
        self.assertIn('Tr("Import the game from your Trespasser disc image.")', raw("launcher/launcher.cpp"))
        play_gate = raw("launcher/play_gate.h")
        self.assertIn("if (!files_ready)", play_gate)
        self.assertIn("return PlayRequest::kFilesNotReady;", play_gate)


class Runtime(unittest.TestCase):
    def test_runtime_is_pinned_and_falls_back_without_fuse(self):
        pins = baselines()
        for key in ("RUNTIME_REPO", "RUNTIME_TAG", "RUNTIME_COMMIT", "ALPINE_VERSION", "ALPINE_SHA256", "RUNTIME_SHA256"):
            self.assertIn(key, pins, key)
        self.assertRegex(pins["RUNTIME_REPO"], r"AppImage/type2-runtime")
        self.assertRegex(pins["RUNTIME_COMMIT"], r"^[0-9a-f]{40}$")
        self.assertRegex(pins["ALPINE_SHA256"], r"^[0-9a-f]{64}$")
        self.assertRegex(pins["RUNTIME_SHA256"], r"^[0-9a-f]{64}$")
        patch = raw("patches/appimage-runtime/0001-extract-and-run-when-fuse-fails.patch")
        for needle in ("APPIMAGE_EXTRACT_AND_RUN", "EXIT_FUSE_MOUNT_FAILED", "extract_and_run = true",
                       "mkdtemp", "forward_signal", "EXIT_EXECERROR"):
            self.assertIn(needle, patch, needle)
        builder = raw("tools/build-appimage-runtime.sh")
        for needle in ("baseline.env", "RUNTIME_SHA256", "Alpine", "chroot", "type 2 magic",
                       "git -C \"$src\" apply", "does not apply to $RUNTIME_COMMIT"):
            self.assertIn(needle, builder, needle)
        package = raw("tools/package-linux.sh")
        for needle in ("build-appimage-runtime.sh", "--runtime-file", "verify_embedded_runtime",
                       "appimagetool_version=1.9.1", "appimagetool_sha256=", "sha256sum -c",
                       "--appimage-extract-and-run"):
            self.assertIn(needle, package, needle)
        self.assertRegex(package, r"appimagetool_sha256=[0-9a-f]{64}")

    def test_package_script_audits_every_elf(self):
        package = raw("tools/package-linux.sh")
        for needle in ("newest_glibc_in", "Version needs section", "check_elf_text", "GLIBCXX", "CXXABI",
                       "GLIBCXX|CXXABI|GCC", "RPATH", "RUNPATH", "dynamic_entries_in", "readelf -d -W", "readelf -S -W",
                       "max_glibc=2.31", "newer_than", "check_appdir", "compgen -G", "ldd", "usr/lib/*.so*",
                       "check_elf_text 'the AppImage runtime:'"):
            self.assertIn(needle, package, needle)
        self.assertIn("objcopy --only-keep-debug", package)
        self.assertIn("objcopy --strip-debug --add-gnu-debuglink", package)
        self.assertIn("strip --strip-unneeded", package)
        self.assertIn("symbols=$dist/$name-symbols", package)
        self.assertIn("not in the package", package)

    def test_package_script_excludes_host_and_static_libraries(self):
        package = raw("tools/package-linux.sh")
        for library in ("libvulkan", "libGL", "libX11", "libwayland", "libasound", "libpulse", "libc",
                        "libdrm", "libgbm", "libnvidia", "libcuda"):
            self.assertIn(library, package, library)
        self.assertIn("static_only='^(libstdc\\+\\+|libgcc_s|libssl|libcrypto|libav(codec|format|util)|libSDL3)", package)
        for marker in ("RenderDoc", "vklayer", "implicit_layer", "explicit_layer", "statically"):
            self.assertIn(marker, package, marker)
        self.assertIn("links $file dynamically", package)


class ReleaseFlow(unittest.TestCase):
    """One command prepares Windows and Linux: package-release.ps1 owns the WSL distribution, the reviewed runtime and
    the stale-artifact cleanup, so nobody runs ci-deps.sh, copies a file or computes a hash by hand."""

    def test_one_dedicated_wsl_distribution(self):
        script = raw("tools/package-release.ps1")
        inputs = raw("tools/release-inputs.ps1")
        self.assertIn("[string]$WslDistro = 'JptFocalBuild'", script)
        self.assertIn("$script:DefaultWslDistro = 'JptFocalBuild'", inputs)
        sibling = "Mua" + "FocalBuild"
        for path in ("README.md", "docs/LINUX.md", "tools/package-release.ps1", "tools/release-inputs.ps1",
                     "tools/test.ps1", "tools/_gate_linux.ps1", "tests/gate_tests.ps1"):
            self.assertNotIn(sibling, raw(path), path)
        self.assertIn("JptFocalBuild", raw("docs/LINUX.md"))
        base = re.search(r"UbuntuBaseSha256 = '([0-9a-f]{64})'", inputs)
        self.assertTrue(base)
        self.assertIn(base.group(1), raw("docs/LINUX.md"))
        self.assertIn("ubuntu-base-20.04.5-base-amd64.tar.gz", inputs)

    def test_release_prepares_the_environment_itself(self):
        script = raw("tools/package-release.ps1")
        inputs = raw("tools/release-inputs.ps1")
        for needle in ("Initialize-WslBuildEnvironment", "Resolve-ReviewedRuntime", "JPT_APPIMAGE_RUNTIME=",
                       "import-appimage-runtime.sh", "package-release-linux.sh", "Test-Artifacts", "verify-release.ps1"):
            self.assertIn(needle, script, needle)
        for needle in ("wsl.exe --import", "wsl.exe -d $Name -u root", "packaging/linux/ci-deps.sh",
                       "Get-WslMissingTools", "$dedicated", "--version 2"):
            self.assertIn(needle.replace("wsl.exe ", "& wsl.exe ") if needle.startswith("wsl.exe") else needle, inputs, needle)
        self.assertNotIn("prepare WSL once", script)
        self.assertNotRegex(script, r"gh release|git tag|git push|upload-artifact")
        self.assertRegex(script, r"Remove-Item -LiteralPath \$stale\.FullName")
        for parameter in ("[switch]$Clean", "[switch]$SkipBuild", "[string]$WslDistro"):
            self.assertIn(parameter, script, parameter)

    def test_reviewed_runtime_is_reused_by_hash(self):
        pin = baselines()["RUNTIME_SHA256"]
        builder = raw("tools/build-appimage-runtime.sh")
        self.assertIn("JPT_APPIMAGE_RUNTIME", builder)
        self.assertIn('[ "$GIVEN" = "$RUNTIME_SHA256" ]', builder)
        inputs = raw("tools/release-inputs.ps1")
        self.assertIn(r"patches\appimage-runtime\baseline.env", inputs)
        self.assertIn(r"cache\appimage-runtime\runtime-x86_64", inputs)
        self.assertIn("Remove-Item -LiteralPath $file", inputs)
        importer = raw("tools/import-appimage-runtime.sh")
        self.assertIn("jurassic-park-trespasser-recomp-build/build/appimage/runtime-jpt-x86_64", importer)
        self.assertIn('sha256sum -- "$candidate"', importer)
        self.assertIn('sha256sum -- "$target.part"', importer)
        self.assertRegex(pin, r"^[0-9a-f]{64}$")
        self.assertRegex(raw(".gitignore"), r"(?m)^/cache/$")
        package = raw("tools/package-linux.sh")
        self.assertIn('runtime=$work/runtime-jpt-x86_64', package)
        self.assertIn("verify_embedded_runtime", package)

    def test_windows_symbols_stay_outside_the_zip(self):
        package = raw("tools/package.ps1")
        self.assertIn('"$name-symbols"', package)
        self.assertIn("Config = 'Release'", package)
        self.assertNotIn("RelWithDebInfo", text("tools/package.ps1"))
        verify = raw("tools/verify-release.ps1")
        for forbidden in ("*.pdb", "*.dmp", "*.log", "DEBUG_SAULO"):
            self.assertIn(forbidden, verify, forbidden)
        self.assertIn("windows_suffix", verify)
        self.assertEqual(release()["windows_suffix"], "windows-x86")
        self.assertIn("/d1trimfile:${JPT_SOURCE_PREFIX}", raw("CMakeLists.txt"), "Release __FILE__ strings carry no checkout path")
        self.assertIn("--no-appstream", raw("tools/package-linux.sh"))

    def test_both_packages_ship_the_same_component_licences(self):
        windows_block = raw("tools/package.ps1").split("$builtInLicenses = [ordered]@{", 1)[1].split("}", 1)[0]
        windows = {name: path.replace("\\", "/") for name, path in re.findall(r"'([A-Za-z-]+)'\s*=\s*'([^']+)'", windows_block)}
        linux_block = raw("tools/package-linux.sh").split("declare -A built_in=(", 1)[1].split(")\n", 1)[0]
        linux = dict(re.findall(r"\[([A-Za-z-]+)\]=(\S+)", linux_block))
        self.assertEqual(windows, linux)
        self.assertNotIn("TRG-Launcher", linux, "the TRG Launcher checkout carries no licence file: its MIT notice is in THIRD-PARTY-LICENSES.txt")
        notices = raw("packaging/licenses/THIRD-PARTY-LICENSES.txt")
        self.assertIn("TRG Launcher", notices)
        self.assertIn("TekRantGaming", notices)
        for path in linux.values():
            if not path.startswith("external/"):
                self.assertTrue((ROOT / path).is_file(), path)
        self.assertIn("licenses\\LICENSE.txt", raw("tools/verify-release.ps1"))

    def test_smoke_test_never_opens_a_window(self):
        smoke = raw("packaging/linux/smoke-test.sh")
        for needle in ("--build-info", "--appimage-extract-and-run", "--appimage-extract", "SDL_VIDEODRIVER=dummy",
                       "SDL_AUDIODRIVER=dummy", "appimage_extracted_", "unset DISPLAY"):
            self.assertIn(needle, smoke, needle)
        self.assertNotRegex(smoke, r"Xvfb|--launcher|--run")
        self.assertIn("packaging/linux/smoke-test.sh", raw("tools/package-release-linux.sh"))

    def test_mirror_leaves_game_and_player_data_behind(self):
        mirror = raw("tools/package-release-linux.sh")
        for name in (".git", "game", "user", "user_data", "cache", "logs", "saves", "build", "dist", "out", ".venv"):
            self.assertIn("--exclude '/%s'" % name, mirror, name)
        for name in ("analysis", "server"):
            self.assertNotIn("--exclude '/%s'" % name, mirror, "the Linux gate reads %s/" % name)
        self.assertIn("jurassic-park-trespasser-recomp-build", mirror)
        self.assertIn("JPT_WSL_BUILD_DIR", mirror)
        self.assertIn("--checksum", mirror)

    def test_source_date_epoch_is_only_taken_from_git(self):
        package = raw("tools/package-linux.sh")
        self.assertIn('git -C "$root" log -1 --format=%ct', package)
        self.assertIn("if [[ -z ${SOURCE_DATE_EPOCH:-} ]]", package)
        self.assertNotIn("|| date +%s", package)


class Docs(unittest.TestCase):
    def test_docs_describe_the_release_command(self):
        docs = raw("docs/LINUX.md")
        for needle in ("tools/package-linux.sh", "tools\\package-release.ps1", "packaging/linux/ci-deps.sh",
                       "glibc 2.31", "baseline.env", "APPIMAGE_EXTRACT_AND_RUN", "Steam Deck",
                       "JurassicParkTrespasserRecompiled-v<version>-x86_64.AppImage", ".AppImage.sha256"):
            self.assertIn(needle, docs, needle)
        build = raw("docs/BUILD.md")
        self.assertIn("ZIP and AppImage", build)
        self.assertIn("Steam Deck hardware validation remains", build)
        user_data = raw("docs/user-data.md")
        self.assertNotIn("future; not built yet", user_data)
        self.assertIn("Linux AppImage", user_data)


class Executed(unittest.TestCase):
    """The checks that need a shell. They run in the Linux gate; on Windows they are skipped, never counted as a pass."""

    @unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "needs Linux bash")
    def test_shell_scripts_parse(self):
        for script in ("tools/package-linux.sh", "tools/package-release-linux.sh", "tools/build-appimage-runtime.sh",
                       "tools/import-appimage-runtime.sh", "packaging/linux/ci-deps.sh", "packaging/linux/AppRun",
                       "packaging/linux/smoke-test.sh", "tools/test-linux.sh"):
            done = subprocess.run(["bash", "-n", str(ROOT / script)], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, script + "\n" + done.stdout + done.stderr)

    @unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "needs Linux bash")
    def test_package_linux_self_test(self):
        done = subprocess.run(["bash", str(ROOT / "tools/package-linux.sh"), "--self-test"],
                              capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("every case passed", done.stdout)

    @unittest.skipUnless(shutil.which("desktop-file-validate"), "desktop-file-validate is not installed")
    def test_desktop_file_validates(self):
        app_id = application_id()
        done = subprocess.run(["desktop-file-validate", str(ROOT / ("packaging/linux/%s.desktop" % app_id))],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    @unittest.skipUnless(shutil.which("appstreamcli"), "appstreamcli is not installed")
    def test_appstream_validates(self):
        app_id = application_id()
        version = project_version()
        with tempfile.TemporaryDirectory() as folder:
            source = ROOT / ("packaging/linux/%s.appdata.xml" % app_id)
            target = Path(folder) / source.name
            target.write_text(raw(str(source.relative_to(ROOT))).replace("@VERSION@", version).replace("@DATE@", "2026-01-01"),
                              encoding="utf-8")
            done = subprocess.run(["appstreamcli", "validate", "--no-net", str(target)], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()

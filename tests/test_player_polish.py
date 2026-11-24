#!/usr/bin/env python3
"""Static contracts of the player-facing package: the gamepad icon in the executable and both windows, the numbered
Level Select, the Bug Report page and the launcher's auto update, on Windows and Linux.

No window, no network, no build: the committed icon files and the sources are read. Run from the repository:

    py -3 tests/test_player_polish.py
"""
import json
import re
import struct
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def text(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


class IconTests(unittest.TestCase):
    def test_the_icon_is_the_gamepad_in_the_launcher_accent(self):
        svg = text("packaging/icons/app-icon.svg")
        launcher = text("launcher/launcher.cpp")
        match = re.search(r"kAccent = ImVec4\(0x([0-9A-F]{2}) / 255\.0f, 0x([0-9A-F]{2}) / 255\.0f, 0x([0-9A-F]{2}) / 255\.0f", launcher)
        self.assertIsNotNone(match, "kAccent not found")
        accent = "#" + "".join(match.groups())
        self.assertIn(accent.lower(), svg.lower(), "the icon carries the launcher accent")
        # The pad's body path and its buttons, as in the other ports (Dragon's Crown Pro PC, 13 Sentinels).
        self.assertIn("M310.933,342C312.734,342", svg)

    def test_the_committed_files_pass_the_icon_tool(self):
        done = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "make-app-icon.py"), "--check"], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        ico = (ROOT / "packaging/icons/app-icon.ico").read_bytes()
        self.assertEqual(struct.unpack("<HHH", ico[:6]), (0, 1, 7), "seven frames, 16 to 256")

    def test_the_old_icon_is_gone(self):
        self.assertFalse((ROOT / "assets" / "app" / "trespasser-icon.png").exists())
        for path in ("cmake/JptLinux.cmake", "docs/LINUX.md", "src/common/jpt_app_identity.h", "tests/test_linux_parity.py"):
            self.assertNotIn("trespasser-icon", text(path), path)
        self.assertIn("packaging/icons/app-icon-256.png", text("cmake/JptLinux.cmake"), "Linux embeds the new icon")

    def test_the_windows_executable_carries_the_icon_and_the_version(self):
        rc = text("src/trespasser_recomp.rc")
        self.assertIn('1 ICON "../packaging/icons/app-icon.ico"', rc)
        for value in ("FileDescription", "ProductName", "OriginalFilename", "FileVersion", "ProductVersion", "CompanyName"):
            self.assertIn(f'VALUE "{value}"', rc)
        self.assertNotIn("RT_MANIFEST", rc, "the linker embeds the manifest, not the resource script")
        self.assertTrue((ROOT / "src" / "trespasser_recomp.manifest").is_file())
        cmake = text("CMakeLists.txt")
        self.assertIn("enable_language(RC)", cmake)
        self.assertIn("src/trespasser_recomp.rc", cmake)
        self.assertIn("src/trespasser_recomp.manifest", cmake, "the linker takes the manifest as an input")
        self.assertIn("generated/jpt_version.h", cmake)
        self.assertIn("packaging/icons/app-icon.ico", cmake, "the executable relinks when the icon changes")

    def test_the_game_window_gets_the_icon(self):
        window = text("src/host/window_host.c")
        self.assertIn("static void set_window_icon", window)
        self.assertIn("WM_SETICON", window)
        self.assertIn("ICON_BIG", window)
        self.assertIn("ICON_SMALL", window)
        self.assertIn("MAKEINTRESOURCEA(1)", window)
        self.assertRegex(window, r"set_window_icon\(window\);\s*\n\s*activation_defer_init")

    def test_the_launcher_window_gets_it(self):
        # Windows: SDL registers its window class with resource 1 of the executable; Linux: the embedded PNG.
        self.assertIn("jpt_app_icon_apply", text("launcher/host.cpp"))
        self.assertIn("1 ICON", text("src/trespasser_recomp.rc"))


class LevelSelectTests(unittest.TestCase):
    def test_numbers_come_from_the_one_table(self):
        policy = text("src/host/debug_menu_policy.c")
        self.assertEqual(policy.count("k_retail_levels[] = {"), 1, "no second table of levels")
        self.assertIn("int dm_level_number(const char* scene)", policy)
        self.assertIn("k_retail_levels[rank].campaign ? rank + 1 : 0", policy)
        self.assertIn('snprintf(out, cap, "%d - %s", number, name)', policy)
        table = policy[policy.index("k_retail_levels[] = {"):]
        table = table[: table.index("};")]
        entries = re.findall(r'\{\s*"([^"]+)",\s*"([^"]+)",\s*([01])\s*\}', table)
        self.assertEqual(
            entries,
            [
                ("BE", "Beach", "1"),
                ("JR", "Jungle Road", "1"),
                ("IJ", "Industrial Jungle", "1"),
                ("IT", "InGen Town", "1"),
                ("LAB", "InGen Lab", "1"),
                ("AS", "Ascent, Part 1", "1"),
                ("AS2", "Ascent, Part 2", "1"),
                ("SUM", "Summit", "1"),
                ("TESTSCENE", "Test Scene", "0"),
                ("TESTSCNNGHT", "Test Scene Night", "0"),
            ],
        )
        rows = policy[policy.index("if (s->page == DM_PAGE_LEVELS) {\n        add_row(rows, max, &n, DM_ROW_SECTION"):]
        rows = rows[: rows.index("return n;")]
        self.assertIn("dm_level_row_text(", rows)
        self.assertIn("DM_ROW_ITEM, i,", rows, "a row selects by the scene's index, never by its text")

    def test_the_load_uses_the_scene_not_the_row(self):
        policy = text("src/host/debug_menu_policy.c")
        self.assertIn('snprintf(s->pending.scene, sizeof s->pending.scene, "%s", s->levels.scene[s->level_cursor]);', policy)


class GameplayOrderTests(unittest.TestCase):
    def test_gameplay_page_uses_canonical_order(self):
        options = text("launcher/options.h")
        order_block = options[options.index("kGameplayOrder[] = {"):]
        order_block = order_block[: order_block.index("};")]
        items = re.findall(r"GameplayItem::(k\w+)", order_block)
        expected = [
            "kLanguage",
            "kMinimap",
            "kCompass",
            "kWaypoints",
            "kVideoSubtitles",
            "kSkipIntro",
            "kTrespasserCeHeading",
            "kDualStow",
            "kActiveAi",
            "kOfficialPatchHeading",
            "kOfficialPatchFixes",
            "kFrameRateInTitle",
            "kFrameCounter",
            "kRenderResolutionInTitle",
        ]
        self.assertEqual(items, expected)
        launcher = text("launcher/launcher.cpp")
        page = launcher[launcher.index("void PageGameplay(") : launcher.index("// ---- Controls")]
        self.assertIn("for (const GameplayItem item : kGameplayOrder)", page)
        cases = re.findall(r"case GameplayItem::(k\w+):", page)
        self.assertEqual(cases, expected)


class BugReportTests(unittest.TestCase):
    def test_the_page_is_active_not_a_placeholder(self):
        launcher = text("launcher/launcher.cpp")
        self.assertNotIn("Coming later", launcher)
        self.assertNotIn("PageComingLater", launcher)
        self.assertIn("incident_page_->Draw(ui, CurrentReportFacts())", launcher)
        self.assertIn("Describe a problem and send a report with one game log.", launcher)
        self.assertNotIn("PageComingLater", text("launcher/options.h"))

    def test_the_service_and_the_client_agree(self):
        options = text("launcher/options.h")
        self.assertIn("https://trespasser-bug-reports.saulobenigno.workers.dev", options)
        self.assertEqual(re.search(r'name = "([^"]+)"', text("server/trespasser-bug-reports/wrangler.toml")).group(1), "trespasser-bug-reports")
        report = text("launcher/incident_report.h") + text("launcher/incident_report.cpp")
        incidents = text("server/trespasser-bug-reports/src/incidents.js")
        self.assertIn('kTicketPrefix[] = "JPT-"', report)
        self.assertIn("JPT-", incidents)
        self.assertIn("trespasser_recomp_", report)
        log_name = re.search(r"const LOG_FILE = /(.+)/;", incidents)
        self.assertIsNotNone(log_name, "the Worker declares LOG_FILE")
        accepts = re.compile(log_name.group(1)).fullmatch
        for name in ("trespasser_recomp_001.log", "trespasser_recomp_1234.log", "trespasser_linux_001.log", "trespasser_linux_1234.log"):
            self.assertIsNotNone(accepts(name), name)
        for name in ("trespasser_001.log", "trespasser_windows_001.log", "trespasser_recomp_12.log", "trespasser_recomp_12345678901.log",
                     "trespasser_recomp_001.txt", "trespasser_recomp_001.dmp", "../trespasser_recomp_001.log",
                     "logs/trespasser_recomp_001.log", "C:\\logs\\trespasser_recomp_001.log", "/abs/trespasser_recomp_001.log",
                     "trespasser_recomp_001.log\n"):
            self.assertIsNone(accepts(name), name)
        for key in ("description", "launcher_version", "game_build", "game_version", "operating_system", "log_file", "log_clipped", "log_text"):
            self.assertIn(f'"{key}"', report)
            self.assertIn(key, incidents)
        for gone in ("cusa", "eboot"):
            self.assertNotIn(gone, report.lower())
            self.assertNotIn(gone, incidents.lower())

    def test_the_worker_has_the_dragon_layout(self):
        for path in ("src/worker.js", "src/admin.js", "src/admin_page.js", "src/api.js", "src/incidents.js",
                     "src/service.js", "src/sha256.js", "local/host.js", "wrangler.toml", "package.json", "test/api.test.js", "test/incidents.test.js"):
            self.assertTrue((ROOT / "server" / "trespasser-bug-reports" / path).is_file(), path)
        self.assertFalse((ROOT / "server" / "trespasser-bug-reports" / "src" / "reports.js").exists(), "no duplicate simplified reports.js")
        worker = text("tools/worker.ps1")
        self.assertIn("server\\trespasser-bug-reports", worker)
        self.assertIn("-DryRun", worker)
        self.assertTrue((ROOT / "tests" / "worker_script_tests.ps1").is_file())
        self.assertTrue((ROOT / "docs" / "BUG_REPORTS.md").is_file())

    def test_both_platforms_compile_the_page(self):
        windows = text("CMakeLists.txt")
        linux = text("cmake/JptLinux.cmake")
        for source in ("launcher/incident_page.cpp", "launcher/my_reports_page.cpp", "launcher/incident_report.cpp",
                       "launcher/incident_client.cpp", "launcher/http_post.cpp", "launcher/log_scrubber.cpp",
                       "launcher/report_history.cpp", "launcher/json_lite.cpp"):
            self.assertIn(source, windows, source)
            self.assertIn(source, linux, source)

    def test_the_game_log_the_page_reads_is_the_one_the_host_writes(self):
        report = text("launcher/incident_report.cpp")
        for mark in ("[launcher] closed, starting the game", "[loader  ] image mapped at", "[guest   ] entering the CRT entry point"):
            self.assertIn(mark, report)
        self.assertIn('"closed, starting the game"', text("launcher/launcher.cpp"))
        self.assertIn('host_log("guest", "entering the CRT entry point', text("src/host/main.c"))
        self.assertIn("host_log_path()", text("src/host/main.c"))
        self.assertIn("log_file", text("launcher/jpt_launcher.h"))


class PackagingTests(unittest.TestCase):
    def test_the_windows_package_is_built_from_release_and_verified_as_the_asset(self):
        package = text("tools/package.ps1")
        self.assertIn("Config = 'Release'", package)
        self.assertIn("Toolchain = 'clang-cl'", package)
        self.assertIn("clangcl-x86-release", package)
        self.assertNotIn("msvc-x86-release", package, "the release host is the clang-cl build")
        self.assertIn("CMAKE_BUILD_TYPE", package)
        self.assertIn(r"'CMAKE_LINKER', '^lld-link(\.exe)?$'", package)
        self.assertIn("'i686-pc-windows-msvc'", package)
        self.assertIn("config\\release.json", package)
        self.assertIn(r"project\(trespasser_recomp VERSION", package)
        for step in ("vcruntime140.dll", "msvcp140.dll", "licenses", "README.txt", "assets", "Get-Sha256", ".sha256", "verify-release.ps1",
                     "build-updater.ps1", "trg-updater.debug", "updaterBuild"):
            self.assertIn(step, package, step)
        self.assertLess(package.index("& $verify -Path $stage"), package.index("CreateEntryFromFile"))
        self.assertLess(package.index("CreateEntryFromFile"), package.index("& $verify -Path $zip"))
        for forbidden in ("pdb", ".obj", "user_data"):
            self.assertNotIn(f"Copy-Item -LiteralPath (Join-Path $buildDir '{forbidden}", package)
        self.assertNotIn("0.1.0", package)
        wrapper = text("tools/package-release.ps1")
        self.assertIn("package.ps1", wrapper)
        self.assertIn("verify-release.ps1", wrapper)
        self.assertNotRegex(wrapper, r"gh release|git tag|git push|Invoke-WebRequest|upload-artifact")
        self.assertNotRegex(package, r"gh release|git tag|git push|Invoke-WebRequest")

    def test_the_checker_verifies_the_zip_itself(self):
        verify = text("tools/verify-release.ps1")
        for check in ("config\\release.json", ".sha256", "root folder", "unlisted DLL", "unlisted program", "FileVersionInfo", "DEBUG_" + "SAULO", "user_data", "saves", "logs", "game"):
            self.assertIn(check, verify, check)
        cases = text("tools/test-release-check.ps1")
        for case in ("wrong name", "no checksum file", "wrong checksum", "no root folder", "user_data inside", "game folder inside", "missing updater helper", "unlisted DLL",
                     "real program of another version"):
            self.assertIn(case, cases, case)

    def test_the_version_has_one_source(self):
        cmake = text("CMakeLists.txt")
        self.assertRegex(cmake, r"project\(trespasser_recomp VERSION \d+\.\d+\.\d+")
        self.assertIn('JPT_PORT_VERSION="${PROJECT_VERSION}"', cmake)
        self.assertIn("JPT_VERSION_STRING", text("src/trespasser_recomp.rc"))
        self.assertNotRegex(text("src/trespasser_recomp.rc"), r"\d+\.\d+\.\d+")


class UpdaterTests(unittest.TestCase):
    def test_the_launcher_checks_downloads_and_restarts(self):
        launcher = text("launcher/launcher.cpp")
        for call in ("MakeUpdateConfig(InstallDir())", "updater_->StartCheck()", "updater_->StartDownload()", "updater_->BeginInstall()",
                     "DrawUpdateBanner(", "PollUpdate()", "OtherProcessRunning("):
            self.assertIn(call, launcher, call)
        self.assertIn("skip_finish_save_", launcher, "the helper restarts the program: the launcher saves once, not twice")

    def test_the_release_contract(self):
        release = json.loads(text("config/release.json"))
        config = text("launcher/update_config.cpp")
        for macro in ("JPT_RELEASE_REPOSITORY", "JPT_RELEASE_ASSET_PREFIX", "JPT_RELEASE_WINDOWS_SUFFIX", "JPT_RELEASE_LINUX_SUFFIX",
                      "JPT_RELEASE_PROGRAM", "JPT_RELEASE_UPDATER"):
            self.assertIn(macro, config)
        cmake = text("CMakeLists.txt")
        self.assertIn("config/release.json", cmake)
        self.assertIn("generated/jpt_release.h", cmake)
        for value in (release["asset_prefix"], release["windows_suffix"], release["repository"]):
            for path in ("launcher/update_config.cpp", "tools/package.ps1", "tools/package-release.ps1", "tools/verify-release.ps1"):
                self.assertNotIn(value, text(path), f"{path} spells {value} itself: config/release.json is the one source")
        updater = release["updater"]
        if updater.endswith(".exe"):
            updater = updater[:-4]
        self.assertIn("add_executable(" + updater + " WIN32", text("CMakeLists.txt"))
        self.assertEqual(release["program"], "trespasser_recomp.exe")
        self.assertIn(release["updater"], text("tools/test-release-check.ps1"))

    def test_the_helper_has_metadata_and_never_asks_for_elevation(self):
        import xml.etree.ElementTree as ET
        cmake = text("CMakeLists.txt")
        self.assertIn("add_executable(trg-updater WIN32", cmake)
        self.assertIn("launcher/updater/trg-updater.rc", cmake)
        self.assertIn("JPT_UPDATER_HELPER_SOURCES", cmake)
        target = re.search(r"add_executable\(trg-updater[^)]*\)", cmake, re.S)
        self.assertIsNotNone(target)
        self.assertNotIn("trg-updater.manifest", target.group(0), "the resource compiler embeds the manifest")
        self.assertNotIn("trg-updater.rc", target.group(0), "the MSVC target leaves the resource script to the release build")
        self.assertIn("JPT_UPDATER_RELEASE_RESOURCE launcher/updater/trg-updater.rc launcher/updater/trg-updater.manifest", cmake,
                      "the release helper resource has its own list")
        self.assertIn("add_dependencies(trespasser_recomp trg-updater)", cmake)
        # /MANIFEST:EMBED in target_link_options conflicts with CMake Ninja's vs_link_exe and triggers mt.exe c10100a7.
        self.assertNotIn("/MANIFEST:EMBED", cmake, "CMake Ninja vs_link_exe must own manifest embedding")
        self.assertNotIn("requireAdministrator", cmake)
        manifest = text("launcher/updater/trg-updater.manifest")
        root = ET.fromstring(manifest)
        self.assertIn('level="asInvoker"', manifest)
        self.assertIn('uiAccess="false"', manifest)
        self.assertTrue(root.tag.endswith("assembly"))
        rc = text("launcher/updater/trg-updater.rc")
        self.assertIn("VS_VERSION_INFO", rc)
        self.assertIn('RT_MANIFEST "trg-updater.manifest"', rc)
        updater = text("tools/build-updater.ps1")
        for needle in ("MSYS2", "clang++", "llvm-objcopy", "llvm-objdump", "windres", "-mwindows", "-static",
                       "-fuse-ld=lld", "--no-insert-timestamp", "--only-keep-debug", "--strip-debug",
                       "--add-gnu-debuglink", "api-ms-win-", ".gnu_debuglink"):
            self.assertIn(needle, updater, needle)

    def test_protected_paths_are_this_ports(self):
        paths = text("launcher/updater/update_paths.cpp")
        for protected in ('"user_data"', '"saves"', '"versions"', '"trespasser.ini"', '"recomp-registry.txt"', '".iso"'):
            self.assertIn(protected, paths, protected)
        for foreign in ("cusa", "eboot", "trophy", "sce_"):
            self.assertNotIn(foreign, paths.lower(), foreign)

    def test_both_platforms_compile_the_updater(self):
        windows = text("CMakeLists.txt")
        linux = text("cmake/JptLinux.cmake")
        for source in ("launcher/update_config.cpp", "launcher/updater/update_controller.cpp", "launcher/updater/update_zip.cpp",
                       "launcher/updater/version.cpp", "launcher/updater/github_release.cpp"):
            self.assertIn(source, windows, source)
            self.assertIn(source, linux, source)
        self.assertIn("download.cpp", windows)
        self.assertIn("download.cpp", linux)
        self.assertIn("update_tests", windows)

    def test_no_other_port_is_named(self):
        for path in ("launcher/update_config.cpp", "launcher/http_post.cpp", "launcher/incident_report.cpp", "launcher/updater/update_ui.cpp"):
            body = text(path).lower()
            for other in ("13-sentinels", "13sentinels", "dragon", "13_sentinels", "aegis"):
                self.assertNotIn(other, body, f"{path} names {other}")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Linux parity contracts of the port, headless and read only: py -3 -m unittest discover -s tests -p "test_linux_parity.py" -v

The same checks run on Windows and in the Linux gate. They hold what no C suite can: that one application name is used
everywhere, that the portable paths never reach for a system folder, that an AppImage's read-only mount is never a target,
that the Linux executable carries the launcher and the setting options the Windows one has, and that the written inventory
(docs/LINUX_PARITY.md) is the one the feature contract produces.
"""
import json
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gate_support as gs  # noqa: E402


def text(path):
    return gs.strip_comments((ROOT / path).read_text(encoding="utf-8", errors="replace"))


def raw(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def application_id():
    match = re.search(r'#define\s+JPT_APPLICATION_ID\s+"([^"]+)"', raw("src/common/jpt_port.h"))
    assert match, "jpt_port.h defines no JPT_APPLICATION_ID"
    return match.group(1)


def sources(*roots):
    out = []
    for root in roots:
        folder = ROOT / root
        out += sorted(p for p in folder.rglob("*") if p.suffix in (".c", ".cpp", ".h") and "third_party" not in p.parts)
    return out


class ApplicationIdentity(unittest.TestCase):
    def test_application_id_is_one_name(self):
        app_id = application_id()
        self.assertRegex(app_id, r"^io\.github\.saulob\.[a-z0-9-]+$")
        desktop = ROOT / "packaging" / "linux" / f"{app_id}.desktop"
        self.assertTrue(desktop.is_file(), f"{desktop.name} must be named after the application ID")
        main = raw(f"packaging/linux/{desktop.name}").split("[Desktop Action", 1)[0]
        entries = dict(line.split("=", 1) for line in main.splitlines() if "=" in line)
        self.assertEqual(entries.get("StartupWMClass"), app_id)
        self.assertEqual(entries.get("Type"), "Application")
        self.assertEqual(entries.get("Exec"), "AppRun")
        self.assertEqual(entries.get("Terminal"), "false")
        self.assertIn("Game", entries.get("Categories", ""))
        found = set()
        for path in sources("src", "launcher", "tests"):
            found.update(re.findall(r"io\.github\.saulob\.[A-Za-z0-9._-]*[A-Za-z0-9]", path.read_text(encoding="utf-8", errors="replace")))
        self.assertLessEqual(found, {app_id}, f"another application ID is spelled in the sources: {sorted(found - {app_id})}")
        self.assertIn(app_id, raw("docs/LINUX.md"))

    def test_identity_and_icon_reach_both_windows(self):
        window = text("src/linux/linux_sdl_window.c")
        launcher_host = text("launcher/host.cpp")
        self.assertIn("jpt_app_identity_apply", window)
        self.assertIn("jpt_app_icon_apply", window)
        self.assertIn("jpt_app_identity_apply", text("src/linux/linux_main.c"))
        self.assertIn("jpt_app_icon_apply", launcher_host, "the launcher window needs the icon too")
        self.assertIn("jpt_app_identity_apply", launcher_host)
        self.assertTrue((ROOT / "packaging" / "icons" / "app-icon-256.png").is_file())
        self.assertIn("app-icon-256.png", raw("cmake/JptLinux.cmake"), "the icon is embedded, not looked up beside an AppImage")

    def test_apprun_starts_the_executable_and_writes_nothing(self):
        apprun = (ROOT / "packaging" / "linux" / "AppRun").read_bytes()
        self.assertNotIn(b"\r", apprun, "AppRun must have LF line endings")
        body = apprun.decode("utf-8")
        self.assertTrue(body.startswith("#!/bin/sh"))
        self.assertIn('readlink -f "$0"', body)
        self.assertIn('exec "$HERE/usr/bin/trespasser_recomp" "$@"', body)
        for forbidden in ("mkdir", "touch", " cp ", " mv ", " ln ", " rm ", ">>", "HOME=", "XDG_", "cd "):
            self.assertNotIn(forbidden, body, f"AppRun must not use {forbidden!r}: the mount is read only and the program decides the folders")


class PortableFolders(unittest.TestCase):
    FILES = ("src/common/jpt_port.c", "src/common/jpt_user_data.c", "src/linux/linux_paths.c", "src/linux/linux_main.c",
             "src/linux/linux_log.c", "launcher/user_data.cpp", "launcher/launcher_log.cpp", "launcher/settings.cpp")
    FORBIDDEN = (r'getenv\s*\(\s*"HOME"', r"\bXDG_[A-Z_]+", r"\.config\b", r"\.local/share", r"SDL_GetPrefPath", r"SDL_GetUserFolder",
                 r"\bgetpwuid\b", r'"~/', r"%APPDATA%|%LOCALAPPDATA%|SHGetKnownFolderPath")

    def test_no_system_folder_is_chosen(self):
        for path in self.FILES:
            body = text(path)
            for pattern in self.FORBIDDEN:
                self.assertIsNone(re.search(pattern, body), f"{path} reaches for a system folder ({pattern}): user_data, game and logs sit beside the program")

    def test_the_port_folder_has_one_implementation(self):
        self.assertIn("jpt_port_folder(", text("src/linux/linux_paths.c"))
        self.assertIn("jpt_port_folder(", text("launcher/user_data.cpp"))
        self.assertIn("APPIMAGE", text("src/linux/linux_paths.c"))
        self.assertIn("APPIMAGE", text("launcher/user_data.cpp"))

    def test_logs_fall_back_only_inside_an_appimage(self):
        paths = text("src/linux/linux_paths.c")
        self.assertRegex(paths, r"logs_explicit\s*&&|!p->logs_explicit")
        self.assertIn("jpt_port_logs_folder(", paths)
        c = text("src/common/jpt_port.c")
        self.assertIn("in_appimage", c)
        self.assertIn('"logs"', c)

    def test_restart_never_targets_the_mount(self):
        for path in sources("src/linux", "launcher", "src/common"):
            body = text(str(path.relative_to(ROOT)))
            self.assertNotRegex(body, r'exec[vl]p?e?\s*\(\s*"/proc/self/exe"', f"{path.name} would restart the mounted copy")
        paths = text("src/linux/linux_paths.c")
        self.assertRegex(paths, r"jpt_port_relaunch\(\s*getenv\(\"APPIMAGE\"\)")
        self.assertIn("execv(command[0]", paths)

    def test_the_executable_folder_is_only_read(self):
        for path in sources("launcher", "src/linux"):
            rel = path.relative_to(ROOT).as_posix()
            body = text(rel)
            for match in re.finditer(r"SDL_GetBasePath\s*\(\s*\)", body):
                self.assertEqual(rel, "launcher/launcher.cpp", f"{rel} uses the program folder, which is read only inside an AppImage")
        launcher = text("launcher/launcher.cpp")
        start = launcher.index("SDL_GetBasePath")
        window = launcher[start:start + 700]
        self.assertNotRegex(window, r'"(w|wb|a|ab)"|ofstream', "launcher assets are read from the program folder, never written")


class LauncherOnLinux(unittest.TestCase):
    def test_the_executable_carries_and_calls_the_launcher(self):
        cmake = raw("cmake/JptLinux.cmake")
        self.assertIn("jpt_launcher_linux", cmake)
        self.assertIn("JPT_LINUX_LAUNCHER=1", cmake)
        main = text("src/linux/linux_main.c")
        self.assertIn("jpt_launcher_run(", main)
        self.assertIn("--launcher", main)
        self.assertIn("--skip-launcher", main)
        self.assertIn("lx_settings_set_overrides", main)

    def test_the_launcher_sources_are_the_windows_ones(self):
        windows = re.search(r"add_library\(jpt_launcher_core STATIC(.*?)\$\{JPT_PAGE_ICON_SOURCE\}\)", raw("CMakeLists.txt"), re.S)
        linux = re.search(r"add_library\(jpt_launcher_core_linux STATIC(.*?)\$\{JPT_PAGE_ICON_SOURCE\}\)", raw("cmake/JptLinux.cmake"), re.S)
        self.assertTrue(windows and linux)
        names = lambda block: set(re.findall(r"launcher/[\w/]+\.cpp", block.group(1)))  # noqa: E731
        self.assertEqual(names(windows), names(linux), "Windows and Linux compile the same launcher sources")

    def test_the_file_picker_is_checked_before_it_is_started(self):
        launcher = text("launcher/launcher.cpp")
        self.assertLess(launcher.index("FilePickerAvailable()"), launcher.index("trg::BrowseForFile"))
        self.assertIn("zenity", launcher + raw("launcher/localization/pt_br_ui_text.cpp"))
        self.assertIn("kdialog", launcher)
        picker = text("launcher/file_picker.cpp")
        self.assertIn('"zenity"', picker)
        self.assertIn('"kdialog"', picker)
        self.assertIn("X_OK", picker)

    def test_the_launcher_is_usable_with_a_gamepad(self):
        host = text("launcher/host.cpp")
        self.assertIn("ImGuiConfigFlags_NavEnableGamepad", host)
        self.assertIn("SDL_INIT_GAMEPAD", host)

    def test_folders_and_pickers_are_started_without_a_shell_injection(self):
        platform = ROOT / "external" / "trg-launcher" / "src" / "platform.cpp"
        if not platform.is_file():
            self.skipTest("external/trg-launcher is not fetched (tools/fetch_launcher_deps.py)")
        body = gs.strip_comments(platform.read_text(encoding="utf-8", errors="replace"))
        self.assertIn("ShellQuote", body)
        for command in re.findall(r'"(?:xdg-open|zenity|kdialog)[^;\n]*', body):
            self.assertRegex(command, r"ShellQuote|\\?\"$|\"\s*\+|\"\s*$|\s\"$", f"unquoted: {command}")
        self.assertIn("xdg-open \" + ShellQuote(", body)

    def test_the_vulkan_cards_come_from_the_runtime_enumeration(self):
        info = text("launcher/system_info.cpp")
        self.assertIn("jpt_vk_probe_headless", info)
        self.assertIn("msaa_color_counts", info)
        self.assertIn("Vulkan", text("launcher/launcher.cpp"))

    def test_every_installed_version_runs_on_linux(self):
        self.assertNotIn("unavailable_here", text("launcher/launcher.cpp"))
        self.assertNotIn("unavailable_here", text("src/common/jpt_game_version.c"))
        cmake = text("cmake/JptLinux.cmake")
        self.assertIn("add_library(jpt_lifted_objects OBJECT ${GEN_SOURCES})", cmake)
        boot = text("src/linux/linux_boot.c")
        self.assertIn("lx_lift_for_version", boot)
        lift = text("src/linux/linux_lift.c")
        for name in ("lift116", "lift_es117", "lift_ptbr117"):
            self.assertIn(name, lift)


class SettingsReachTheLinuxRuntime(unittest.TestCase):
    def test_every_setting_option_of_the_windows_host_is_accepted(self):
        windows = gs.strip_comments(raw("src/host/main.c"))
        table = re.search(r"settings\[\]\s*=\s*\{(.*?)\};", windows, re.S).group(1)
        expected = dict(re.findall(r'\{\s*"(--[\w-]+)"\s*,\s*(JPT_KEY_\w+)\s*\}', table))
        expected.pop("--renderer")
        linux = gs.strip_comments(raw("src/linux/linux_main.c"))
        block = re.search(r"k_setting_options\[\]\s*=\s*\{(.*?)\};", linux, re.S).group(1)
        found = dict(re.findall(r'\{\s*"(--[\w-]+)"\s*,\s*(JPT_KEY_\w+)\s*\}', block))
        self.assertEqual(found, expected)
        self.assertIn('"--renderer"', linux)
        self.assertIn("TRESPASSER_RENDERER", linux)

    def test_monitor_mode_and_size_are_applied_to_the_window(self):
        screen = text("src/linux/linux_screen.c")
        self.assertIn("window_w", screen)
        self.assertIn("borderless", screen)
        self.assertIn("monitor", screen)
        window = text("src/linux/linux_sdl_window.c")
        self.assertIn("jpt_find_monitor", window)
        self.assertIn("SDL_WINDOWPOS_CENTERED_DISPLAY", window)

    def test_graphics_notice_is_shown_without_an_external_tool(self):
        window = text("src/linux/linux_sdl_window.c")
        self.assertIn("SDL_ShowSimpleMessageBox", window)
        self.assertIn("lx_log_path()", window)
        self.assertIn("was not changed", window)
        self.assertNotRegex(window, r"zenity|kdialog|system\s*\(|popen")
        vulkan = text("src/linux/linux_vulkan.c")
        self.assertIn("Vulkan could not start", vulkan)
        self.assertNotIn("jpt_config_save", vulkan, "a fallback never rewrites the saved renderer")

    def test_gpu_environment_variables_reach_device_selection(self):
        context = text("src/vulkan/vk_context.c")
        for name in ("DRI_PRIME", "MESA_VK_DEVICE_SELECT", "VK_LOADER_DEVICE_SELECT", "__NV_PRIME_RENDER_OFFLOAD"):
            self.assertIn(name, context)


class Inventory(unittest.TestCase):
    def setUp(self):
        self.contract = json.loads(raw("tests/feature_contract.json"))

    def test_no_feature_is_unknown_or_unexplained(self):
        for feature in self.contract["features"]:
            for platform in ("windows", "linux"):
                section = feature[platform]
                self.assertIn(section["status"], ("PASS", "PLATFORM_ONLY", "BLOCKED"), f"{feature['id']} {platform}")
                if section["status"] != "PASS":
                    self.assertTrue(section.get("reason", "").strip(), f"{feature['id']} {platform} is {section['status']} without a reason")
                if feature["scope"] == "global":
                    self.assertEqual(section["status"], "PASS", f"{feature['id']} is global: a gap on {platform} is a failure, not a justification")

    def test_required_features_are_listed(self):
        ids = {feature["id"] for feature in self.contract["features"]}
        required = {"launcher", "launcher-file-picker", "launcher-open-folder", "launcher-gamepad", "port-folder", "appimage-paths",
                    "application-id", "window-icon", "relaunch", "graphics-failure-notice", "monitor-mode-size", "cli-setting-options",
                    "game-version-selection", "handheld-1280x800", "appimage-packaging-files", "vulkan-gpu-list",
                    "steam-deck-appimage", "steam-deck-controller", "steam-deck-1280x800", "steam-deck-gamescope",
                    "steam-deck-paths", "steam-deck-updater",
                    "user-data", "logs", "save-load", "quick-save-load", "vulkan-renderer", "software-renderer", "fullscreen", "gamepad",
                    "keyboard", "mouse", "sdl3-audio", "video-subtitles", "russian-text", "patch-1.1", "ce-features", "minimap", "objectives",
                    "cheats", "frame-counter", "hotkey-table", "crash-diagnostics", "loading-screens", "frontend-hd"}
        self.assertLessEqual(required, ids, f"missing from tests/feature_contract.json: {sorted(required - ids)}")

    def test_the_written_inventory_is_current(self):
        done = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "linux_parity_report.py"), "--check"], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_docs_state_what_they_must(self):
        docs = raw("docs/LINUX.md")
        for needle in ("native x86_64", "Wine", "Proton", "Vulkan", "Software renderer", "glibc 2.31", "X11", "Wayland", "zenity", "kdialog",
                       "xdg-open", "user_data", "logs", "AppImage", "Steam Deck", "Gamescope", "Gaming Mode", "1280x800", "DRI_PRIME",
                       "MESA_VK_DEVICE_SELECT", "VK_LOADER_DEVICE_SELECT", "__NV_PRIME_RENDER_OFFLOAD", "APPIMAGE",
                       "tools/test-linux-parity.sh", "--launcher", "VSync", "controller", "Known limits", "docs/LINUX_PARITY.md", application_id()):
            self.assertIn(needle, docs, f"docs/LINUX.md does not mention {needle}")
        self.assertRegex(docs, r"(?i)not (yet )?validated|has not been validated|unvalidated", "Steam Deck must not be claimed as validated")


class SteamDeckPreparation(unittest.TestCase):
    def test_single_linux_build_for_steam_deck(self):
        cmake = raw("cmake/JptLinux.cmake")
        self.assertNotRegex(cmake, r"(?i)steamdeck|steam_deck|steamworks|libsteam_api")
        self.assertIn("add_executable(trespasser_recomp", cmake)
        for path in sources("src", "launcher"):
            body = text(str(path.relative_to(ROOT)))
            self.assertNotRegex(body, r"steam_api\.h|SteamAPI_Init|SteamInput", f"{path.name} must not depend on Steamworks")

    def test_gamescope_detection_and_lifecycle(self):
        port = text("src/common/jpt_port.c")
        self.assertIn("GAMESCOPE_WAYLAND_DISPLAY", port)
        self.assertIn("jpt_port_is_gamescope", port)
        swap = text("src/vulkan/vk_swapchain.c")
        self.assertIn("jpt_vk_choose_image_count_for", swap)
        self.assertIn("jpt_port_in_gamescope()", swap)
        window = text("src/linux/linux_sdl_window.c")
        for event in ("SDL_EVENT_WINDOW_MINIMIZED", "SDL_EVENT_WINDOW_HIDDEN", "SDL_EVENT_WINDOW_RESTORED", "SDL_EVENT_WINDOW_SHOWN"):
            self.assertIn(event, window)

    def test_launcher_gamepad_and_builtin_browser(self):
        launcher = text("launcher/launcher.cpp")
        self.assertIn("UseBuiltInFileChooser", launcher)
        self.assertIn("disc_browser_.Open(PortFolder())", launcher)
        self.assertIn("disc_browser_.Draw(", launcher)
        self.assertIn("PortFolder()", launcher)
        browser = text("launcher/file_browser.cpp")
        self.assertIn("/run/media", browser)
        self.assertIn("JPT_FILE_CHOOSER", browser)
        self.assertIn("launcher/file_browser.cpp", raw("CMakeLists.txt"))
        self.assertIn("launcher/file_browser.cpp", raw("cmake/JptLinux.cmake"))
        self.assertIn("launcher/disc_browser.cpp", raw("CMakeLists.txt"))
        self.assertIn("launcher/disc_browser.cpp", raw("cmake/JptLinux.cmake"))

    def test_updater_targets_original_appimage(self):
        updater = text("launcher/updater/update_platform.cpp")
        self.assertIn('std::getenv("APPIMAGE")', updater)
        self.assertIn("HasOriginalAppImage", updater)
        self.assertIn("ApplyAppImageUpdate", updater)
        self.assertIn("IsTypeTwoAppImage", updater)
        self.assertIn("PathIsWithin(candidate, mount)", updater)
        cfg = text("launcher/update_config.cpp")
        self.assertIn("HasOriginalAppImage", cfg)
        self.assertIn('"--launcher"', cfg)


class LinuxStartupAndEncoding(unittest.TestCase):
    def test_version_folder_is_mounted_as_c_drive_in_boot_and_main(self):
        boot = text("src/linux/linux_boot.c")
        self.assertIn("jpt_game_version_installed", boot)
        self.assertLess(boot.index("resolve_game_dir"), boot.index("jpt_vpath_mount(vp, 'C'"))
        self.assertIn("lx_features_start(&settings, mount_dir", boot)
        main = text("src/linux/linux_main.c")
        self.assertIn("snprintf(paths->game_dir, sizeof paths->game_dir", main)

    def test_messagebox_converts_to_utf8_and_never_auto_retries(self):
        user32 = text("src/linux/linux_user32.c")
        self.assertIn("lx_text_to_utf8", user32)
        self.assertIn("lx_text_active_codepage", user32)
        mb = re.search(r"static uint64_t u_MessageBoxA\(.*?\n\}", user32, re.S)
        self.assertIsNotNone(mb)
        self.assertNotIn("case 5: return 4", mb.group(0))
        self.assertIn("case 1: case 5: case 6: return 2", mb.group(0))
        text_c = text("src/linux/linux_text.c")
        self.assertIn("jpt_utf8_valid", text_c)
        self.assertIn("jpt_cp_to_utf8", text_c)


if __name__ == "__main__":
    unittest.main()

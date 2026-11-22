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
        entries = dict(line.split("=", 1) for line in raw(f"packaging/linux/{desktop.name}").splitlines() if "=" in line)
        self.assertEqual(entries.get("StartupWMClass"), app_id)
        self.assertEqual(entries.get("Type"), "Application")
        self.assertEqual(entries.get("Exec"), "trespasser_recomp")
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
        self.assertTrue((ROOT / "assets" / "app" / "trespasser-icon.png").is_file())
        self.assertIn("trespasser-icon.png", raw("cmake/JptLinux.cmake"), "the icon is embedded, not looked up beside an AppImage")

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
            rel = str(path.relative_to(ROOT))
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

    def test_an_unavailable_version_is_named_not_started(self):
        self.assertIn("jpt_game_version_unavailable_here", text("launcher/launcher.cpp"))
        self.assertIn("jpt_game_version_unavailable_here", text("src/common/jpt_game_version.c"))


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
                       "xdg-open", "user_data", "logs", "AppImage", "Steam Deck", "1280x800", "DRI_PRIME", "MESA_VK_DEVICE_SELECT",
                       "VK_LOADER_DEVICE_SELECT", "__NV_PRIME_RENDER_OFFLOAD", "APPIMAGE", "tools/test-linux-parity.sh", "--launcher",
                       "VSync", "controller", "Known limits", "docs/LINUX_PARITY.md", application_id()):
            self.assertIn(needle, docs, f"docs/LINUX.md does not mention {needle}")
        self.assertRegex(docs, r"(?i)not (yet )?validated|has not been validated|unvalidated", "Steam Deck must not be claimed as validated")


if __name__ == "__main__":
    unittest.main()

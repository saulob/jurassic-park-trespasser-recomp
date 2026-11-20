#!/usr/bin/env python3
"""The renderer chosen at start stays until exit, headless: py -3 -m unittest discover -s tests -p "test_renderer_fixed.py" """
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "src" / "host"
REMOVED = ("d3dr_set_hardware", "d3dr_defer_switch", "d3dr_pending_switch", "d3dr_directdraw_created",
           "renderer_switch", "pending_switch", "WM_HOST_RENDERER", "KEY_HOST_RENDERER", "guest_restore_direct3d",
           "guest_request_display_restart", "dd_census")
HARDWARE_WRITE = re.compile(r"\bR\.hardware\s*=(?!=)")


def host_sources():
    for path in sorted(HOST.rglob("*")):
        if path.suffix in (".c", ".h", ".cpp"):
            yield path, path.read_text(encoding="utf-8", errors="replace")


class RendererFixed(unittest.TestCase):
    def test_no_runtime_switch_left(self):
        for path, text in host_sources():
            for name in REMOVED:
                self.assertNotIn(name, text, f"{name} in {path.relative_to(ROOT)}")

    def test_renderer_set_once_at_start_up(self):
        text = (HOST / "ddraw" / "d3d11_renderer.c").read_text(encoding="utf-8")
        writes = [m.start() for m in HARDWARE_WRITE.finditer(text)]
        start = text.index("int d3dr_startup(void) {")
        self.assertEqual(len(writes), 1)
        self.assertTrue(start < writes[0] < text.index("\n}\n", start))
        for path, other in host_sources():
            if path.name != "d3d11_renderer.c":
                self.assertIsNone(HARDWARE_WRITE.search(other), str(path.relative_to(ROOT)))

    def test_f5_is_only_the_minimap_toggle(self):
        policy = (HOST / "host_policy.c").read_text(encoding="utf-8")
        self.assertIn("case VK_F5_: return down && !repeat ? KEY_HOST_MINIMAP : KEY_DROP;", policy)
        window = (HOST / "window_host.c").read_text(encoding="utf-8")
        self.assertRegex(window, r"case KEY_HOST_MINIMAP:\s*window_host_toggle_minimap\(\"F5\"\);\s*return 0;")
        self.assertEqual(window.count("case KEY_HOST_MINIMAP:"), 1)
        for path, other in host_sources():
            self.assertNotIn("KEY_HOST_VIDEO_TEST", other, str(path.relative_to(ROOT)))

    def test_use_d3d_answers_the_start_up_choice(self):
        registry = (HOST / "registry.c").read_text(encoding="utf-8")
        branch = registry[registry.index('if (!_stricmp(name, "Use D3D")) {'):]
        branch = branch[:branch.index("return 1;")]
        self.assertIn("d3dr_hardware() ? 1 : 0", branch)


if __name__ == "__main__":
    unittest.main()

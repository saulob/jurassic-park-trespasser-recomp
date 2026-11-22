"""A failed build leaves the previous executable in place; run.ps1 must not run it silently, and the Vulkan bridge must say
at start-up which code the executable holds."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def text(*parts):
    return (ROOT.joinpath(*parts)).read_text(encoding="utf-8", errors="replace")


class RunFreshness(unittest.TestCase):
    def test_run_refuses_an_executable_older_than_the_sources(self):
        run = text("tools", "run.ps1")
        self.assertIn("-AllowStale", run)
        self.assertIn("LastWriteTimeUtc -gt $built", run)
        self.assertLess(run.index("LastWriteTimeUtc -gt $built"), run.index("& $exe @GameArgs"))

    def test_the_vulkan_bridge_logs_its_provenance_and_the_regional_sync(self):
        bridge = text("src", "host", "ddraw", "vk_win_renderer.c")
        self.assertIn("Vulkan build provenance: bridge compiled %s %s", bridge)
        self.assertIn("__DATE__, __TIME__", bridge)
        self.assertIn("regional target sync enabled", bridge)
        self.assertIn("target tiles (32 px)", bridge)

    def test_the_tile_helpers_are_defined_before_they_are_used(self):
        bridge = text("src", "host", "ddraw", "vk_win_renderer.c")
        for name in ("tile_count", "tiles_set_all", "tiles_mark_rect", "tiles_invalidate_all"):
            first = bridge.index(name + "(")
            self.assertTrue(bridge[max(0, first - 12):first].startswith(("static", "size_t", "void")) or "static" in bridge[first - 20:first], name)
            self.assertIn("static", bridge[first - 20:first], name)
        self.assertLess(bridge.index("#define VK_TILE_SHIFT"), bridge.index("static void tiles_mark_rect("))

    def test_the_vulkan_bridge_defers_locks_to_their_first_touch_like_direct3d_11(self):
        bridge = text("src", "host", "ddraw", "vk_win_renderer.c")
        for needle in ("static int defer_lock(", "page_guard_arm_alias", "static void settle_deferred(void)",
                       "V.deferred != t || !page_guard_disarm()", "deferred Locks (first-touch)"):
            self.assertIn(needle, bridge, needle)

if __name__ == "__main__":
    unittest.main()

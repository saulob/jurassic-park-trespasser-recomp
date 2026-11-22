"""
Direct3D 11 against Vulkan on one command stream: renderer_benchmark_d3d11_tests.exe and renderer_benchmark_vulkan_tests.exe issue
the same synthetic Direct3D 6 stream (tests/renderer_workload.h) through their own backend, offscreen, and write their
sample statistics. This test runs both, requires the same stream, prints the medians, p95 and p99 with the Vulkan/Direct3D 11
ratios and fails when Vulkan costs more than a few times Direct3D 11 (a regression of the backend's own overhead).
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("JPT_BUILD_DIR", ROOT / "build" / "cmake" / "msvc-x86"))
EXE = {"d3d11": BUILD / "renderer_benchmark_d3d11_tests.exe", "vulkan": BUILD / "renderer_benchmark_vulkan_tests.exe"}
# Total backend time per frame, Vulkan / Direct3D 11. Measured about 1.2 on the development machine.
MAX_TOTAL_RATIO = 3.0
MAX_CATEGORY_RATIO = 12.0
TOTAL = "total backend time per frame"


def run(backend, *options):
    exe = EXE[backend]
    if not exe.exists():
        raise unittest.SkipTest("%s is not built" % exe.name)
    with tempfile.TemporaryDirectory() as folder:
        out = Path(folder) / "result.txt"
        done = subprocess.run([str(exe), str(out)] + list(options), capture_output=True, text=True, timeout=300)
        text = done.stdout + done.stderr
        if done.returncode != 0:
            raise AssertionError("%s exit %d\n%s" % (exe.name, done.returncode, text))
        if not out.exists():
            raise unittest.SkipTest("%s: no device (%s)" % (exe.name, text.strip().splitlines()[-1] if text.strip() else ""))
        return parse(out.read_text())


def parse(text):
    result = {"metrics": {}, "extra": {}}
    for line in text.splitlines():
        m = re.match(r"stream (\w+) draws (\d+) state_changes (\d+) texture_updates (\d+) polygons (\d+) frames (\d+) warmup (\d+)", line)
        if m:
            result["stream"] = m.group(1)
            result["counts"] = tuple(int(g) for g in m.groups()[1:])
            continue
        m = re.match(r"metric (.+) median ([\d.]+) p95 ([\d.]+) p99 ([\d.]+)", line)
        if m:
            result["metrics"][m.group(1)] = tuple(float(g) for g in m.groups()[1:])
            continue
        m = re.match(r"extra (.+) median ([\d.]+) p95 ([\d.]+) p99 ([\d.]+)", line)
        if m:
            result["extra"][m.group(1)] = tuple(float(g) for g in m.groups()[1:])
    return result


class RendererBenchmark(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d3d11 = run("d3d11")
        cls.vulkan = run("vulkan")

    def test_both_backends_ran_the_same_stream(self):
        self.assertEqual(self.d3d11["stream"], self.vulkan["stream"])
        self.assertEqual(self.d3d11["counts"], self.vulkan["counts"])
        draws, changes, updates, polygons, frames, warmup = self.vulkan["counts"]
        self.assertGreaterEqual(frames, 200)
        self.assertGreaterEqual(warmup, 20)
        self.assertEqual(draws, frames * 4000)
        self.assertGreater(changes, draws)
        self.assertGreater(updates, 0)
        self.assertGreater(polygons, 0)

    def test_a_different_stream_is_noticed(self):
        changed = run("vulkan", "--mutate")
        self.assertNotEqual(changed["stream"], self.d3d11["stream"])

    def test_samples_are_ordered_and_real(self):
        for name, data in (("d3d11", self.d3d11), ("vulkan", self.vulkan)):
            for metric, (median, p95, p99) in data["metrics"].items():
                self.assertGreaterEqual(p95, median, "%s %s" % (name, metric))
                self.assertGreaterEqual(p99, p95, "%s %s" % (name, metric))
            self.assertGreater(data["metrics"][TOTAL][0], 0.05, name)
            self.assertGreater(data["metrics"]["draw translation"][0], 0.0, name) if "draw translation" in data["metrics"] else None

    def test_vulkan_overhead_against_direct3d11(self):
        lines = ["", "%-34s %26s %26s %8s" % ("per frame, ms (median / p95 / p99)", "Direct3D 11", "Vulkan", "ratio")]
        names = list(self.vulkan["metrics"])
        for name in names:
            a, b = self.d3d11["metrics"][name], self.vulkan["metrics"][name]
            ratio = b[0] / a[0] if a[0] > 0 else float("inf")
            lines.append("%-34s %8.3f /%8.3f /%8.3f %8.3f /%8.3f /%8.3f %8.2f" % (name[:34], *a, *b, ratio))
        for name, values in self.vulkan["extra"].items():
            lines.append("%-34s %26s %8.3f /%8.3f /%8.3f" % (name[:34], "-", *values))
        print("\n".join(lines))
        a, b = self.d3d11["metrics"][TOTAL], self.vulkan["metrics"][TOTAL]
        self.assertLessEqual(b[0] / a[0], MAX_TOTAL_RATIO, "Vulkan backend overhead against Direct3D 11 (median)")
        self.assertLessEqual(b[2] / a[2], MAX_TOTAL_RATIO * 1.5, "Vulkan backend overhead against Direct3D 11 (p99)")
        # The category split differs by design: Direct3D 11 pays for its batches inside the state calls, Vulkan inside the
        # draw calls. The sum of the categories is what is comparable, and it is bounded here.
        sum_a = sum(v[0] for k, v in self.d3d11["metrics"].items() if k != TOTAL)
        sum_b = sum(v[0] for k, v in self.vulkan["metrics"].items() if k != TOTAL)
        self.assertLessEqual(sum_b / sum_a, MAX_TOTAL_RATIO)


if __name__ == "__main__":
    sys.exit(unittest.main())

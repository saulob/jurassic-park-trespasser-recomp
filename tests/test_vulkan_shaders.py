#!/usr/bin/env python3
"""Validates the Vulkan 1.2 GLSL shader sources and checked-in SPIR-V bundle (headless, read only):
    py -3 -I -B -m unittest discover -s tests -p "test_vulkan_shaders.py"

Verifies every expected GLSL 450 source exists, the embedded SPIR-V bundle in src/vulkan/vk_shader_spv.inc has valid
SPIR-V magic, version, word alignment, and execution model entry points, and re-validates compilation with
glslangValidator / spirv-val whenever those tools are already present on the machine."""
import hashlib
import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import compile_vk_shaders as cvs  # noqa: E402


class VulkanShaders(unittest.TestCase):
    def test_all_glsl_sources_exist_and_declare_version_450(self):
        for _enum, filename, _stage in cvs.SHADERS:
            path = cvs.SHADER_DIR / filename
            self.assertTrue(path.is_file(), f"missing shader source {path}")
            text = path.read_text(encoding="utf-8")
            self.assertIn("#version 450", text, filename)
            self.assertIn("void main()", text, filename)

    def test_embedded_spirv_bundle_is_valid_and_complete(self):
        self.assertTrue(cvs.OUT_INC.is_file(), "missing src/vulkan/vk_shader_spv.inc")
        text = cvs.OUT_INC.read_text(encoding="utf-8")
        m_hash = re.search(r'#define JPT_VK_SHADER_BUNDLE_SHA256 "([0-9a-f]{64})"', text)
        self.assertIsNotNone(m_hash, "missing 64-hex JPT_VK_SHADER_BUNDLE_SHA256")
        expected_hash = m_hash.group(1)

        hasher = hashlib.sha256()
        for enum_name, filename, stage in cvs.SHADERS:
            sym = f"k_spv_{enum_name.lower()}"
            m = re.search(rf"static const uint32_t {re.escape(sym)}\[(\d+)\] = \{{([^}}]+)\}};", text, re.S)
            self.assertIsNotNone(m, f"missing array {sym} in vk_shader_spv.inc")
            words = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)u", m.group(2))]
            self.assertEqual(len(words), int(m.group(1)), filename)
            raw = struct.pack(f"<{len(words)}I", *words)
            cvs.verify_spirv_header(raw, stage, filename)
            hasher.update(filename.encode("utf-8"))
            hasher.update(b"\0")
            hasher.update(raw)

        self.assertEqual(hasher.hexdigest(), expected_hash, "embedded SPIR-V bundle SHA-256 mismatch")

    def test_glslang_and_spirv_val_match_bundle_when_installed(self):
        if not cvs.find_tool("glslangValidator"):
            return
        compiled, bundle_hash = cvs.compile_all()
        self.assertEqual(len(compiled), len(cvs.SHADERS))
        text = cvs.OUT_INC.read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertEqual(text, cvs.format_inc(compiled, bundle_hash),
                         "src/vulkan/vk_shader_spv.inc is out of date with src/vulkan/shaders/")


if __name__ == "__main__":
    unittest.main()

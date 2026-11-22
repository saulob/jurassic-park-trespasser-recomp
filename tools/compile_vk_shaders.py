#!/usr/bin/env python3
"""Compiles src/vulkan/shaders/*.{vert,frag} to validated Vulkan 1.2 SPIR-V and generates src/vulkan/vk_shader_spv.inc.

    python3 -I tools/compile_vk_shaders.py [--check]

Uses glslangValidator (and spirv-val when available) from PATH or standard tool locations.
Runtime builds consume the checked-in SPIR-V bundle directly with zero runtime shader compiler dependency."""
import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADER_DIR = ROOT / "src" / "vulkan" / "shaders"
OUT_INC = ROOT / "src" / "vulkan" / "vk_shader_spv.inc"

SHADERS = [
    ("TL_VERT", "tl.vert", "vert"),
    ("FIXED_FRAG", "fixed.frag", "frag"),
    ("WATER_FRAG", "water.frag", "frag"),
    ("SKY_FRAG", "sky.frag", "frag"),
    ("QUAD_VERT", "quad.vert", "vert"),
    ("FILL_FRAG", "fill.frag", "frag"),
    ("COPY_FRAG", "copy.frag", "frag"),
    ("COPY_CPU_FRAG", "copy_cpu.frag", "frag"),
    ("PACK565_FRAG", "pack565.frag", "frag"),
    ("PRESENT_FRAG", "present.frag", "frag"),
    ("AO_PREPARE_FRAG", "ao_prepare.frag", "frag"),
    ("AO_PREPARE_MSAA_FRAG", "ao_prepare_msaa.frag", "frag"),
    ("AO_GENERATE_FRAG", "ao_generate.frag", "frag"),
    ("AO_BLUR_FRAG", "ao_blur.frag", "frag"),
    ("AO_APPLY_FRAG", "ao_apply.frag", "frag"),
    ("VIDEO_CONVERT_FRAG", "video_convert.frag", "frag"),
    ("VIDEO_SMOOTH_FRAG", "video_smooth.frag", "frag"),
    ("POLY_VERT", "poly.vert", "vert"),
    ("POLY_TEXTURE_FRAG", "poly_texture.frag", "frag"),
    ("POLY_BUMP_FRAG", "poly_bump.frag", "frag"),
    ("AO_APPLY_MASKED_FRAG", "ao_apply_masked.frag", "frag"),
    ("AO_APPLY_MASKED_MSAA_FRAG", "ao_apply_masked_msaa.frag", "frag"),
    ("VIDEO_VERT", "video.vert", "vert"),
    ("VIDEO_DEBLOCK_X_FRAG", "video_deblock_x.frag", "frag"),
    ("VIDEO_DEBLOCK_Y_FRAG", "video_deblock_y.frag", "frag"),
    ("VIDEO_GLOW_FRAG", "video_glow.frag", "frag"),
    ("VIDEO_FINAL_FRAG", "video_final.frag", "frag"),
    ("UI_VERT", "ui.vert", "vert"),
    ("UI_FRAG", "ui.frag", "frag"),
]

SPIRV_MAGIC = 0x07230203


def find_tool(name: str):
    found = shutil.which(name)
    if found:
        return found
    for candidate in (
        Path(r"C:\msys64\clang64\bin") / f"{name}.exe",
        Path(r"C:\msys64\ucrt64\bin") / f"{name}.exe",
        Path(r"C:\msys64\mingw64\bin") / f"{name}.exe",
        Path("/usr/bin") / name,
    ):
        if candidate.is_file():
            return str(candidate)
    return None


def verify_spirv_header(spv: bytes, stage: str, filename: str):
    if len(spv) < 20 or len(spv) % 4 != 0:
        raise ValueError(f"{filename}: invalid SPIR-V byte length {len(spv)}")
    magic, version, _gen, _bound, _schema = struct.unpack_from("<5I", spv, 0)
    if magic != SPIRV_MAGIC:
        raise ValueError(f"{filename}: bad SPIR-V magic 0x{magic:08x}")
    if version < 0x00010000 or version > 0x00010600:
        raise ValueError(f"{filename}: unexpected SPIR-V version 0x{version:08x}")
    words = struct.unpack(f"<{len(spv) // 4}I", spv)
    expected_model = 0 if stage == "vert" else 4  # Vertex=0, Fragment=4
    found_entry = False
    i = 5
    while i < len(words):
        op = words[i] & 0xFFFF
        wc = words[i] >> 16
        if wc == 0 or i + wc > len(words):
            raise ValueError(f"{filename}: malformed SPIR-V instruction at word {i}")
        if op == 15 and wc >= 4:  # OpEntryPoint
            if words[i + 1] == expected_model:
                found_entry = True
        i += wc
    if not found_entry:
        raise ValueError(f"{filename}: OpEntryPoint missing expected execution model {expected_model}")


def compile_all():
    glslang = find_tool("glslangValidator")
    if not glslang:
        raise RuntimeError("glslangValidator not found in PATH or MSYS2")
    spirv_val = find_tool("spirv-val")

    compiled = []
    bundle_hasher = hashlib.sha256()

    with tempfile.TemporaryDirectory(prefix="jpt_vk_spv_") as td:
        for enum_name, filename, stage in SHADERS:
            src = SHADER_DIR / filename
            if not src.is_file():
                raise FileNotFoundError(f"missing shader source: {src}")
            out_spv = Path(td) / f"{filename}.spv"
            cmd = [glslang, "-V", "--target-env", "vulkan1.2", "-S", stage, "-o", str(out_spv), str(src)]
            done = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if done.returncode != 0:
                raise RuntimeError(f"{filename} compile failed:\n{done.stdout}")
            if spirv_val:
                val = subprocess.run([spirv_val, "--target-env", "vulkan1.2", str(out_spv)],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                if val.returncode != 0:
                    raise RuntimeError(f"{filename} spirv-val failed:\n{val.stdout}")
            data = out_spv.read_bytes()
            verify_spirv_header(data, stage, filename)
            bundle_hasher.update(filename.encode("utf-8"))
            bundle_hasher.update(b"\0")
            bundle_hasher.update(data)
            compiled.append((enum_name, filename, stage, data))

    return compiled, bundle_hasher.hexdigest()


def format_inc(compiled, bundle_hash: str) -> str:
    lines = [
        "/* Generated by tools/compile_vk_shaders.py (Vulkan 1.2 SPIR-V). Do not edit by hand. */",
        f'#define JPT_VK_SHADER_BUNDLE_SHA256 "{bundle_hash}"',
        f"#define JPT_VK_SHADER_COUNT {len(compiled)}",
        "",
        "enum {",
    ]
    for idx, (enum_name, _fn, _st, _data) in enumerate(compiled):
        lines.append(f"    JPT_VK_SPV_{enum_name} = {idx},")
    lines.append("};")
    lines.append("")

    for enum_name, filename, _stage, data in compiled:
        words = struct.unpack(f"<{len(data) // 4}I", data)
        sym = f"k_spv_{enum_name.lower()}"
        lines.append(f"/* {filename}: {len(data)} bytes */")
        lines.append(f"static const uint32_t {sym}[{len(words)}] = {{")
        for row in range(0, len(words), 8):
            chunk = ", ".join(f"0x{w:08x}u" for w in words[row:row + 8])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")

    lines.append("typedef struct {")
    lines.append("    const char* name;")
    lines.append("    const uint32_t* words;")
    lines.append("    size_t byte_size;")
    lines.append("    int is_vertex;")
    lines.append("} jpt_vk_spv_entry_t;")
    lines.append("")
    lines.append("static const jpt_vk_spv_entry_t k_vk_spv_bundle[JPT_VK_SHADER_COUNT] = {")
    for enum_name, filename, stage, data in compiled:
        sym = f"k_spv_{enum_name.lower()}"
        is_v = 1 if stage == "vert" else 0
        lines.append(f'    {{ "{filename}", {sym}, {len(data)}u, {is_v} }},')
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify checked-in vk_shader_spv.inc matches sources")
    args = parser.parse_args()

    compiled, bundle_hash = compile_all()
    text = format_inc(compiled, bundle_hash)

    if args.check:
        if not OUT_INC.is_file():
            print(f"MISSING {OUT_INC}", file=sys.stderr)
            return 1
        existing = OUT_INC.read_text(encoding="utf-8").replace("\r\n", "\n")
        if existing != text:
            print("STALE src/vulkan/vk_shader_spv.inc: re-run tools/compile_vk_shaders.py", file=sys.stderr)
            return 1
        print(f"OK: {len(compiled)} Vulkan 1.2 SPIR-V shaders up to date (sha256={bundle_hash[:16]}...)")
        return 0

    OUT_INC.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT_INC, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"Generated {OUT_INC.relative_to(ROOT)} ({len(compiled)} shaders, sha256={bundle_hash[:16]}...)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

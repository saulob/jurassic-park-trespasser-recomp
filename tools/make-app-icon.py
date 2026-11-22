#!/usr/bin/env python3
"""Regenerates assets/app/trespasser-icon.png, the application icon of the window (needs Pillow).

It is a crop of the port's own launcher header art (assets/launcher/header-art.png, drawn in code by
tools/make-header-art.py: no game or film asset), centred on the setting sun, with rounded corners. 256x256 RGBA.

    py -3 tools/make-app-icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "assets" / "launcher" / "header-art.png"
OUT = ROOT / "assets" / "app" / "trespasser-icon.png"
SIZE = 256
SUN_X = 0.848
RADIUS = 0.18


def main():
    art = Image.open(SOURCE).convert("RGBA")
    width, height = art.size
    left = int(SUN_X * width - height / 2)
    left = max(0, min(width - height, left))
    square = art.crop((left, 0, left + height, height))
    flat = Image.new("RGBA", square.size, (0x10, 0x18, 0x24, 255))
    flat.alpha_composite(square)
    icon = flat.resize((SIZE, SIZE), Image.LANCZOS)
    mask = Image.new("L", (SIZE * 4, SIZE * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, SIZE * 4 - 1, SIZE * 4 - 1), radius=int(SIZE * 4 * RADIUS), fill=255)
    icon.putalpha(mask.resize((SIZE, SIZE), Image.LANCZOS))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    icon.save(OUT, optimize=True)
    print(f"{OUT.relative_to(ROOT)}: {SIZE}x{SIZE}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Regenerates assets/launcher/header-art.png: placeholder art for the launcher header (needs Pillow).

3840x432 RGBA, the same size and alpha ramp as the other ports' header art: transparent up to 22% of the width,
then a smoothstep to full at 55%, so the launcher's gradient shows through behind the title.

    py -3 tools/make-header-art.py
"""
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

WIDTH, HEIGHT = 3840, 432
FADE_START, FADE_END = 0.22, 0.55
OUT = Path(__file__).resolve().parent.parent / "assets" / "launcher" / "header-art.png"


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def sky():
    image = Image.new("RGBA", (WIDTH, HEIGHT))
    top, horizon = (10, 22, 30), (150, 92, 38)
    pixels = image.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            glow = max(0.0, 1.0 - math.hypot((x - 3050) / 1500, (y - 300) / 380))
            base = lerp(top, horizon, (y / HEIGHT) ** 1.6)
            pixels[x, y] = lerp(base, (236, 168, 76), glow * 0.75) + (255,)
    return image


def ridge(draw, rng, base, roughness, color, step=24):
    points = [(0, HEIGHT)]
    y = base
    for x in range(0, WIDTH + step, step):
        y += rng.uniform(-roughness, roughness)
        y = min(max(y, base - 90), base + 40)
        points.append((x, y))
    points.append((WIDTH, HEIGHT))
    draw.polygon(points, fill=color)


def palm(draw, rng, x, ground, height, color):
    lean = rng.uniform(-0.25, 0.25)
    trunk = [(x + lean * height * (t / 10) ** 2, ground - height * t / 10) for t in range(11)]
    for (x0, y0), (x1, y1) in zip(trunk, trunk[1:]):
        draw.line([(x0, y0), (x1, y1)], fill=color, width=max(4, int(height / 28)))
    cx, cy = trunk[-1]
    for i in range(11):
        side = -1 if i % 2 else 1
        spread = rng.uniform(0.15, 1.0)
        length = height * rng.uniform(0.35, 0.62)
        lift = rng.uniform(0.1, 0.45)
        frond = []
        for t in range(17):
            s = t / 16
            fx = cx + side * spread * length * s
            fy = cy - length * lift * s + length * 0.85 * s * s
            frond.append((fx, fy))
        for (x0, y0), (x1, y1) in zip(frond, frond[1:]):
            draw.line([(x0, y0), (x1, y1)], fill=color, width=max(2, int(height / 70)))
        for t, (fx, fy) in enumerate(frond[2:-1], start=2):
            leaf = length * 0.16 * (1 - t / 16)
            draw.line([(fx, fy), (fx + side * leaf * 0.4, fy + leaf)], fill=color, width=2)
            draw.line([(fx, fy), (fx - side * leaf * 0.3, fy + leaf * 0.9)], fill=color, width=2)


def main():
    rng = random.Random(1998)
    image = sky()
    draw = ImageDraw.Draw(image)
    draw.ellipse((2930, 190, 3170, 430), fill=(250, 214, 140, 255))
    ridge(draw, rng, 250, 9, (44, 62, 58, 255))
    ridge(draw, rng, 290, 7, (26, 42, 36, 255))
    mist = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    ImageDraw.Draw(mist).rectangle((0, 300, WIDTH, 340), fill=(220, 190, 150, 70))
    image.alpha_composite(mist.filter(ImageFilter.GaussianBlur(18)))
    ridge(draw, rng, 345, 6, (14, 26, 20, 255))
    for x, height in ((1480, 230), (1610, 300), (2380, 210), (3330, 330), (3480, 260), (3760, 300)):
        palm(draw, rng, x + rng.uniform(-30, 30), HEIGHT + 10, height, (8, 16, 12, 255))
    ridge(draw, rng, 405, 4, (6, 12, 9, 255), step=16)

    alpha = Image.new("L", (WIDTH, HEIGHT))
    column = []
    for x in range(WIDTH):
        u = min(max((x / WIDTH - FADE_START) / (FADE_END - FADE_START), 0.0), 1.0)
        column.append(int(255 * u * u * (3 - 2 * u)))
    alpha.putdata(column * HEIGHT)
    image.putalpha(alpha)
    image.save(OUT, optimize=True)
    print(f"{OUT} {WIDTH}x{HEIGHT}")


if __name__ == "__main__":
    main()

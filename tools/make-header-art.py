#!/usr/bin/env python3
"""Regenerates assets/launcher/header-art.png, the launcher header art (needs Pillow and NumPy).

Original procedural art: an island at dusk seen from the jungle, with a volcanic peak, sea, mist and distant
sauropods. 3840x432 RGBA like the other ports' header art; transparent up to 22% of the width, then a smoothstep to
full at 55%, so the launcher's gradient shows behind the title. Everything left of 58% is sky and low ridges only.

    py -3 tools/make-header-art.py
"""
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

WIDTH, HEIGHT = 3840, 432
SCALE = 2
FADE_START, FADE_END = 0.22, 0.55
SUBJECT_START = 0.58
OUT = Path(__file__).resolve().parent.parent / "assets" / "launcher" / "header-art.png"

W, H = WIDTH * SCALE, HEIGHT * SCALE
HORIZON = 0.745 * H
SUN = (0.848 * W, 0.645 * H)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def paint(canvas, alpha, color):
    a = alpha[..., None]
    canvas *= 1.0 - a
    canvas += a * np.array(color, dtype=np.float32)


def mask_image(draw_fn, blur=0.0):
    layer = Image.new("L", (W, H), 0)
    draw_fn(ImageDraw.Draw(layer))
    if blur:
        layer = layer.filter(ImageFilter.GaussianBlur(blur * SCALE))
    return np.asarray(layer, dtype=np.float32) / 255.0


def sky():
    y = np.linspace(0.0, 1.0, H, dtype=np.float32)[:, None]
    x = np.linspace(0.0, 1.0, W, dtype=np.float32)[None, :]
    stops = [(0.0, (7, 16, 24)), (0.38, (22, 40, 50)), (0.62, (92, 74, 62)), (0.745, (196, 116, 52)),
             (1.0, (120, 70, 40))]
    canvas = np.zeros((H, W, 3), dtype=np.float32)
    for (y0, c0), (y1, c1) in zip(stops, stops[1:]):
        t = np.clip((y - y0) / (y1 - y0), 0.0, 1.0)
        band = (y >= y0) & (y <= y1)
        for i in range(3):
            canvas[..., i] = np.where(band, c0[i] + (c1[i] - c0[i]) * t, canvas[..., i])
    dx, dy = (x * W - SUN[0]) / (0.36 * W), (y * H - SUN[1]) / (0.95 * H)
    glow = np.clip(1.0 - np.sqrt(dx * dx + dy * dy), 0.0, 1.0) ** 1.8
    paint(canvas, glow * 0.85, (255, 186, 98))
    dx, dy = (x * W - SUN[0]) / (0.07 * W), (y * H - SUN[1]) / (0.22 * H)
    paint(canvas, np.clip(1.0 - np.sqrt(dx * dx + dy * dy), 0.0, 1.0) ** 1.2 * 0.7, (255, 222, 160))
    return canvas


def clouds(canvas, rng):
    def draw(d):
        for _ in range(26):
            cx = rng.uniform(0.6, 1.02) * W
            cy = rng.uniform(0.16, 0.56) * H
            rx, ry = rng.uniform(0.04, 0.14) * W, rng.uniform(0.012, 0.03) * H
            d.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=int(rng.uniform(70, 150)))
    lit = mask_image(draw, blur=10)
    fade = np.clip((np.arange(W, dtype=np.float32) / W - SUBJECT_START) / 0.06, 0.0, 1.0)[None, :]
    paint(canvas, lit * fade * 0.55, (232, 150, 96))


def sun_disc(canvas):
    r = 0.075 * H
    paint(canvas, mask_image(lambda d: d.ellipse((SUN[0] - r, SUN[1] - r, SUN[0] + r, SUN[1] + r), fill=255), 1.5),
          (255, 232, 176))


def silhouette(top):
    rows = np.arange(H, dtype=np.float32)[:, None]
    return np.clip(rows - top[None, :] + 0.5, 0.0, 1.0)


def noise_line(rng, base, amplitude, smooth):
    values = np.cumsum(np.array([rng.uniform(-1, 1) for _ in range(W // smooth + 2)], dtype=np.float32))
    values -= np.linspace(values[0], values[-1], len(values))
    line = np.interp(np.arange(W), np.arange(len(values)) * smooth, values).astype(np.float32)
    return base + line / (np.abs(line).max() + 1e-6) * amplitude


def volcano(canvas, rng):
    x = np.arange(W, dtype=np.float32)
    top = np.full(W, float(H), dtype=np.float32)
    for cx, peak, half in ((0.705 * W, 0.30 * H, 0.15 * W), (0.915 * W, 0.56 * H, 0.07 * W),
                           (0.635 * W, 0.60 * H, 0.05 * W)):
        u = np.abs(x - cx) / half
        shape = HORIZON - (HORIZON - peak) * np.exp(-u * u * 1.6) * (1.0 - 0.08 * np.cos(u * 9.0))
        top = np.minimum(top, np.where(u < 2.2, shape, H))
    top += noise_line(rng, 0.0, 0.012 * H, 40)
    paint(canvas, silhouette(top) * 0.92, (88, 70, 72))
    cap = mask_image(lambda d: d.ellipse((0.66 * W, 0.24 * H, 0.76 * W, 0.33 * H), fill=150), blur=14)
    paint(canvas, cap * 0.6, (214, 150, 112))


def sea(canvas, rng):
    rows = np.arange(H, dtype=np.float32)[:, None]
    water = (rows >= HORIZON).astype(np.float32) * np.ones((1, W), dtype=np.float32)
    depth = np.clip((rows - HORIZON) / (H - HORIZON), 0.0, 1.0)
    base = np.stack([150 - 120 * depth, 92 - 70 * depth, 58 - 36 * depth], axis=-1) * np.ones((1, W, 1))
    canvas[:] = canvas * (1.0 - water[..., None]) + base * water[..., None]

    def glints(d):
        y = HORIZON + 2
        while y < H:
            spread = 0.02 * W + (y - HORIZON) * 0.9
            for _ in range(3):
                cx = SUN[0] + rng.gauss(0, spread * 0.45)
                length = rng.uniform(0.2, 1.0) * spread * 0.5
                d.line((cx - length, y, cx + length, y), fill=int(rng.uniform(120, 255)), width=SCALE)
            y += rng.uniform(2.0, 4.5) * SCALE
    paint(canvas, mask_image(glints, blur=0.6) * 0.75, (255, 214, 150))


def canopy_top(rng, baseline, x_from, x_to, rmin, rmax):
    x = np.arange(W, dtype=np.float32)
    top = baseline.copy()
    cx = x_from
    while cx < x_to:
        r = rng.uniform(rmin, rmax)
        cy = np.interp(cx, x, baseline) - r * rng.uniform(0.0, 0.9)
        span = np.abs(x - cx) < r
        if not span.any():
            cx += r
            continue
        dome = cy - np.sqrt(r * r - (x[span] - cx) ** 2) * rng.uniform(0.55, 1.0)
        top[span] = np.minimum(top[span], dome)
        if rng.random() < 0.25:
            spire = np.abs(x - cx) < r * 0.3
            peak = cy - r * rng.uniform(0.9, 1.6)
            top[spire] = np.minimum(top[spire], peak + np.abs(x[spire] - cx) / (r * 0.3) * (cy - peak))
        cx += r * rng.uniform(0.45, 1.0)
    return top


def sauropod(d, x, ground, size, facing):
    def p(dx, dy):
        return (x + facing * dx * size, ground - dy * size)
    body = [p(-0.55, 0.62), p(-0.2, 0.86), p(0.35, 0.82), p(0.62, 0.6), p(0.55, 0.42), p(-0.5, 0.42)]
    d.polygon(body, fill=255)
    neck = [p(0.45, 0.78), p(0.75, 1.25), p(0.98, 1.62), p(1.12, 1.66), p(1.16, 1.58), p(0.98, 1.5), p(0.82, 1.18),
            p(0.6, 0.58)]
    d.polygon(neck, fill=255)
    tail = [p(-0.5, 0.75), p(-1.05, 0.55), p(-1.55, 0.3), p(-1.6, 0.27), p(-1.05, 0.44), p(-0.5, 0.48)]
    d.polygon(tail, fill=255)
    for lx in (-0.42, -0.25, 0.32, 0.48):
        d.rectangle((min(p(lx, 0)[0], p(lx + 0.1, 0)[0]), ground - 0.5 * size, max(p(lx, 0)[0], p(lx + 0.1, 0)[0]),
                     ground), fill=255)


def palm(d, rng, x, ground, height):
    lean = rng.uniform(-0.28, 0.28)
    trunk = [(x + lean * height * (t / 12) ** 2, ground - height * t / 12) for t in range(13)]
    for i, ((x0, y0), (x1, y1)) in enumerate(zip(trunk, trunk[1:])):
        d.line([(x0, y0), (x1, y1)], fill=255, width=max(3, int(height / 26 * (1 - i / 20))))
    cx, cy = trunk[-1]
    for i in range(13):
        angle = math.pi * (i / 12.0) + rng.uniform(-0.12, 0.12)
        length = height * rng.uniform(0.34, 0.56)
        droop = rng.uniform(0.55, 1.1)
        frond = []
        for t in range(19):
            s = t / 18
            fx = cx + math.cos(angle) * length * s
            fy = cy - math.sin(angle) * length * s * 0.55 + length * droop * s * s * 0.75
            frond.append((fx, fy))
        d.line(frond, fill=255, width=max(2, int(height / 90)))
        for t, (fx, fy) in enumerate(frond[2:-1], start=2):
            leaf = length * 0.2 * (1 - t / 19)
            d.line([(fx, fy), (fx + leaf * 0.35, fy + leaf)], fill=255, width=SCALE)
            d.line([(fx, fy), (fx - leaf * 0.35, fy + leaf * 0.95)], fill=255, width=SCALE)
    d.ellipse((cx - height * 0.03, cy - height * 0.02, cx + height * 0.03, cy + height * 0.05), fill=255)


def tree_fern(d, rng, x, ground, height):
    top = (x + rng.uniform(-0.05, 0.05) * height, ground - height * 0.55)
    d.line([(x, ground), top], fill=255, width=max(4, int(height / 18)))
    for i in range(16):
        angle = -math.pi * rng.uniform(0.0, 1.0)
        length = height * rng.uniform(0.35, 0.6)
        frond = []
        for t in range(15):
            s = t / 14
            fx = top[0] + math.cos(angle) * length * s
            fy = top[1] + math.sin(angle) * length * s * 0.6 + length * s * s * 0.65
            frond.append((fx, fy))
        d.line(frond, fill=255, width=max(2, int(height / 80)))
        for fx, fy in frond[1:-1]:
            d.line([(fx, fy), (fx, fy + height * 0.05)], fill=255, width=SCALE)


def main():
    rng = random.Random(1998)
    x = np.arange(W, dtype=np.float32)
    canvas = sky()
    clouds(canvas, rng)
    sun_disc(canvas)
    volcano(canvas, rng)
    sea(canvas, rng)

    sea_gap = (x > 0.745 * W) & (x < 0.9 * W)
    far = canopy_top(rng, noise_line(rng, 0.772 * H, 0.01 * H, 60), 0, W, 3 * SCALE, 11 * SCALE)
    far[sea_gap] = H
    paint(canvas, silhouette(far), (58, 50, 48))
    herd = mask_image(lambda d: (sauropod(d, 0.79 * W, 0.812 * H, 0.072 * H, 1),
                                 sauropod(d, 0.806 * W, 0.816 * H, 0.046 * H, 1)))
    paint(canvas, herd, (40, 34, 34))
    mist = mask_image(lambda d: d.rectangle((0, 0.73 * H, W, 0.8 * H), fill=255), blur=16)
    paint(canvas, mist * 0.3, (226, 170, 120))

    rise = np.clip((x / W - SUBJECT_START) / 0.1, 0.0, 1.0)
    valley = np.exp(-((x / W - 0.81) / 0.06) ** 2)
    mid_base = 0.88 * H - rise * rise * (3 - 2 * rise) * 0.15 * H + valley * 0.19 * H
    mid = canopy_top(rng, mid_base + noise_line(rng, 0.0, 0.01 * H, 50), 0, W, 6 * SCALE, 24 * SCALE)
    paint(canvas, silhouette(mid), (24, 34, 28))
    low_mist = mask_image(lambda d: d.rectangle((0.55 * W, 0.86 * H, W, 0.92 * H), fill=255), blur=12)
    paint(canvas, low_mist * 0.2, (200, 150, 110))

    def jungle(d):
        for px, height in ((0.672, 0.78), (0.9, 1.02), (0.948, 0.84)):
            palm(d, rng, px * W + rng.uniform(-20, 20), H + 6 * SCALE, height * H)
        for fx, height in ((0.615, 0.34), (0.7, 0.4), (0.865, 0.48), (0.99, 0.55)):
            tree_fern(d, rng, fx * W, H + 4 * SCALE, height * H)
    ground = canopy_top(rng, noise_line(rng, 0.962 * H, 0.008 * H, 30), 0, W, 3 * SCALE, 8 * SCALE)
    near = np.maximum(silhouette(ground), mask_image(jungle))
    paint(canvas, near, (6, 12, 9))

    image = Image.fromarray(np.clip(canvas + 0.5, 0, 255).astype(np.uint8), "RGB")
    image = image.resize((WIDTH, HEIGHT), Image.LANCZOS).convert("RGBA")
    alpha = Image.new("L", (WIDTH, HEIGHT))
    column = []
    for px in range(WIDTH):
        u = min(max((px / WIDTH - FADE_START) / (FADE_END - FADE_START), 0.0), 1.0)
        column.append(int(255 * u * u * (3 - 2 * u)))
    alpha.putdata(column * HEIGHT)
    image.putalpha(alpha)
    image.save(OUT, optimize=True)
    print(f"{OUT} {WIDTH}x{HEIGHT}")


if __name__ == "__main__":
    main()

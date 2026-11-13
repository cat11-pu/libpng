#!/usr/bin/env python3
"""Expose the color carried by fully transparent pixels.

Normal alpha compositing hides a fully transparent pixel's R,G,B, which is
exactly why stale palette/tRNS colors go unnoticed until they bleed through an
edge.  This script renders every pixel's *color* channels directly, while
marking fully transparent pixels with a white border, so the buggy driver's
non-zero fringe colors are visible next to the fixed driver's cleared black.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
OUT = os.path.join(ROOT, "out")
VIS = os.path.join(ROOT, "report", "visual")
CELL = 28


def color_map(rgba_path, w, h):
    data = open(rgba_path, "rb").read()
    img = Image.new("RGB", (w * CELL, h * CELL), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    for p in range(w * h):
        r, g, b, a = data[p * 4:p * 4 + 4]
        x, y = (p % w) * CELL, (p // w) * CELL
        draw.rectangle((x, y, x + CELL - 1, y + CELL - 1), fill=(r, g, b))
        if a == 0:
            # white outline marks alpha==0 regardless of the stored color
            draw.rectangle((x, y, x + CELL - 1, y + CELL - 1),
                           outline=(255, 255, 255), width=3)
    return img


def main():
    os.makedirs(VIS, exist_ok=True)
    names = sorted(
        os.path.splitext(f)[0] for f in os.listdir(os.path.join(ROOT, "corpus"))
        if f.endswith(".png"))
    w = h = 16
    for name in names:
        buggy = color_map(os.path.join(OUT, name + ".buggy.rgba"), w, h)
        fixed = color_map(os.path.join(OUT, name + ".fixed.rgba"), w, h)
        strip = Image.new("RGB", (w * CELL * 2 + 8, h * CELL), (128, 128, 128))
        strip.paste(buggy, (0, 0))
        strip.paste(fixed, (w * CELL + 8, 0))
        strip.save(os.path.join(VIS, name + "_fringe_map.png"))
    print("wrote fringe maps to", os.path.relpath(VIS, ROOT))


if __name__ == "__main__":
    main()

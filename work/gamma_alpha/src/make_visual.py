#!/usr/bin/env python3
"""Render visual reproductions of the two defects.

For each corpus image it composites the buggy and fixed RGBA rasters over a
magenta/green checkerboard (so transparent areas are visible), scales the
result up with nearest neighbour, and writes one side-by-side PNG plus a
close-up that focuses on the transparent fringe.
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
OUT = os.path.join(ROOT, "out")
VIS = os.path.join(ROOT, "report", "visual")
SCALE = 16


def checker(w, h, tile=8):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            on = ((x // tile) + (y // tile)) % 2 == 0
            px[x, y] = (220, 0, 220) if on else (0, 170, 170)
    return img


def composite(rgba_path, w, h):
    raw = Image.open(rgba_path) if rgba_path.endswith(".png") else None
    data = open(rgba_path, "rb").read()
    fg = Image.frombytes("RGBA", (w, h), data)
    bg = checker(w, h)
    return Image.alpha_composite(bg.convert("RGBA"), fg).convert("RGB")


def main():
    os.makedirs(VIS, exist_ok=True)
    names = sorted(
        os.path.splitext(f)[0] for f in os.listdir(os.path.join(ROOT, "corpus"))
        if f.endswith(".png"))

    for name in names:
        # width/height are 16 for every corpus image
        w = h = 16
        buggy = composite(os.path.join(OUT, name + ".buggy.rgba"), w, h)
        fixed = composite(os.path.join(OUT, name + ".fixed.rgba"), w, h)

        big_b = buggy.resize((w * SCALE, h * SCALE), Image.NEAREST)
        big_f = fixed.resize((w * SCALE, h * SCALE), Image.NEAREST)

        strip = Image.new("RGB", (w * SCALE * 2 + 8, h * SCALE), (255, 255, 255))
        strip.paste(big_b, (0, 0))
        strip.paste(big_f, (w * SCALE + 8, 0))
        strip.save(os.path.join(VIS, name + "_compare.png"))

        # 4x close-up of the top-left corner where the transparent fringe lives
        corner_b = buggy.crop((0, 0, 8, 8)).resize((8 * 24, 8 * 24), Image.NEAREST)
        corner_f = fixed.crop((0, 0, 8, 8)).resize((8 * 24, 8 * 24), Image.NEAREST)
        closeup = Image.new("RGB", (8 * 24 * 2 + 8, 8 * 24), (255, 255, 255))
        closeup.paste(corner_b, (0, 0))
        closeup.paste(corner_f, (8 * 24 + 8, 0))
        closeup.save(os.path.join(VIS, name + "_fringe_closeup.png"))

    print("wrote", len(names) * 2, "images to", os.path.relpath(VIS, ROOT))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
import json
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from pngcodec import build_png, COLOR_CHANNELS

OUT = os.path.join(os.path.dirname(__file__), "corpus", "valid")


def make_rows(rng, w, h, channels, bit_depth):
    if bit_depth < 8:
        row_bytes = (w * channels * bit_depth + 7) // 8
        mask = (1 << bit_depth) - 1
        rows = []
        for y in range(h):
            row = bytearray(row_bytes)
            for x in range(w):
                for c in range(channels):
                    val = rng.randint(0, mask)
                    bit = (x * channels + c) * bit_depth
                    row[bit // 8] |= val << (8 - bit_depth - bit % 8)
            rows.append(bytes(row))
        return rows
    sample_bytes = bit_depth // 8
    rows = []
    for y in range(h):
        row = bytearray(w * channels * sample_bytes)
        for x in range(w):
            for c in range(channels):
                off = (x * channels + c) * sample_bytes
                val = rng.randrange(1 << bit_depth)
                if bit_depth == 16:
                    row[off:off + 2] = struct.pack(">H", val)
                else:
                    row[off] = val
        rows.append(bytes(row))
    return rows


def main():
    rng = random.Random(20261114)
    os.makedirs(OUT, exist_ok=True)
    combos = []
    for depth, ctypes in ((1, (0, 3)), (2, (0, 3)), (4, (0, 3)),
                          (8, (0, 2, 3, 4, 6)), (16, (0, 2, 4, 6))):
        for ct in ctypes:
            combos.append((depth, ct))
    meta = []
    for i in range(500):
        bit_depth, color_type = combos[i % len(combos)]
        channels = COLOR_CHANNELS[color_type]
        w = rng.randint(1, 64)
        h = rng.randint(1, 64)
        interlace = 1 if rng.random() < 0.25 else 0
        rows = make_rows(rng, w, h, channels, bit_depth)
        palette = trns = None
        extras = []
        if color_type == 3:
            ncol = rng.randint(2, 256)
            palette = bytes(rng.randrange(256) for _ in range(ncol * 3))
            if rng.random() < 0.5:
                trns = bytes(rng.randrange(256) for _ in range(rng.randint(1, ncol)))
        elif color_type in (0, 2) and rng.random() < 0.3:
            n = 2 ** bit_depth
            entries = rng.randint(1, min(n, 16))
            if color_type == 2:
                entries = max(3, entries - (entries % 3))
            trns = b"".join(struct.pack(">H", rng.randrange(1 << bit_depth))
                            for _ in range(entries))
        if rng.random() < 0.4:
            extras.append((b"gAMA", struct.pack(">I", rng.randint(10000, 100000))))
        if rng.random() < 0.3:
            extras.append((b"tIME", struct.pack(">HBBBBB", 2026, 11, 14,
                                                rng.randrange(24), rng.randrange(60),
                                                rng.randrange(60))))
        if rng.random() < 0.25:
            extras.append((b"tEXt", b"Comment\x00fuzzlab generated"))
        data = build_png(w, h, bit_depth, color_type, rows, interlace,
                         palette=palette, trns=trns, extra_chunks=extras,
                         compression_idat=(rng.random() > 0.2))
        name = f"valid_{i:04d}.png"
        with open(os.path.join(OUT, name), "wb") as fh:
            fh.write(data)
        meta.append({"file": name, "w": w, "h": h, "depth": bit_depth,
                     "ctype": color_type, "interlace": interlace,
                     "trns": trns is not None})
    with open(os.path.join(OUT, "manifest.json"), "w") as fh:
        json.dump(meta, fh)
    print(f"wrote {len(meta)} valid PNGs to {OUT}")


if __name__ == "__main__":
    main()

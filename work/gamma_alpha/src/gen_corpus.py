#!/usr/bin/env python3
"""Generate the gamma/transparency reproduction corpus.

The images deliberately combine a non-trivial transfer function (gAMA or
sRGB) with alpha (RGBA, gray+alpha or palette+tRNS) and place fully
transparent pixels whose stored color is saturated.  Those pixels expose any
decoder that fails to clear the color components when alpha is zero.
"""
import os
import struct
import zlib

SIG = b"\x89PNG\r\n\x1a\n"


def chunk(typ, data):
    return (struct.pack(">I", len(data)) + typ + data +
            struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def filter_line(filt, raw, prev, bpp_bytes, stride):
    out = bytearray(stride + 1)
    out[0] = filt
    for i in range(stride):
        x = raw[i]
        a = raw[i - bpp_bytes] if i >= bpp_bytes else 0
        b = prev[i] if prev is not None else 0
        c = prev[i - bpp_bytes] if prev is not None and i >= bpp_bytes else 0
        if filt == 0:
            v = x
        elif filt == 1:
            v = (x - a) & 0xFF
        elif filt == 2:
            v = (x - b) & 0xFF
        elif filt == 3:
            v = (x - ((a + b) >> 1)) & 0xFF
        else:
            v = (x - paeth(a, b, c)) & 0xFF
        out[i + 1] = v
    return bytes(out)


def encode_idat(width, height, channels, bit_depth, rows, interlace=0):
    bpp_bytes = max(1, channels * bit_depth // 8)
    stride = (width * channels * bit_depth + 7) // 8
    parts = bytearray()
    if interlace == 0:
        prev = None
        for y, row in enumerate(rows):
            filt = (y * 7 + 3) % 5  # exercise every filter type deterministically
            parts += filter_line(filt, row, prev, bpp_bytes, stride)
            prev = row
    else:
        # Adam7
        adam7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
                 (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))
        sample_bits = channels * bit_depth
        for x0, y0, dx, dy in adam7:
            pw = (width - x0 + dx - 1) // dx if width > x0 else 0
            ph = (height - y0 + dy - 1) // dy if height > y0 else 0
            if pw == 0 or ph == 0:
                continue
            sub_stride = (pw * sample_bits + 7) // 8
            prev = None
            for py in range(ph):
                full_y = y0 + py * dy
                line = rows[full_y]
                sub = bytearray(sub_stride)
                for px in range(pw):
                    full_x = x0 + px * dx
                    src = full_x * sample_bits
                    dst = px * sample_bits
                    for k in range(sample_bits):
                        if (line[(src + k) // 8] >> (7 - (src + k) % 8)) & 1:
                            sub[(dst + k) // 8] |= 1 << (7 - (dst + k) % 8)
                parts += filter_line(py % 5, bytes(sub), prev,
                                     bpp_bytes, sub_stride)
                prev = bytes(sub)
    return zlib.compress(bytes(parts), 9)


def write_png(path, width, height, bit_depth, color_type, rows,
              interlace=0, palette=None, trns=None, gama=None, srgb=False):
    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type,
                       0, 0, interlace)
    out = bytearray(SIG)
    out += chunk(b"IHDR", ihdr)
    if srgb:
        # rendering intent 0 (perceptual)
        out += chunk(b"sRGB", struct.pack(">B", 0))
        out += chunk(b"gAMA", struct.pack(">I", 45455))
    elif gama is not None:
        out += chunk(b"gAMA", struct.pack(">I", int(round(gama * 100000))))
    if palette is not None:
        out += chunk(b"PLTE", palette)
    if trns is not None:
        out += chunk(b"tRNS", trns)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    out += chunk(b"IDAT", encode_idat(width, height, channels,
                                      bit_depth, rows, interlace))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(bytes(out))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    corpus = os.path.join(here, "..", "corpus")
    os.makedirs(corpus, exist_ok=True)
    w = h = 16

    # ---- 1. 8-bit RGBA, gAMA 0.45455 (file gamma 2.2), with zero-alpha fringe
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            edge = (x == 0 or y == 0 or x == w - 1 or y == h - 1)
            if edge:
                # fully transparent but carrying a saturated, non-zero color
                row += bytes([255, 0, 128, 0])
            else:
                r = (x * 17) & 0xFF
                g = (y * 17) & 0xFF
                b = ((x + y) * 9) & 0xFF
                a = 64 + ((x * y) % 192)
                row += bytes([r, g, b, a])
        rows.append(bytes(b"\x00" + row) if False else row)
    raw_rows = [bytes(r) for r in rows]
    write_png(os.path.join(corpus, "rgba8_gama22.png"), w, h, 8, 6,
              raw_rows, gama=45455 / 100000.0)

    # ---- 2. 8-bit RGBA, sRGB chunk, interlaced, zero-alpha fringe
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if (x + y) % 5 == 0:
                row += bytes([0, 255, 255, 0])  # transparent cyan fringe
            else:
                row += bytes([(x * 13) & 0xFF, (y * 11) & 0xFF,
                              (x ^ y) & 0xFF, 255])
        rows.append(bytes(row))
    write_png(os.path.join(corpus, "rgba8_srgb_adam7.png"), w, h, 8, 6,
              rows, interlace=1, srgb=True)

    # ---- 3. 16-bit RGBA, gAMA 0.45455, zero-alpha fringe
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if x == w // 2:
                row += struct.pack(">HHHH", 60000, 1000, 30000, 0)
            else:
                row += struct.pack(
                    ">HHHH",
                    (x * 4000) & 0xFFFF, (y * 4000) & 0xFFFF,
                    ((x + y) * 2000) & 0xFFFF,
                    1000 + ((x * y) % 64000))
        rows.append(bytes(row))
    write_png(os.path.join(corpus, "rgba16_gama22.png"), w, h, 16, 6,
              rows, gama=45455 / 100000.0)

    # ---- 4. 8-bit gray+alpha, gAMA 1.0 (linear), zero-alpha fringe
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if y < 2:
                row += bytes([255, 0])  # transparent white fringe
            else:
                row += bytes([(x * 17) & 0xFF, 128 + (x % 128)])
        rows.append(bytes(row))
    write_png(os.path.join(corpus, "ga8_linear.png"), w, h, 8, 4,
              rows, gama=1.0)

    # ---- 5. palette + tRNS, transparent palette entry holds a red color
    palette = bytes([
        255, 0, 0,      # index 0: red, will be transparent
        0, 255, 0,      # index 1: green, opaque
        0, 0, 255,      # index 2: blue, semi-transparent
        255, 255, 0,    # index 3: yellow, opaque
    ])
    trns = bytes([0, 255, 128])  # entry 0 transparent, 2 half alpha
    idx_rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if x == 0 or x == w - 1:
                row.append(0)  # transparent red
            else:
                row.append((x + y) % 4)
        idx_rows.append(bytes(row))
    write_png(os.path.join(corpus, "palette8_trns.png"), w, h, 8, 3,
              idx_rows, palette=palette, trns=trns, gama=45455 / 100000.0)

    # ---- 6. RGB + tRNS (single transparent color), gAMA 2.2 file
    trans_color = (200, 100, 50)
    rgb_rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if y == h - 1:
                row += bytes(trans_color)  # becomes fully transparent
            else:
                row += bytes([(x * 15) & 0xFF, (y * 15) & 0xFF,
                              (x * y) & 0xFF])
        rgb_rows.append(bytes(row))
    trns16 = struct.pack(">HHH", *trans_color)
    write_png(os.path.join(corpus, "rgb8_trns_gama22.png"), w, h, 8, 2,
              rgb_rows, trns=trns16, gama=45455 / 100000.0)

    # ---- 7. gray + tRNS (single transparent gray level), gAMA 2.2 file
    trans_gray = 128
    gray_rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if x == 0:
                row.append(trans_gray)  # matches tRNS -> fully transparent
            else:
                row.append((y * 16 + x) & 0xFF)
        gray_rows.append(bytes(row))
    write_png(os.path.join(corpus, "gray8_trns_gama22.png"), w, h, 8, 0,
              gray_rows, trns=struct.pack(">H", trans_gray),
              gama=45455 / 100000.0)

    print("generated", len(os.listdir(corpus)), "files in", corpus)


if __name__ == "__main__":
    main()

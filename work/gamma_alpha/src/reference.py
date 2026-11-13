#!/usr/bin/env python3
"""Independent reference PNG decoder used as the pixel-level ground truth.

This decoder is intentionally written from scratch and does not link libpng.
It supports the subset produced by gen_corpus.py (8/16-bit, gray/RGB/palette,
optional alpha, tRNS, gAMA/sRGB, Adam7) and implements:

  * correct PNG un-filtering (None/Sub/Up/Average/Paeth) and Adam7 placement;
  * the libpng-compatible gamma table used for an sRGB-encoded output;
  * clearing of R,G,B to zero for every fully transparent pixel.

Output matches a correctly configured libpng driver byte for byte on 8-bit
components (see compare.py).
"""
import struct
import zlib


def _chunks(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        yield typ, body
        pos += 12 + length


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter(raw, stride, bpp):
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    while pos < len(raw):
        filt = raw[pos]
        line = raw[pos + 1:pos + 1 + stride]
        pos += 1 + stride
        cur = bytearray(stride)
        for i in range(stride):
            x = line[i]
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if filt == 0:
                v = x
            elif filt == 1:
                v = (x + a) & 0xFF
            elif filt == 2:
                v = (x + b) & 0xFF
            elif filt == 3:
                v = (x + ((a + b) >> 1)) & 0xFF
            else:
                v = (x + _paeth(a, b, c)) & 0xFF
            cur[i] = v
        out += cur
        prev = cur
    return bytes(out)


ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
         (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


def _gamma_table_8bit(file_gama, screen_gamma):
    """Replicate libpng's 8-bit gamma table for an sRGB output.

    libpng derives the table exponent from ``png_reciprocal2(file, screen)``
    as ``1 / (file_gama * screen_gama)`` and builds
    ``out = 255 * (v/255) ** exponent``.  For an sRGB-encoded file
    (gAMA 0.45455) rendered to an sRGB output (screen 2.2) the exponent is 1,
    so samples pass through unchanged.
    """
    exponent = 1.0 / (file_gama * screen_gamma)
    table = [0] * 256
    for v in range(256):
        if 0 < v < 255:
            table[v] = int(255.0 * ((v / 255.0) ** exponent) + 0.5)
        else:
            table[v] = v
    return table


def decode(path):
    with open(path, "rb") as f:
        data = f.read()

    width = height = bit_depth = color_type = interlace = 0
    palette = trns = idat = None
    # gAMA exponent (libpng colorspace.gamma); sRGB and the "no chunk" default
    # both resolve to 0.45455.  The output table uses screen_gamma / gAMA.
    file_gama = 1.0 / 2.2
    have_gamma = False

    for typ, body in _chunks(data):
        if typ == b"IHDR":
            (width, height, bit_depth, color_type, comp, filt,
             interlace) = struct.unpack(">IIBBBBB", body)
        elif typ == b"sRGB":
            file_gama = 1.0 / 2.2
            have_gamma = True
        elif typ == b"gAMA":
            if not have_gamma:
                file_gama = struct.unpack(">I", body)[0] / 100000.0
                have_gamma = True
        elif typ == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat = body if idat is None else idat + body
        elif typ == b"IEND":
            break

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(idat)

    # Assemble per-pixel samples in the file's native layout first.
    pixels = [[None] * width for _ in range(height)]

    # zlib stores all Adam7 passes (or the single non-interlaced pass) as one
    # concatenated filtered stream; split it using the known per-pass sizes.
    passes = ADAM7 if interlace == 1 else ((0, 0, 1, 1),)
    pass_slices = []
    cursor = 0
    for x0, y0, dx, dy in passes:
        pw = (width - x0 + dx - 1) // dx if width > x0 else 0
        ph = (height - y0 + dy - 1) // dy if height > y0 else 0
        if pw == 0 or ph == 0:
            pass_slices.append(None)
            continue
        bits_per_pixel = channels * bit_depth
        stride = (pw * bits_per_pixel + 7) // 8
        bpp = max(1, bits_per_pixel // 8)
        size = ph * (stride + 1)
        pass_slices.append((x0, y0, dx, dy, pw, ph, stride, bpp,
                            raw[cursor:cursor + size]))
        cursor += size

    for sl in pass_slices:
        if sl is None:
            continue
        x0, y0, dx, dy, pw, ph, stride, bpp, chunk_raw = sl
        unfiltered = _unfilter(chunk_raw, stride, bpp)
        for py in range(ph):
            line = unfiltered[py * stride:(py + 1) * stride]
            for px in range(pw):
                samples = _read_samples(line, px, channels, bit_depth)
                pixels[y0 + py * dy][x0 + px * dx] = samples

    # Expand to RGBA8.
    table = _gamma_table_8bit(file_gama, 2.2)
    out = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            s = pixels[y][x]
            r = g = b = a = 0
            if color_type == 6:  # RGBA
                if bit_depth == 16:
                    # big-endian 16-bit samples; take the high byte to match
                    # libpng's png_set_strip_16 (high-byte truncation)
                    r, g, b, a = s[0], s[2], s[4], s[6]
                else:
                    r, g, b, a = s
            elif color_type == 2:  # RGB (+tRNS)
                r, g, b = s
                a = 255
                if trns is not None:
                    tr, tg, tb = struct.unpack(">HHH", trns[:6])
                    # tRNS for 8-bit RGB stores the exact sample values in the
                    # low byte (high byte is 0); match before gamma correction.
                    if (r, g, b) == (tr & 0xFF, tg & 0xFF, tb & 0xFF):
                        a = 0
            elif color_type == 4:  # gray+alpha
                if bit_depth == 16:
                    r = g = b = s[0]
                    a = s[2]
                else:
                    r = g = b = s[0]
                    a = s[1]
            elif color_type == 0:  # gray (+tRNS)
                r = g = b = s[0]
                a = 255
                if trns is not None:
                    # tRNS stores the gray level in the same precision as the
                    # image samples; for 8-bit input the value is in the low
                    # byte.  Match before gamma correction.
                    tg = struct.unpack(">H", trns[:2])[0]
                    target = tg if bit_depth == 16 else tg & 0xFF
                    if s[0] == target:
                        a = 0
            elif color_type == 3:  # palette
                idx = s[0]
                r, g, b = palette[idx]
                a = trns[idx] if trns is not None and idx < len(trns) else 255

            # Gamma-correct color components only (alpha stays linear).
            r = table[r]
            g = table[g]
            b = table[b]

            if a == 0:
                r = g = b = 0

            q = (y * width + x) * 4
            out[q:q + 4] = bytes((r, g, b, a))

    return width, height, bytes(out)
def _read_samples(line, px, channels, bit_depth):
    if bit_depth >= 8:
        bpp = channels * bit_depth // 8
        return list(line[px * bpp:(px + 1) * bpp])
    # sub-byte packed samples (palette / gray), one sample per pixel
    bits = bit_depth
    start = px * bits
    byte = line[start // 8]
    shift = 8 - bits - (start % 8)
    mask = (1 << bits) - 1
    return [(byte >> shift) & mask]


if __name__ == "__main__":
    import sys
    w, h, rgba = decode(sys.argv[1])
    with open(sys.argv[2], "wb") as f:
        f.write(rgba)
    print(f"reference {sys.argv[1]} {w}x{h} -> {sys.argv[2]}")

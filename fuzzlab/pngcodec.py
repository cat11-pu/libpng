#!/usr/bin/env python3
"""Small independent PNG encoder used by the fuzz corpus generator."""

import struct
import zlib

SIG = b"\x89PNG\r\n\x1a\n"
COLOR_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
         (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


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


def filter_scanline(filt, raw, prev, bpp, stride):
    out = bytearray(stride + 1)
    out[0] = filt
    bpp_bytes = max(1, bpp // 8)
    for i in range(stride):
        x = raw[i]
        a = raw[i - bpp_bytes] if i >= bpp_bytes else 0
        b = prev[i] if prev is not None else 0
        c = prev[i - bpp_bytes] if prev is not None and i >= bpp_bytes else 0
        if filt == 0:
            value = x
        elif filt == 1:
            value = (x - a) & 0xFF
        elif filt == 2:
            value = (x - b) & 0xFF
        elif filt == 3:
            value = (x - ((a + b) >> 1)) & 0xFF
        else:
            value = (x - paeth(a, b, c)) & 0xFF
        out[i + 1] = value
    return bytes(out)


def adam7_passes(width, height):
    for x0, y0, dx, dy in ADAM7:
        pw = (width - x0 + dx - 1) // dx if width > x0 else 0
        ph = (height - y0 + dy - 1) // dy if height > y0 else 0
        yield x0, y0, dx, dy, pw, ph


def _filter_picker():
    state = [0x12345678]

    def pick(y):
        state[0] = (state[0] * 1103515245 + 12345 + y) & 0x7FFFFFFF
        return state[0] % 5

    return pick


def encode_idat(width, height, channels, bit_depth, rows, interlace):
    bpp = channels * bit_depth
    pick = _filter_picker()
    if not interlace:
        stride = (width * channels * bit_depth + 7) // 8
        raw = bytearray()
        prev = None
        for y in range(height):
            raw += filter_scanline(pick(y), rows[y], prev, bpp, stride)
            prev = rows[y]
        return zlib.compress(bytes(raw), 6)

    raw = bytearray()
    for x0, y0, dx, dy, pw, ph in adam7_passes(width, height):
        if pw == 0 or ph == 0:
            continue
        sub_stride = (pw * channels * bit_depth + 7) // 8
        prev = None
        for py in range(ph):
            line = rows[y0 + py * dy]
            sub = bytearray(sub_stride)
            sample_bits = channels * bit_depth
            for px in range(pw):
                src_bit = (x0 + px * dx) * sample_bits
                dst_bit = px * sample_bits
                for k in range(sample_bits):
                    sb, db = src_bit + k, dst_bit + k
                    if (line[sb // 8] >> (7 - sb % 8)) & 1:
                        sub[db // 8] |= 1 << (7 - db % 8)
            raw += filter_scanline(pick(py), bytes(sub), prev, bpp, sub_stride)
            prev = bytes(sub)
    return zlib.compress(bytes(raw), 6)


def build_png(width, height, bit_depth, color_type, rows, interlace=0,
              palette=None, trns=None, extra_chunks=()):
    channels = COLOR_CHANNELS[color_type]
    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type,
                       0, 0, interlace)
    out = bytearray(SIG)
    out += chunk(b"IHDR", ihdr)
    if palette is not None:
        out += chunk(b"PLTE", palette)
    if trns is not None:
        out += chunk(b"tRNS", trns)
    for typ, data in extra_chunks:
        out += chunk(typ, data)
    out += chunk(b"IDAT",
                 encode_idat(width, height, channels, bit_depth, rows,
                             interlace))
    out += chunk(b"IEND", b"")
    return bytes(out)


def iter_chunks(data):
    if data[:8] != SIG:
        raise ValueError("not a PNG")
    pos = 8
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        yield pos, length, typ, body
        pos += 12 + length


def rebuild(parts):
    return SIG + b"".join(chunk(typ, body) for typ, body in parts)

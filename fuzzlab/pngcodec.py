#!/usr/bin/env python3
"""Minimal pure-Python PNG encoder used to build the test corpora."""
import struct
import zlib

SIG = b"\x89PNG\r\n\x1a\n"


def chunk(typ, data):
    return (struct.pack(">I", len(data)) + typ + data +
            struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))


COLOR_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


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


ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
         (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


def adam7_passes(w, h):
    for x0, y0, dx, dy in ADAM7:
        pw = (w - x0 + dx - 1) // dx if w > x0 else 0
        ph = (h - y0 + dy - 1) // dy if h > y0 else 0
        yield x0, y0, dx, dy, pw, ph


def encode_idat(w, h, channels, bit_depth, rows, interlace, filter_picker):
    bpp = channels * bit_depth
    if not interlace:
        stride = (w * channels * bit_depth + 7) // 8
        buf = bytearray()
        prev = None
        for y in range(h):
            filt = filter_picker(y)
            buf += filter_scanline(filt, rows[y], prev, bpp, stride)
            prev = rows[y]
        return zlib.compress(bytes(buf), 6)
    parts = bytearray()
    for x0, y0, dx, dy, pw, ph in adam7_passes(w, h):
        if pw == 0 or ph == 0:
            continue
        sub_stride = (pw * channels * bit_depth + 7) // 8
        prev = None
        for py in range(ph):
            full_y = y0 + py * dy
            line = rows[full_y]
            sub = bytearray(sub_stride)
            sample_bits = channels * bit_depth
            for px in range(pw):
                full_x = x0 + px * dx
                src_bit = full_x * sample_bits
                dst_bit = px * sample_bits
                for k in range(sample_bits):
                    sb, db = src_bit + k, dst_bit + k
                    if (line[sb // 8] >> (7 - sb % 8)) & 1:
                        sub[db // 8] |= 1 << (7 - db % 8)
            filt = filter_picker(py)
            parts += filter_scanline(filt, bytes(sub), prev, bpp, sub_stride)
            prev = bytes(sub)
    return zlib.compress(bytes(parts), 6)


def build_png(w, h, bit_depth, color_type, raw_rows, interlace=0,
              palette=None, trns=None, extra_chunks=(), compression_idat=True):
    channels = COLOR_CHANNELS[color_type]
    ihdr = struct.pack(">IIBBBBB", w, h, bit_depth, color_type, 0, 0, interlace)
    out = bytearray(SIG)
    out += chunk(b"IHDR", ihdr)
    if palette is not None:
        out += chunk(b"PLTE", palette)
    if trns is not None:
        out += chunk(b"tRNS", trns)
    for typ, data in extra_chunks:
        out += chunk(typ, data)
    rng_pick = _filter_rng()
    idat = encode_idat(w, h, channels, bit_depth, raw_rows, interlace, rng_pick)
    out += chunk(b"IDAT", idat)
    out += chunk(b"IEND", b"")
    return bytes(out)


def _filter_rng():
    state = [0x12345678]
    def pick(y):
        state[0] = (state[0] * 1103515245 + 12345 + y) & 0x7FFFFFFF
        return state[0] % 5
    return pick


def _stored_zlib(data):
    return zlib.compress(data, 1)


def iter_chunks(data):
    assert data[:8] == SIG
    p = 8
    while p + 12 <= len(data):
        length = struct.unpack(">I", data[p:p + 4])[0]
        typ = data[p + 4:p + 8]
        body = data[p + 8:p + 8 + length]
        yield p, length, typ, body
        p += 12 + length


def rebuild(chunks):
    out = bytearray(SIG)
    for typ, body in chunks:
        out += chunk(typ, body)
    return bytes(out)

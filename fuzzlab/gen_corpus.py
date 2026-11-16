#!/usr/bin/env python3
"""Generate deterministic valid and malformed PNG corpora plus small samples."""

import json
import os
import random
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pngcodec import (SIG, build_png, chunk, iter_chunks, rebuild,
                      COLOR_CHANNELS)

HERE = os.path.dirname(__file__)
VALID = os.path.join(HERE, "corpus", "valid")
MALFORMED = os.path.join(HERE, "corpus", "malformed")
SAMPLES = os.path.join(HERE, "samples")


def write(path, data):
    with open(path, "wb") as fh:
        fh.write(data)


def base_png(rng, width=None, height=None, interlace=None):
    width = width or rng.randint(2, 48)
    height = height or rng.randint(2, 48)
    depth = rng.choice([1, 2, 4, 8])
    color_type = rng.choice([0, 2, 3, 4, 6]) if depth == 8 else \
        rng.choice([0, 3])
    channels = COLOR_CHANNELS[color_type]
    palette = None
    if color_type == 3:
        ncol = 2 ** depth
        palette = bytes(rng.randrange(256) for _ in range(ncol * 3))
    rows = make_rows(rng, width, height, channels, depth, color_type,
                     palette)
    if interlace is None:
        interlace = 1 if rng.random() < 0.2 else 0
    data = build_png(width, height, depth, color_type, rows, interlace,
                     palette=palette)
    return data, {"w": width, "h": height, "depth": depth,
                  "ctype": color_type, "interlace": interlace}


def make_rows(rng, width, height, channels, depth, color_type, palette):
    ncol = len(palette) // 3 if palette is not None else 0
    if depth < 8:
        row_bytes = (width * channels * depth + 7) // 8
        rows = []
        for _ in range(height):
            row = bytearray(row_bytes)
            for x in range(width):
                for c in range(channels):
                    value = rng.randrange(ncol) if color_type == 3 else \
                        rng.randrange(1 << depth)
                    bit = (x * channels + c) * depth
                    row[bit // 8] |= value << (8 - depth - bit % 8)
            rows.append(bytes(row))
        return rows

    sample_bytes = depth // 8
    rows = []
    for _ in range(height):
        row = bytearray(width * channels * sample_bytes)
        for x in range(width):
            for c in range(channels):
                off = (x * channels + c) * sample_bytes
                value = rng.randrange(ncol) if color_type == 3 else \
                    rng.randrange(1 << depth)
                if depth == 16:
                    row[off:off + 2] = struct.pack(">H", value)
                else:
                    row[off] = value
        rows.append(bytes(row))
    return rows


def idat_body(data):
    for _, _, typ, body in iter_chunks(data):
        if typ == b"IDAT":
            return body
    raise ValueError("no IDAT")


def replace_idat(data, body):
    out = bytearray(SIG)
    replaced = False
    for _, _, typ, old in iter_chunks(data):
        if typ == b"IHDR" and not replaced:
            out += chunk(typ, old)
        elif typ == b"IDAT" and not replaced:
            out += chunk(b"IDAT", body)
            replaced = True
        elif typ != b"IDAT":
            out += chunk(typ, old)
    return bytes(out)


def gen_valid(rng):
    os.makedirs(VALID, exist_ok=True)
    combos = []
    for depth, ctypes in ((1, (0, 3)), (2, (0, 3)), (4, (0, 3)),
                          (8, (0, 2, 3, 4, 6)), (16, (0, 2, 4, 6))):
        for color_type in ctypes:
            combos.append((depth, color_type))

    manifest = []
    for index in range(500):
        depth, color_type = combos[index % len(combos)]
        channels = COLOR_CHANNELS[color_type]
        width = rng.randint(1, 64)
        height = rng.randint(1, 64)
        interlace = 1 if rng.random() < 0.25 else 0
        palette = trns = None
        extras = []
        if color_type == 3:
            ncol = max(2, min(2 ** depth, width * height))
            palette = bytes(rng.randrange(256) for _ in range(ncol * 3))
            if rng.random() < 0.4:
                trns = bytes(rng.randrange(256)
                             for _ in range(rng.randint(1, ncol)))
        elif color_type in (0, 2) and rng.random() < 0.3:
            entries = rng.randint(1, min(2 ** depth, 16))
            if color_type == 2:
                entries = max(3, entries - entries % 3)
            trns = b"".join(struct.pack(">H", rng.randrange(1 << depth))
                            for _ in range(entries))
        if rng.random() < 0.4:
            extras.append((b"gAMA",
                           struct.pack(">I", rng.randint(10000, 100000))))
        if rng.random() < 0.25:
            extras.append((b"tEXt", b"Comment\x00fuzzlab generated"))

        rows = make_rows(rng, width, height, channels, depth, color_type,
                         palette)
        data = build_png(width, height, depth, color_type, rows, interlace,
                         palette=palette, trns=trns, extra_chunks=extras)
        name = f"valid_{index:04d}.png"
        write(os.path.join(VALID, name), data)
        manifest.append({"file": name, "w": width, "h": height,
                         "depth": depth, "ctype": color_type,
                         "interlace": interlace,
                         "trns": trns is not None})

    with open(os.path.join(VALID, "manifest.json"), "w") as fh:
        json.dump(manifest, fh)


def gen_truncation(rng):
    data, meta = base_png(rng)
    variant = rng.randrange(5)
    if variant == 0:
        data = data[:rng.randrange(1, len(data))]
    elif variant == 1:
        data = data[rng.randrange(1, 8):]
    elif variant == 2:
        data = data[:rng.choice((0, 4, 8))]
    elif variant == 3:
        pos = data.rfind(b"IEND")
        data = data[:max(8, pos - 4)]
    else:
        pos = data.find(b"IDAT")
        length = struct.unpack(">I", data[pos - 4:pos])[0]
        data = data[:pos + 4 + rng.randrange(0, max(1, length))]
    return data, {**meta, "variant": variant}


def gen_idat_truncation(rng):
    data, meta = base_png(rng)
    raw = zlib.decompress(idat_body(data))
    variant = rng.randrange(4)
    if variant == 0:
        cut = rng.randrange(1, max(2, len(raw)))
        data = replace_idat(data, zlib.compress(raw[:cut], 6))
    elif variant == 1:
        pos = data.find(b"IDAT")
        length = struct.unpack(">I", data[pos - 4:pos])[0]
        data = data[:pos + 4 + rng.randrange(0, max(1, length))]
    elif variant == 2:
        parts = [(typ, body) for _, _, typ, body in iter_chunks(data)
                 if typ != b"IDAT"]
        data = rebuild(parts)
    else:
        compressed = idat_body(data)
        data = replace_idat(data, compressed[:rng.randrange(1, len(compressed))])
    return data, {**meta, "variant": variant}


def gen_length_mismatch(rng):
    data, meta = base_png(rng)
    chunks = list(iter_chunks(data))
    variant = rng.randrange(5)
    candidates = chunks if variant != 2 else \
        [item for item in chunks if item[1] > 0]
    offset, length, typ, body = rng.choice(candidates)
    if variant == 0:
        data = data[:offset] + struct.pack(">I", 0x7FFFFFFF) + data[offset + 4:]
    elif variant == 1:
        data = data[:offset] + struct.pack(">I", 0xFFFFFFFF) + data[offset + 4:]
    elif variant == 2:
        data = data[:offset] + struct.pack(">I", 0) + data[offset + 4:]
    elif variant == 3:
        data = data[:offset] + struct.pack(">I", length + 1) + data[offset + 4:]
    else:
        crc_off = offset + 8 + length
        data = data[:crc_off] + b"\0\0\0\0" + data[crc_off + 4:]
    return data, {**meta, "variant": variant, "target": typ.decode("ascii"),
                  "at": offset}


def gen_oversize(rng):
    variant = rng.randrange(6)
    if variant == 0:
        width, height = 70000, rng.randint(1, 16)
    elif variant == 1:
        width, height = rng.randint(1, 16), 70000
    elif variant == 2:
        width, height = 65535, 65535
    elif variant == 3:
        width, height = 0x7FFFFFFF, 0x7FFFFFFF
    elif variant == 4:
        width, height = rng.choice([(0, 8), (8, 0), (0, 0)])
    else:
        width, height = 1000000, 1000000
    data, meta = base_png(rng, 2, 2)
    ihdr = struct.pack(">IIBBBBB", width, height, meta["depth"],
                       meta["ctype"], 0, 0, 0)
    parts = [(b"IHDR", ihdr)] + [(t, b) for _, _, t, b in iter_chunks(data)
                                  if t != b"IHDR"]
    return rebuild(parts), {**meta, "variant": variant,
                            "declared_w": width, "declared_h": height}


def gen_bad_filter(rng):
    data, meta = base_png(rng, interlace=0)
    raw = bytearray(zlib.decompress(idat_body(data)))
    channels = COLOR_CHANNELS[meta["ctype"]]
    stride = 1 + (meta["w"] * channels * meta["depth"] + 7) // 8
    positions = [y * stride for y in range(meta["h"])]
    variant = rng.randrange(3)
    if variant == 0:
        raw[rng.choice(positions)] = rng.choice([5, 7, 42, 255])
    elif variant == 1:
        raw[0] = 255
    else:
        bad = rng.choice([5, 9, 251])
        for pos in positions:
            if pos < len(raw):
                raw[pos] = bad
    return replace_idat(data, zlib.compress(bytes(raw), 6)), \
        {**meta, "variant": variant}


def gen_cyclic(rng):
    data, meta = base_png(rng)
    parts = [(typ, body) for _, _, typ, body in iter_chunks(data)]
    names = [typ for typ, _ in parts]
    variant = rng.randrange(6)
    if variant == 0:
        parts.insert(rng.randrange(2, len(parts)), parts[0])
    elif variant == 1:
        if b"PLTE" in names:
            pi, ii = names.index(b"PLTE"), names.index(b"IDAT")
            parts[pi], parts[ii] = parts[ii], parts[pi]
        else:
            parts.insert(1, (b"IDAT", b"x"))
    elif variant == 2:
        if b"PLTE" in names:
            plte = parts.pop(names.index(b"PLTE"))
            parts.insert(names.index(b"IDAT") + 1, plte)
        else:
            parts.insert(names.index(b"IDAT") + 1, (b"PLTE", b"\0\0\0"))
    elif variant == 3:
        parts.append((b"tEXt", b"after\x00iend"))
        parts.append((b"IEND", b""))
    elif variant == 4:
        parts.insert(names.index(b"IEND"), (b"IEND", b""))
    else:
        return SIG + chunk(b"IHDR", parts[0][1]) + data, \
            {**meta, "variant": variant}
    return rebuild(parts), {**meta, "variant": variant}


CATEGORIES = {
    "truncation": gen_truncation,
    "idat_truncation": gen_idat_truncation,
    "length_mismatch": gen_length_mismatch,
    "oversize": gen_oversize,
    "illegal_filter": gen_bad_filter,
    "cyclic_chunk": gen_cyclic,
}


def gen_malformed(rng):
    os.makedirs(MALFORMED, exist_ok=True)
    manifest = []
    for category, generator in CATEGORIES.items():
        for index in range(500):
            data, meta = generator(rng)
            name = f"{category}_{index:03d}.png"
            write(os.path.join(MALFORMED, name), data)
            manifest.append({"file": name, "category": category,
                             "bytes": len(data), **meta})
    with open(os.path.join(MALFORMED, "manifest.json"), "w") as fh:
        json.dump(manifest, fh)


def gen_minimal_samples():
    os.makedirs(SAMPLES, exist_ok=True)
    rows = [bytes([10 * y + x for x in range(4)]) for y in range(4)]
    good = build_png(4, 4, 8, 0, rows)
    write(os.path.join(SAMPLES, "valid_gray8.png"), good)

    rows_rgba = []
    for y in range(4):
        row = bytearray()
        for x in range(4):
            row += bytes([x * 17, y * 17, (x + y) * 9, 255])
        rows_rgba.append(bytes(row))
    write(os.path.join(SAMPLES, "valid_rgba8.png"),
          build_png(4, 4, 8, 6, rows_rgba))

    write(os.path.join(SAMPLES, "truncated_idat.png"),
          good[:8 + 12 + 13 + 8 + 5])

    stream = zlib.compress(b"\x00" + rows[0], 6)
    write(os.path.join(SAMPLES, "truncated_idat_stream.png"),
          rebuild([(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0)),
                   (b"IDAT", stream[:max(2, len(stream) // 2)]),
                   (b"IEND", b"")]))

    bad_ihdr = struct.pack(">I", 0xFFFFFFFF) + b"IHDR" + b"\x00" * 13
    write(os.path.join(SAMPLES, "illegal_chunk_length.png"), SIG + bad_ihdr)

    oversize = struct.pack(">IIBBBBB", 70000, 8, 8, 0, 0, 0, 0)
    write(os.path.join(SAMPLES, "oversize_dimensions.png"),
          SIG + chunk(b"IHDR", oversize) + chunk(b"IEND", b""))

    huge_pixels = struct.pack(">IIBBBBB", 65535, 65535, 8, 0, 0, 0, 0)
    write(os.path.join(SAMPLES, "oversize_pixels.png"),
          SIG + chunk(b"IHDR", huge_pixels) + chunk(b"IEND", b""))

    raw = bytearray()
    raw += b"\xff" + rows[0]
    for y in range(1, 4):
        raw += b"\x00" + rows[y]
    write(os.path.join(SAMPLES, "illegal_filter.png"),
          rebuild([(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0)),
                   (b"IDAT", zlib.compress(bytes(raw), 6)),
                   (b"IEND", b"")]))

    parts = [(typ, body) for _, _, typ, body in iter_chunks(good)]
    write(os.path.join(SAMPLES, "trailing_after_iend.png"),
          rebuild(parts) + b"junk")


def main():
    rng = random.Random(20261117)
    gen_minimal_samples()
    gen_valid(rng)
    gen_malformed(rng)
    print("wrote 500 valid, 3000 malformed and 9 minimal samples")


if __name__ == "__main__":
    main()

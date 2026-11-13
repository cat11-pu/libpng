#!/usr/bin/env python3
import json
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from pngcodec import build_png, chunk, SIG, iter_chunks, rebuild

OUT = os.path.join(os.path.dirname(__file__), "corpus", "malformed")


def base_png(rng, w=None, h=None):
    w = w or rng.randint(2, 48)
    h = h or rng.randint(2, 48)
    depth = rng.choice([1, 2, 4, 8])
    ct = rng.choice([0, 2, 3, 4, 6]) if depth == 8 else rng.choice([0, 3])
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    if ct == 3:
        row_bytes = (w * depth + 7) // 8
        rows = [bytes(rng.randrange(256) for _ in range(row_bytes)) for _ in range(h)]
        palette = bytes(rng.randrange(256) for _ in range(256 * 3))
    else:
        row_bytes = w * channels
        rows = [bytes(rng.randrange(256) for _ in range(row_bytes)) for _ in range(h)]
        palette = None
    interlace = 1 if rng.random() < 0.2 else 0
    return build_png(w, h, depth, ct, rows, interlace=interlace, palette=palette), \
        {"w": w, "h": h, "depth": depth, "ctype": ct, "interlace": interlace}


def chunk_list(data):
    return [(typ, body) for _, _, typ, body in iter_chunks(data)]


def cat_truncate(rng):
    data, meta = base_png(rng)
    variant = rng.randrange(5)
    if variant == 0:  # cut inside a chunk body
        cut = rng.randrange(9, max(10, len(data) - 8))
        data = data[:cut]
    elif variant == 1:  # remove signature tail
        cut = rng.randrange(1, 8)
        data = data[cut:]
    elif variant == 2:  # empty file / signature only
        data = data[:rng.choice((0, 4, 8))]
    elif variant == 3:  # drop IEND and trailing
        iend = data.rfind(b"IEND")
        data = data[:iend - 4] if iend > 0 else data[:8]
    else:  # cut IDAT stream mid-stream
        idat = data.find(b"IDAT")
        clen = struct.unpack(">I", data[idat - 4:idat])[0]
        cut = idat + 4 + rng.randrange(0, max(1, clen))
        data = data[:cut]
    return data, {**meta, "variant": variant}


def cat_length_mismatch(rng):
    data, meta = base_png(rng)
    variant = rng.randrange(5)
    chunks = list(iter_chunks(data))
    if variant in (1, 3):
        crit = [i for i, c in enumerate(chunks) if c[2] in (b"IHDR", b"IDAT", b"PLTE")]
        idx = rng.choice(crit)
    else:
        idx = rng.randrange(len(chunks))
    off, length, typ, body = chunks[idx]
    if variant == 0:  # declared length larger, truncated on disk
        new_len = length + rng.randint(1, 64)
        avail = max(1, len(data) - (off + 8))
        data = data[:off] + struct.pack(">I", min(new_len, avail)) + data[off + 4:]
        data = data[:len(data) - rng.randint(1, min(8, len(data) - off - 9))]
    elif variant == 1:  # declared length smaller, next chunk header lands in body
        new_len = rng.randrange(0, max(1, length))
        data = data[:off] + struct.pack(">I", new_len) + data[off + 4:]
    elif variant == 2:  # giant declared length
        data = data[:off] + struct.pack(">I", 0xFFFFFFF0) + data[off + 4:]
    elif variant == 3:  # zero-length critical chunk
        data = data[:off] + struct.pack(">I", 0) + data[off + 4:]
    else:  # corrupt CRC of a critical chunk, keep length (still well-framed)
        crc_off = off + 8 + length
        bad = struct.pack(">I", rng.randrange(1 << 32))
        data = data[:crc_off] + bad + data[crc_off + 4:]
    return data, {**meta, "variant": variant, "target": typ.decode("latin1"),
                  "at": off}


def cat_oversize(rng):
    variant = rng.randrange(6)
    if variant == 0:
        w, h = 70000, rng.randint(1, 16)
    elif variant == 1:
        w, h = rng.randint(1, 16), 70000
    elif variant == 2:
        w = h = 65535
    elif variant == 3:
        w, h = 0x10000, 0x10000
    elif variant == 4:
        w, h = rng.choice([(0, 8), (8, 0), (0, 0)])
    else:
        w, h = 40000, 40000
    data, meta = base_png(rng, max(1, min(w, 32)), max(1, min(h, 32)))
    ihdr = struct.pack(">IIBBBBB", w, h, meta["depth"], meta["ctype"], 0, 0,
                       meta["interlace"])
    data = SIG + chunk(b"IHDR", ihdr) + data[8 + 12 + 13:]
    return data, {**meta, "variant": variant, "declared_w": w, "declared_h": h}


def cat_bad_filter(rng):
    data, meta = base_png(rng)
    if meta["interlace"]:
        data, meta = base_png(rng)
        meta["interlace"] = 0
        w, h, depth, ct = meta["w"], meta["h"], meta["depth"], meta["ctype"]
        channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
        if ct == 3:
            row_bytes = (w * depth + 7) // 8
            rows = [bytes(rng.randrange(256) for _ in range(row_bytes)) for _ in range(h)]
            palette = bytes(rng.randrange(256) for _ in range(256 * 3))
            data = build_png(w, h, depth, ct, rows, interlace=0, palette=palette)
        else:
            rows = [bytes(rng.randrange(256) for _ in range(w * channels)) for _ in range(h)]
            data = build_png(w, h, depth, ct, rows, interlace=0)
    variant = rng.randrange(4)
    idat_idx = next(i for i, (_, _, t, _) in enumerate(iter_chunks(data)) if t == b"IDAT")
    off, length, _, body = list(iter_chunks(data))[idat_idx]
    raw = zlib_inflate(body)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[meta["ctype"]]
    filter_positions = []
    if not meta["interlace"]:
        stride = 1 + (meta["w"] * channels * meta["depth"] + 7) // 8
        filter_positions = [y * stride for y in range(meta["h"])]
    else:
        from pngcodec import adam7_passes
        cursor = 0
        for _, _, _, _, pw, ph in adam7_passes(meta["w"], meta["h"]):
            if pw == 0 or ph == 0:
                continue
            sub_stride = 1 + (pw * channels * meta["depth"] + 7) // 8
            filter_positions += [cursor + py * sub_stride for py in range(ph)]
            cursor += sub_stride * ph
    filter_positions = [p for p in filter_positions if p < len(raw)]
    if variant == 0:  # illegal filter type 5..255, always on a filter byte
        pos = rng.choice(filter_positions or [0])
        raw = bytearray(raw)
        raw[pos] = rng.choice([5, 6, 100, 200, 255])
        raw = bytes(raw)
        tag_pos = pos
    elif variant == 1:  # corrupt filter byte of first scanline
        raw = bytes([rng.choice([5, 7, 255])]) + raw[1:]
        tag_pos = 0
    elif variant == 2:  # force illegal value on every filter byte
        raw = bytearray(raw)
        bad = rng.choice([5, 9, 42, 251, 255])
        for p in filter_positions:
            raw[p] = bad
        raw = bytes(raw)
        tag_pos = filter_positions[0] if filter_positions else 0
    else:  # truncated deflate payload: drop at least one full scanline
        stride = 1 + (meta["w"] * channels * meta["depth"] + 7) // 8
        drop = rng.randint(stride, max(stride, len(raw) // 2))
        raw = raw[:max(1, len(raw) - drop)]
        tag_pos = len(raw)
    import zlib
    new_idat = zlib.compress(raw, 6)
    data = data[:off] + chunk(b"IDAT", new_idat) + data[off + 12 + length:]
    return data, {**meta, "variant": variant, "stream_pos": tag_pos}


def zlib_inflate(body):
    import zlib
    return zlib.decompress(body)


def cat_cyclic(rng):
    data, meta = base_png(rng)
    variant = rng.choice([0, 0, 0, 1, 1, 1, 2, 2, 2, 5, 5, 3, 4])
    if variant in (1, 2) and meta["ctype"] != 3:
        # force a palette-based base for ordering violations
        w = h = rng.randint(4, 32)
        depth = rng.choice([1, 2, 4, 8])
        rows = [bytes(rng.randrange(256) for _ in range((w * depth + 7) // 8))
                for _ in range(h)]
        palette = bytes(rng.randrange(256) for _ in range(256 * 3))
        data = build_png(w, h, depth, 3, rows, palette=palette)
        meta = {"w": w, "h": h, "depth": depth, "ctype": 3, "interlace": 0}
    ch = chunk_list(data)
    names = [t for t, _ in ch]
    if variant == 0:  # duplicated IHDR
        ch.insert(rng.randrange(2, len(ch)), ch[0])
    elif variant == 1:  # IDAT before PLTE
        if b"PLTE" in names:
            pi = names.index(b"PLTE")
            ii = names.index(b"IDAT")
            ch[pi], ch[ii] = ch[ii], ch[pi]
        else:
            ch.insert(1, (b"IDAT", b"x"))
    elif variant == 2:  # PLTE after IDAT
        if b"PLTE" in names:
            plte = ch.pop(names.index(b"PLTE"))
            ch.insert(names.index(b"IDAT") + 1, plte)
        else:
            ch.insert(max(1, names.index(b"IDAT")), (b"PLTE", b"\0\0\0"))
    elif variant == 3:  # two IEND with data between / after
        ch.append((b"tEXt", b"after\x00iend"))
        ch.append((b"IEND", b""))
    elif variant == 4:  # nested / repeated IDAT chains and IEND mid stream
        iend_pos = names.index(b"IEND")
        ch.insert(iend_pos, (b"IDAT", b"stale-stream-bytes"))
        ch.insert(iend_pos + 1, (b"IEND", b""))
    else:  # IHDR loop: append another full signature + IHDR
        return SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)), \
            {**meta, "variant": variant}
    return rebuild(ch), {**meta, "variant": variant}


CATEGORIES = {
    "truncation": cat_truncate,
    "length_mismatch": cat_length_mismatch,
    "oversize": cat_oversize,
    "illegal_filter": cat_bad_filter,
    "cyclic_chunk": cat_cyclic,
}


def main():
    rng = random.Random(99173)
    os.makedirs(OUT, exist_ok=True)
    manifest = []
    per_cat = 600
    idx = 0
    for cat, fn in CATEGORIES.items():
        for j in range(per_cat):
            data, meta = fn(rng)
            name = f"{cat}_{j:03d}.png"
            with open(os.path.join(OUT, name), "wb") as fh:
                fh.write(data)
            manifest.append({"file": name, "category": cat, "bytes": len(data), **meta})
            idx += 1
    with open(os.path.join(OUT, "manifest.json"), "w") as fh:
        json.dump(manifest, fh)
    print(f"wrote {len(manifest)} malformed PNGs across {len(CATEGORIES)} categories")


if __name__ == "__main__":
    main()

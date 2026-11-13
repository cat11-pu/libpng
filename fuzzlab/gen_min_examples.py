#!/usr/bin/env python3
"""Build one hand-crafted minimal counterexample per malformed category."""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pngcodec import SIG, chunk, build_png

OUT = os.path.join(os.path.dirname(__file__), "min")


def main():
    os.makedirs(OUT, exist_ok=True)
    # base: 4x4 gray8, filter-0 rows
    rows = [bytes([10 * y + x for x in range(4)]) for y in range(4)]
    base = build_png(4, 4, 8, 0, rows)

    # 1. truncation: signature + IHDR header, cut mid IHDR body
    with open(os.path.join(OUT, "min_truncation.png"), "wb") as fh:
        fh.write(base[:8 + 8 + 5])

    # 2. chunk length mismatch: IHDR declares length 255 but file ends
    bad_ihdr = struct.pack(">I", 255) + b"IHDR" + b"\x00\x00\x00\x04"
    with open(os.path.join(OUT, "min_length_mismatch.png"), "wb") as fh:
        fh.write(SIG + bad_ihdr)

    # 3. oversize: 70000x8 with valid IHDR CRC, no image data
    ihdr = struct.pack(">IIBBBBB", 70000, 8, 8, 0, 0, 0, 0)
    with open(os.path.join(OUT, "min_oversize.png"), "wb") as fh:
        fh.write(SIG + chunk(b"IHDR", ihdr) + chunk(b"IEND", b""))

    # 4. illegal filter: first scanline uses filter type 255
    stride = 1 + 4
    raw = bytearray()
    raw += b"\xff" + rows[0]
    for y in range(1, 4):
        raw += b"\x00" + rows[y]
    idat = zlib.compress(bytes(raw), 6)
    with open(os.path.join(OUT, "min_illegal_filter.png"), "wb") as fh:
        fh.write(SIG + chunk(b"IHDR", ihdr.replace(struct.pack(">II", 70000, 8),
                                                    struct.pack(">II", 4, 4)))
                 + chunk(b"IDAT", idat) + chunk(b"IEND", b""))

    # 5. cyclic chunk: duplicated IHDR after the real one
    good_ihdr = struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0)
    with open(os.path.join(OUT, "min_cyclic_chunk.png"), "wb") as fh:
        fh.write(SIG + chunk(b"IHDR", good_ihdr)
                 + chunk(b"IHDR", good_ihdr)
                 + chunk(b"IDAT", idat) + chunk(b"IEND", b""))

    for name in sorted(os.listdir(OUT)):
        print(name, os.path.getsize(os.path.join(OUT, name)), "bytes")


if __name__ == "__main__":
    main()

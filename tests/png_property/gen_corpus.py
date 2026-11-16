#!/usr/bin/env python3
"""Generate a deterministic PNG property-test corpus.

The malformed corpus contains exactly 200 files: 50 truncations, 50 invalid
chunks, 50 oversized/overflowing IHDR dimensions and 50 duplicate-palette
files.  Cases are interleaved by category so a partially completed run still
has balanced coverage.

The valid corpus contains exactly 50 PNGs selected from contrib/testpngs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shutil
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
CATEGORIES = (
    "truncated",
    "invalid_chunk",
    "huge_dimensions",
    "duplicate_palette",
)


@dataclass(frozen=True)
class Chunk:
    start: int
    length: int
    type: bytes
    data_start: int
    data_end: int
    end: int

    @property
    def raw(self) -> bytes:
        return bytes(self._raw)

    def with_raw(self, raw: bytes) -> "Chunk":
        clone = object.__new__(Chunk)
        object.__setattr__(clone, "_raw", raw)
        for name in ("start", "length", "type", "data_start", "data_end", "end"):
            object.__setattr__(clone, name, getattr(self, name))
        return clone


def be32(value: int) -> bytes:
    return struct.pack(">I", value & 0xFFFFFFFF)


def make_chunk(chunk_type: bytes, data: bytes) -> bytes:
    if len(chunk_type) != 4:
        raise ValueError("PNG chunk type must contain four bytes")
    return (
        be32(len(data))
        + chunk_type
        + data
        + be32(zlib.crc32(chunk_type + data) & 0xFFFFFFFF)
    )


def parse_chunks(data: bytes) -> list[Chunk]:
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")

    chunks: list[Chunk] = []
    pos = len(PNG_SIGNATURE)
    while pos < len(data):
        if pos + 12 > len(data):
            raise ValueError(f"short chunk header at {pos}")
        length = struct.unpack_from(">I", data, pos)[0]
        end = pos + 12 + length
        if end > len(data):
            raise ValueError(f"short chunk at {pos}: need {end}, have {len(data)}")
        chunk_type = data[pos + 4 : pos + 8]
        chunks.append(
            Chunk(
                start=pos,
                length=length,
                type=chunk_type,
                data_start=pos + 8,
                data_end=pos + 8 + length,
                end=end,
            ).with_raw(data[pos:end])
        )
        pos = end
        if chunk_type == b"IEND":
            break
    return chunks


def find_chunk(chunks: list[Chunk], chunk_type: bytes) -> Chunk:
    for chunk in chunks:
        if chunk.type == chunk_type:
            return chunk
    raise ValueError(f"missing {chunk_type!r} chunk")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_png(path: Path, data: bytes) -> None:
    path.write_bytes(data)


def describe_offset(chunks: list[Chunk], offset: int) -> str:
    for chunk in chunks:
        if chunk.start <= offset < chunk.end:
            if offset < chunk.data_start:
                part = "length/type"
            elif offset < chunk.data_end:
                part = "data"
            else:
                part = "crc"
            return f"{chunk.type.decode('latin-1')} {part} at +{offset - chunk.start}"
    return f"byte {offset}"


def select_valid_sources(repo_root: Path) -> list[Path]:
    sources = sorted((repo_root / "contrib" / "testpngs").glob("*.png"))
    if len(sources) < 50:
        raise SystemExit(f"need at least 50 test PNGs, found {len(sources)}")

    # Evenly sample the sorted corpus so all colour types, bit depths, gamma
    # variants, tRNS cases and palette depths are represented.
    count = len(sources)
    indices = {round(i * (count - 1) / 49) for i in range(50)}
    if len(indices) != 50:
        raise SystemExit("valid corpus selection produced duplicate indices")
    return [sources[index] for index in sorted(indices)]


def generate_valid(repo_root: Path, corpus_dir: Path) -> list[dict]:
    valid_dir = corpus_dir / "valid"
    valid_dir.mkdir(parents=True, exist_ok=True)
    records = []

    for index, source in enumerate(select_valid_sources(repo_root)):
        data = source.read_bytes()
        chunks = parse_chunks(data)
        ihdr = find_chunk(chunks, b"IHDR")
        width, height = struct.unpack_from(">II", data, ihdr.data_start)
        bit_depth = data[ihdr.data_start + 8]
        color_type = data[ihdr.data_start + 9]
        destination = valid_dir / f"v{index:03d}_{source.name}"
        write_png(destination, data)
        records.append(
            {
                "case_id": f"v{index:03d}",
                "path": str(destination.relative_to(corpus_dir)),
                "source": str(source.relative_to(repo_root)),
                "width": width,
                "height": height,
                "bit_depth": bit_depth,
                "color_type": color_type,
                "bytes": len(data),
                "sha256": sha256(data),
            }
        )
    return records


def mutation_base_pool(valid_records: list[dict], corpus_dir: Path) -> list[bytes]:
    # Spread the base images over different filter/colour/gamma variants.
    picks = [valid_records[round(i * 49 / 9)] for i in range(10)]
    return [(corpus_dir / record["path"]).read_bytes() for record in picks]


def generate_truncations(bases: list[bytes]) -> list[tuple[bytes, int, str]]:
    rng = random.Random(0x5452554E43)  # TRUNC
    cases: list[tuple[bytes, int, str]] = []

    for seed in range(50):
        base = bases[seed % len(bases)]
        chunks = parse_chunks(base)
        candidates = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}
        candidates.add(len(base) - 1)
        for chunk in chunks:
            candidates.update(
                {
                    chunk.start,
                    chunk.start + 4,
                    chunk.start + 8,
                    chunk.data_end,
                    chunk.data_end + 2,
                    chunk.end - 1,
                }
            )
        candidates.update(rng.randrange(len(base)) for _ in range(96))
        candidates = sorted(offset for offset in candidates if 0 <= offset < len(base))
        # Required offsets exercise the signature, IHDR, and end-of-file paths.
        required = {0, 1, 7, 8, 11, 12, 16, 20, len(base) - 1}
        mandatory = sorted(required.intersection(candidates))
        pool = [offset for offset in candidates if offset not in required]
        chosen = mandatory + rng.sample(pool, 50 - len(mandatory))
        offset = chosen[seed]
        cases.append(
            (
                base[:offset],
                offset,
                f"truncate at {offset}: {describe_offset(chunks, offset)}; "
                f"original={len(base)} bytes",
            )
        )
    return cases


def clone_bytes(data: bytes) -> bytearray:
    return bytearray(data)


def set_be32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", data, offset, value & 0xFFFFFFFF)


def generate_invalid_chunks(bases: list[bytes]) -> list[tuple[bytes, int, str]]:
    cases: list[tuple[bytes, int, str]] = []

    for seed in range(50):
        base = bases[seed % len(bases)]
        chunks = parse_chunks(base)
        ihdr = find_chunk(chunks, b"IHDR")
        idat = find_chunk(chunks, b"IDAT")
        kind = seed % 10

        if kind == 0:
            data = clone_bytes(base)
            offset = ihdr.data_end + 3
            data[offset] ^= 0x01
            detail = "corrupt IHDR CRC"
        elif kind == 1:
            data = clone_bytes(base)
            offset = idat.data_end + 1
            data[offset] ^= 0x80
            detail = "corrupt IDAT CRC"
        elif kind == 2:
            data = clone_bytes(base)
            offset = idat.start
            set_be32(data, offset, 0x7FFFFFFF)
            detail = "set IDAT length to 0x7fffffff"
        elif kind == 3:
            data = clone_bytes(base)
            offset = ihdr.start
            set_be32(data, offset, 0)
            detail = "set IHDR length to zero"
        elif kind == 4:
            data = clone_bytes(base)
            offset = idat.start + 4
            data[offset : offset + 4] = b"I\x00AT"
            detail = "invalid zero byte in IDAT chunk type"
        elif kind == 5:
            offset = idat.start
            bad = make_chunk(b"ABCD", b"")
            data = clone_bytes(base[:offset] + bad + base[offset:])
            detail = "insert unknown critical chunk ABCD before IDAT"
        elif kind == 6:
            offset = ihdr.end
            data = clone_bytes(base[:offset] + ihdr.raw + base[offset:])
            detail = "duplicate IHDR chunk"
        elif kind == 7:
            offset = idat.start
            reserved = make_chunk(b"abcD", b"")
            data = clone_bytes(base[:offset] + reserved + base[offset:])
            detail = "insert chunk with reserved bit set (abcD)"
        elif kind == 8:
            data = clone_bytes(base)
            offset = idat.data_start + (idat.length // 2)
            if idat.length == 0:
                offset = idat.data_start
                data = clone_bytes(base[:offset] + b"\x00" + base[offset:])
            else:
                data[offset] ^= 0x5A
            detail = "flip byte inside IDAT zlib stream"
        else:
            data = clone_bytes(base)
            offset = idat.start
            set_be32(data, offset, 1)
            detail = "set IDAT length to one byte"

        # Some branches insert bytes; keep the offset deterministic and useful.
        cases.append((bytes(data), offset, detail))

    return cases


def rewrite_ihdr(data: bytes, width: int, height: int) -> bytes:
    mutable = clone_bytes(data)
    ihdr = find_chunk(parse_chunks(data), b"IHDR")
    set_be32(mutable, ihdr.data_start, width)
    set_be32(mutable, ihdr.data_start + 4, height)
    crc = zlib.crc32(mutable[ihdr.start + 4 : ihdr.data_end]) & 0xFFFFFFFF
    set_be32(mutable, ihdr.data_end, crc)
    return bytes(mutable)


def generate_huge_dimensions(bases: list[bytes]) -> list[tuple[bytes, int, str]]:
    dimensions = (
        (0, 1),
        (1, 0),
        (0, 0),
        (0x80000000, 1),
        (1, 0x80000000),
        (0xFFFFFFFF, 1),
        (1, 0xFFFFFFFF),
        (0x7FFFFFFF, 0x7FFFFFFF),
        (0x10000000, 0x10000000),
        (1_000_000, 1_000_000),
    )
    cases: list[tuple[bytes, int, str]] = []

    for seed in range(50):
        base = bases[seed % len(bases)]
        ihdr = find_chunk(parse_chunks(base), b"IHDR")
        old_width, old_height = struct.unpack_from(">II", base, ihdr.data_start)
        width, height = dimensions[seed % len(dimensions)]
        data = rewrite_ihdr(base, width, height)
        offset = ihdr.data_start + (0 if width != old_width else 4)
        if width == old_width and height == old_height:
            offset = ihdr.data_start
        cases.append(
            (
                data,
                offset,
                f"rewrite IHDR dimensions to {width}x{height}, IHDR CRC fixed",
            )
        )
    return cases


def palette_variant(plte_data: bytes, variant: int) -> bytes:
    if variant == 0:
        return plte_data
    if variant == 1:
        return bytes(byte ^ 0xA5 for byte in plte_data)
    if variant == 2:
        entries = [plte_data[i : i + 3] for i in range(0, len(plte_data), 3)]
        return b"".join(reversed(entries))
    if variant == 3:
        return bytes((byte + 0x3C) & 0xFF for byte in plte_data)
    return plte_data[::-1]


def generate_duplicate_palettes(
    valid_records: list[dict], corpus_dir: Path
) -> list[tuple[bytes, int, str]]:
    palette_records = [
        record
        for record in valid_records
        if Path(record["path"]).name.startswith("v") and "palette-" in record["path"]
    ]
    if not palette_records:
        raise SystemExit("valid corpus has no palette PNGs")

    bases = [
        (corpus_dir / palette_records[i % len(palette_records)]["path"]).read_bytes()
        for i in range(10)
    ]
    cases: list[tuple[bytes, int, str]] = []

    for seed in range(50):
        base = bases[seed % len(bases)]
        chunks = parse_chunks(base)
        plte = find_chunk(chunks, b"PLTE")
        idat = find_chunk(chunks, b"IDAT")
        variant = seed % 5
        plte_data = base[plte.data_start : plte.data_end]

        if variant == 0:
            offset = plte.end
            insertion = make_chunk(b"PLTE", plte_data)
            detail = "duplicate identical PLTE immediately after PLTE"
        elif variant == 1:
            offset = plte.end
            insertion = make_chunk(b"PLTE", palette_variant(plte_data, 1))
            detail = "duplicate XORed PLTE immediately after PLTE"
        elif variant == 2:
            offset = plte.start
            insertion = make_chunk(b"PLTE", palette_variant(plte_data, 2))
            detail = "insert reversed PLTE immediately before PLTE"
        elif variant == 3:
            offset = plte.end
            insertion = make_chunk(
                b"PLTE", palette_variant(plte_data, 3)
            ) + make_chunk(b"PLTE", palette_variant(plte_data, 4))
            detail = "insert two alternating duplicate PLTE chunks"
        else:
            offset = idat.end
            insertion = make_chunk(b"PLTE", palette_variant(plte_data, 1))
            detail = "insert duplicate PLTE after IDAT"

        cases.append((base[:offset] + insertion + base[offset:], offset, detail))

    return cases


def write_manifest(corpus_dir: Path, records: list[dict]) -> None:
    with (corpus_dir / "manifest.jsonl").open("w", encoding="utf-8") as stream:
        for record in records:
            stream.write(json.dumps(record, sort_keys=True) + "\n")


def build_corpus(repo_root: Path, corpus_dir: Path) -> tuple[list[dict], list[dict]]:
    if corpus_dir.exists():
        shutil.rmtree(corpus_dir)
    corpus_dir.mkdir(parents=True)

    valid_records = generate_valid(repo_root, corpus_dir)
    valid_path = corpus_dir / "valid_manifest.jsonl"
    with valid_path.open("w", encoding="utf-8") as stream:
        for record in valid_records:
            stream.write(json.dumps(record, sort_keys=True) + "\n")

    bases = mutation_base_pool(valid_records, corpus_dir)
    category_cases = {
        "truncated": generate_truncations(bases),
        "invalid_chunk": generate_invalid_chunks(bases),
        "huge_dimensions": generate_huge_dimensions(bases),
        "duplicate_palette": generate_duplicate_palettes(valid_records, corpus_dir),
    }
    for category, cases in category_cases.items():
        if len(cases) != 50:
            raise SystemExit(f"{category}: expected 50 cases, got {len(cases)}")

    malformed_dir = corpus_dir / "malformed"
    malformed_dir.mkdir()
    malformed_records = []
    global_index = 0

    # Deliberately interleave categories.  A failed early run cannot leave one
    # class of malformed input completely untested.
    for seed in range(50):
        for category in CATEGORIES:
            data, offset, detail = category_cases[category][seed]
            name = f"m{global_index:03d}_{category}_s{seed:02d}.png"
            path = malformed_dir / name
            write_png(path, data)
            malformed_records.append(
                {
                    "case_id": f"m{global_index:03d}",
                    "category": category,
                    "seed": seed,
                    "path": str(path.relative_to(corpus_dir)),
                    "offset": offset,
                    "offset_hex": f"0x{offset:08x}",
                    "detail": detail,
                    "bytes": len(data),
                    "sha256": sha256(data),
                }
            )
            global_index += 1

    write_manifest(corpus_dir, malformed_records)
    return malformed_records, valid_records


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="libpng source tree",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("artifacts/png_property/corpus"),
        help="corpus output directory",
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    out = args.out
    if not out.is_absolute():
        out = (repo_root / out).resolve()

    malformed, valid = build_corpus(repo_root, out)
    print(f"corpus: {out}")
    print(f"malformed: {len(malformed)}")
    print(f"valid: {len(valid)}")
    for category in CATEGORIES:
        count = sum(record["category"] == category for record in malformed)
        print(f"  {category}: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

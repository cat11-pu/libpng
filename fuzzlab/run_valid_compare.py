#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor, as_completed

from PIL import Image

HERE = os.path.dirname(__file__)
HARNESS = os.path.join(HERE, "build", "harness")
CORPUS = os.path.join(HERE, "corpus", "valid")
REPORT = os.path.join(HERE, "reports", "valid_compare.json")


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    tokens = data.split(b"\n", 3)
    assert tokens[0] == b"P6"
    dims = tokens[1].split()
    w, h = int(dims[0]), int(dims[1])
    maxv = int(tokens[2])
    assert maxv == 255
    pix = tokens[3]
    assert len(pix) == w * h * 3
    return w, h, pix


def libpng_style_rgb(entry, src):
    """Mirror the harness: expand palette/tRNS, strip 16->8 (high byte),
    gray->RGB, then take RGB channels (filler dropped)."""
    with Image.open(src) as im:
        im.load()
        w, h = im.size
        if im.mode == "RGBA" or im.mode == "LA":
            im = im.convert(im.mode[:-1])
        if im.mode == "P":
            im = im.convert("RGB")
        elif im.mode == "L":
            im = im.convert("RGB")
        if entry["depth"] == 16 and im.mode == "I;16":
            base_mode = im.mode.split(";")[0]
            raw16 = im.tobytes("raw", im.mode)
            ch = 1
            reduced = bytearray(len(raw16) // 2)
            j = 0
            for i in range(0, len(raw16) - 2 * ch + 1, 2 * ch):
                for c in range(ch):
                    reduced[j] = raw16[i + c * 2 + 1]
                    j += 1
            im = Image.frombytes("L", (w, h), bytes(reduced))
        if im.mode == "L":
            im = im.convert("RGB")
        return (w, h), im.convert("RGB").tobytes()


def compare_one(entry):
    src = os.path.join(CORPUS, entry["file"])
    (rw, rh), ref = libpng_style_rgb(entry, src)
    with tempfile.NamedTemporaryFile(suffix=".ppm", delete=False) as tf:
        out = tf.name
    try:
        proc = subprocess.run([HARNESS, "ppm", src, out], capture_output=True,
                              text=True, timeout=30)
        if proc.returncode != 0:
            return {**entry, "match": False, "mismatch_pixels": None,
                    "reason": "harness_failed", "stderr": proc.stderr[-400:]}
        w, h, got = read_ppm(out)
    finally:
        os.unlink(out)
    if (w, h) != (rw, rh):
        return {**entry, "match": False, "mismatch_pixels": None,
                "reason": f"dimension {w}x{h} != {rw}x{rh}"}
    if got == ref:
        return {**entry, "match": True, "mismatch_pixels": 0, "reason": ""}
    diffs = []
    n = w * h
    for i in range(n):
        o = i * 3
        if got[o:o + 3] != ref[o:o + 3]:
            diffs.append((i % w, i // w,
                          tuple(got[o:o + 3]), tuple(ref[o:o + 3])))
            if len(diffs) >= 5:
                break
    total = sum(1 for i in range(n) if got[i * 3:i * 3 + 3] != ref[i * 3:i * 3 + 3])
    return {**entry, "match": False, "mismatch_pixels": total,
            "reason": "pixel_diff", "first_diffs": diffs}


def main():
    with open(os.path.join(CORPUS, "manifest.json")) as fh:
        manifest = json.load(fh)
    results = []
    with ProcessPoolExecutor(max_workers=14) as pool:
        futures = {pool.submit(compare_one, e): e for e in manifest}
        done = 0
        for fut in as_completed(futures):
            results.append(fut.result())
            done += 1
            if done % 100 == 0:
                print(f"  {done}/{len(manifest)}", flush=True)
    order = {e["file"]: i for i, e in enumerate(manifest)}
    results.sort(key=lambda r: order[r["file"]])
    with open(REPORT, "w") as fh:
        json.dump(results, fh, indent=1)
    ok = sum(1 for r in results if r["match"])
    print(f"pixel-exact matches: {ok}/{len(results)}")
    fails = [r for r in results if not r["match"]]
    by_reason = {}
    for r in fails:
        by_reason[r["reason"]] = by_reason.get(r["reason"], 0) + 1
    print("failures:", by_reason)
    for r in fails[:10]:
        print(r["file"], r["reason"], r.get("first_diffs"))


if __name__ == "__main__":
    main()

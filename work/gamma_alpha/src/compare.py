#!/usr/bin/env python3
"""Pixel-level comparison harness.

For every corpus PNG it runs:
  * the independent pure-Python reference decoder (ground truth);
  * the buggy C driver;
  * the fixed C driver.

The fixed raster must match the reference byte for byte.  The buggy raster is
scored to demonstrate the two defects, separating:
  * gamma drift on opaque/partial-alpha color components;
  * non-zero color leaking through fully transparent pixels.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
CORPUS = os.path.join(ROOT, "corpus")
OUT = os.path.join(ROOT, "out")


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{cmd} failed: {proc.stderr}")
    return proc.stdout.strip()


def load(path):
    with open(path, "rb") as f:
        return f.read()


def compare(name):
    png = os.path.join(CORPUS, name + ".png")
    buggy = os.path.join(OUT, name + ".buggy.rgba")
    fixed = os.path.join(OUT, name + ".fixed.rgba")
    ref = os.path.join(OUT, name + ".ref.rgba")

    run([os.path.join(HERE, "decode_buggy"), png, buggy])
    run([os.path.join(HERE, "decode_fixed"), png, fixed])
    run([sys.executable, os.path.join(HERE, "reference.py"), png, ref])

    ref_b = bytearray(load(ref))
    bug_b = bytearray(load(buggy))
    fix_b = bytearray(load(fixed))

    assert len(ref_b) == len(fix_b) == len(bug_b)
    npix = len(ref_b) // 4

    fixed_mismatch = sum(
        1 for i in range(0, len(ref_b), 4)
        if fix_b[i:i + 4] != ref_b[i:i + 4])

    gamma_drift = 0          # pixels whose color differs while alpha > 0
    gamma_max = 0
    fringe_bad = 0           # transparent pixels retaining non-zero color
    buggy_diff_bytes = 0
    opaque_drift_examples = []
    fringe_examples = []

    for p in range(npix):
        q = p * 4
        if bug_b[q:q + 4] != ref_b[q:q + 4]:
            buggy_diff_bytes += sum(
                1 for k in range(4) if bug_b[q + k] != ref_b[q + k])

        alpha = ref_b[q + 3]
        if alpha == 0:
            if bug_b[q] or bug_b[q + 1] or bug_b[q + 2]:
                fringe_bad += 1
                if len(fringe_examples) < 3:
                    fringe_examples.append(
                        (p, tuple(bug_b[q:q + 4]), tuple(ref_b[q:q + 4])))
        else:
            dc = max(abs(bug_b[q] - ref_b[q]),
                     abs(bug_b[q + 1] - ref_b[q + 1]),
                     abs(bug_b[q + 2] - ref_b[q + 2]))
            if dc:
                gamma_drift += 1
                gamma_max = max(gamma_max, dc)
                if len(opaque_drift_examples) < 3:
                    opaque_drift_examples.append(
                        (p, tuple(bug_b[q:q + 4]), tuple(ref_b[q:q + 4])))

    return {
        "image": name + ".png",
        "pixels": npix,
        "fixed_vs_reference_mismatch_pixels": fixed_mismatch,
        "fixed_matches_reference": fixed_mismatch == 0,
        "buggy_gamma_drift_pixels": gamma_drift,
        "buggy_gamma_max_channel_delta": gamma_max,
        "buggy_transparent_fringe_pixels": fringe_bad,
        "buggy_total_differing_bytes": buggy_diff_bytes,
        "gamma_drift_examples": opaque_drift_examples,
        "fringe_examples": fringe_examples,
    }


def main():
    os.makedirs(OUT, exist_ok=True)
    names = sorted(
        os.path.splitext(f)[0] for f in os.listdir(CORPUS)
        if f.endswith(".png"))
    results = [compare(n) for n in names]

    report_path = os.path.join(ROOT, "report", "comparison.json")
    with open(report_path, "w") as f:
        json.dump(results, f, indent=2)

    headers = ("image", "pixels", "fixed==ref", "gamma_drift",
               "max_dR/G/B", "fringe_bad", "diff_bytes")
    print("{:<26}{:>7}{:>11}{:>12}{:>11}{:>11}{:>11}".format(*headers))
    for r in results:
        print("{:<26}{:>7}{:>11}{:>12}{:>11}{:>11}{:>11}".format(
            r["image"], r["pixels"],
            "yes" if r["fixed_matches_reference"] else "NO",
            r["buggy_gamma_drift_pixels"],
            r["buggy_gamma_max_channel_delta"],
            r["buggy_transparent_fringe_pixels"],
            r["buggy_total_differing_bytes"]))

    all_match = all(r["fixed_matches_reference"] for r in results)
    print("\nfixed driver matches reference on all pixels:",
          "PASS" if all_match else "FAIL")
    print("json report:", os.path.relpath(report_path, ROOT))
    return 0 if all_match else 1


if __name__ == "__main__":
    raise SystemExit(main())

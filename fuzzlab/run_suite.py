#!/usr/bin/env python3
"""Run malformed and valid corpora through the strict decode harness."""

import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed

HERE = os.path.dirname(__file__)
BUILD = os.path.join(HERE, "build")
HARNESS = os.path.join(BUILD, "harness")
BASELINE = os.path.join(BUILD, "harness.baseline")
MALFORMED = os.path.join(HERE, "corpus", "malformed")
VALID = os.path.join(HERE, "corpus", "valid")
REPORTS = os.path.join(HERE, "reports")

ENV = dict(os.environ)
ENV["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1:halt_on_error=1"
ENV["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

REJECT_RE = re.compile(
    r"^REJECT code=(\d+)\(([^)]+)\) offset=0x([0-9a-f]+) "
    r"chunk=(\S+) detail=\"([^\"]*)\" file=")
OK_RE = re.compile(
    r"^OK width=(\d+) height=(\d+) rowbytes=(\d+) pixels=(\d+) "
    r"hash=0x([0-9a-f]+) alloc_peak=(\d+)$")
ALLOC_RE = re.compile(r"alloc_peak=(\d+)")


def run(binary, args, timeout=20):
    try:
        proc = subprocess.run([binary] + args, capture_output=True, text=True,
                              timeout=timeout, env=ENV)
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return None, "", "timeout"


def malformed_one(entry):
    path = os.path.join(MALFORMED, entry["file"])
    rc, stdout, stderr = run(HARNESS, ["check", path])
    match = None
    for line in stderr.splitlines():
        candidate = REJECT_RE.match(line)
        if candidate:
            match = candidate
            break

    if match:
        code = int(match.group(1))
        alloc_match = ALLOC_RE.search(line)
        result = {
            "file": entry["file"],
            "category": entry["category"],
            "kind": "clean_reject",
            "rc": rc,
            "code": code,
            "name": match.group(2),
            "offset": int(match.group(3), 16),
            "chunk": match.group(4),
            "detail": match.group(5),
            "alloc_peak": int(alloc_match.group(1)) if alloc_match else 0,
        }
    elif rc == 0:
        result = {"file": entry["file"], "category": entry["category"],
                  "kind": "decoded", "rc": rc, "code": None}
    elif rc is None:
        result = {"file": entry["file"], "category": entry["category"],
                  "kind": "timeout", "rc": None, "code": None}
    else:
        result = {"file": entry["file"], "category": entry["category"],
                  "kind": "crash", "rc": rc, "code": None,
                  "stderr_tail": "\n".join(stderr.splitlines()[-20:])}
    result["stdout"] = stdout.strip()
    return result


def parse_ok(stdout):
    for line in stdout.splitlines():
        match = OK_RE.match(line)
        if match:
            return {
                "width": int(match.group(1)),
                "height": int(match.group(2)),
                "rowbytes": int(match.group(3)),
                "pixels": int(match.group(4)),
                "hash": match.group(5),
                "alloc_peak": int(match.group(6)),
            }
    return None


def valid_one(entry):
    path = os.path.join(VALID, entry["file"])
    rc_now, out_now, err_now = run(HARNESS, ["check", path])
    now = parse_ok(out_now)
    rc_base, out_base, err_base = run(BASELINE, ["check", path])
    base = parse_ok(out_base)
    exact = (rc_now == 0 and rc_base == 0 and now is not None and
             base is not None and now["hash"] == base["hash"] and
             now["width"] == base["width"] and
             now["height"] == base["height"] and
             now["rowbytes"] == base["rowbytes"])
    return {
        "file": entry["file"],
        "depth": entry["depth"],
        "ctype": entry["ctype"],
        "interlace": entry["interlace"],
        "match": exact,
        "hash": now["hash"] if now else None,
        "alloc_peak": now["alloc_peak"] if now else None,
        "baseline_hash": base["hash"] if base else None,
        "rc": rc_now,
        "baseline_rc": rc_base,
        "stderr": err_now[-400:] if not exact else "",
    }


def main():
    os.makedirs(REPORTS, exist_ok=True)
    if not os.path.exists(BASELINE):
        print("missing baseline harness: " + BASELINE, file=sys.stderr)
        return 2

    with open(os.path.join(MALFORMED, "manifest.json")) as fh:
        malformed = json.load(fh)
    with open(os.path.join(VALID, "manifest.json")) as fh:
        valid = json.load(fh)

    malformed_results = []
    with ProcessPoolExecutor(max_workers=14) as pool:
        futures = [pool.submit(malformed_one, entry) for entry in malformed]
        for done, future in enumerate(as_completed(futures), 1):
            malformed_results.append(future.result())
            if done % 500 == 0:
                print(f"malformed {done}/{len(futures)}", flush=True)

    order = {entry["file"]: index for index, entry in enumerate(malformed)}
    malformed_results.sort(key=lambda item: order[item["file"]])

    valid_results = []
    with ProcessPoolExecutor(max_workers=14) as pool:
        futures = [pool.submit(valid_one, entry) for entry in valid]
        for done, future in enumerate(as_completed(futures), 1):
            valid_results.append(future.result())
            if done % 100 == 0:
                print(f"valid {done}/{len(futures)}", flush=True)

    order = {entry["file"]: index for index, entry in enumerate(valid)}
    valid_results.sort(key=lambda item: order[item["file"]])

    bad = [item for item in malformed_results
           if item["kind"] != "clean_reject"]
    exact = sum(1 for item in valid_results if item["match"])
    summary = {
        "malformed_total": len(malformed_results),
        "clean_reject": sum(1 for item in malformed_results
                            if item["kind"] == "clean_reject"),
        "decoded": sum(1 for item in malformed_results
                       if item["kind"] == "decoded"),
        "crash": sum(1 for item in malformed_results
                     if item["kind"] == "crash"),
        "timeout": sum(1 for item in malformed_results
                       if item["kind"] == "timeout"),
        "offset_complete": sum(1 for item in malformed_results
                               if item["kind"] == "clean_reject" and
                               item["code"] is not None and
                               item["offset"] is not None and
                               item["chunk"]),
        "sanitizer_events": sum(
            1 for item in malformed_results
            if item["kind"] == "crash" and
            ("AddressSanitizer" in item.get("stderr_tail", "") or
             "UndefinedBehaviorSanitizer" in item.get("stderr_tail", "") or
             "LeakSanitizer" in item.get("stderr_tail", ""))),
        "valid_total": len(valid_results),
        "valid_pixel_identical": exact,
        "valid_failures": len(valid_results) - exact,
        "max_alloc_peak_bytes": max(
            [item.get("alloc_peak") or 0 for item in malformed_results] +
            [item.get("alloc_peak") or 0 for item in valid_results]),
        "categories": {},
    }

    for item in malformed_results:
        bucket = summary["categories"].setdefault(item["category"], {})
        bucket[item["kind"]] = bucket.get(item["kind"], 0) + 1

    with open(os.path.join(REPORTS, "malformed_results.json"), "w") as fh:
        json.dump(malformed_results, fh, indent=1)
    with open(os.path.join(REPORTS, "valid_comparison.json"), "w") as fh:
        json.dump(valid_results, fh, indent=1)
    with open(os.path.join(REPORTS, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=1)

    print(json.dumps(summary, indent=2))
    if bad:
        print(f"non-clean malformed results: {len(bad)}", file=sys.stderr)
        for item in bad[:10]:
            print(item, file=sys.stderr)
        return 1
    if exact != len(valid_results):
        print(f"valid mismatches: {len(valid_results) - exact}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

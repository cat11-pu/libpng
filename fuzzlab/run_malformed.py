#!/usr/bin/env python3
import json
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed

HERE = os.path.dirname(__file__)
HARNESS = os.path.join(HERE, "build", "harness")
CORPUS = os.path.join(HERE, "corpus", "malformed")
REPORT = os.path.join(HERE, "reports", "malformed_results.json")

ENV = dict(os.environ)
ENV["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1:halt_on_error=1:print_stats=0"
ENV["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"


def classify(rc, err):
    if "AddressSanitizer" in err:
        kind = "asan"
    elif "LeakSanitizer" in err:
        kind = "lsan"
    elif "runtime error" in err and "UndefinedSanitizer" in err:
        kind = "ubsan"
    elif rc in (134, -6):
        kind = "abort"
    elif rc in (139, -11):
        kind = "segv"
    elif rc == 1:
        kind = "clean_reject"
    elif rc == 0:
        kind = "decoded"
    else:
        kind = f"other_rc_{rc}"
    return kind


def run_one(entry):
    path = os.path.join(CORPUS, entry["file"])
    try:
        proc = subprocess.run([HARNESS, "decode", path], capture_output=True,
                              text=True, timeout=30, env=ENV)
        rc = proc.returncode
        err = proc.stderr
    except subprocess.TimeoutExpired:
        return {**entry, "kind": "timeout", "rc": None, "detail": "", "stderr_tail": ""}
    kind = classify(rc, err)
    reject = ""
    for line in err.splitlines():
        if line.startswith("REJECT "):
            reject = line
            break
    tail = "\n".join(err.splitlines()[-12:])
    return {**entry, "kind": kind, "rc": rc, "detail": reject, "stderr_tail": tail}


def main():
    with open(os.path.join(CORPUS, "manifest.json")) as fh:
        manifest = json.load(fh)
    results = []
    with ProcessPoolExecutor(max_workers=14) as pool:
        futures = {pool.submit(run_one, e): e for e in manifest}
        done = 0
        for fut in as_completed(futures):
            results.append(fut.result())
            done += 1
            if done % 250 == 0:
                print(f"  {done}/{len(manifest)}", flush=True)
    order = {e["file"]: i for i, e in enumerate(manifest)}
    results.sort(key=lambda r: order[r["file"]])
    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    with open(REPORT, "w") as fh:
        json.dump(results, fh, indent=1)
    summary = {}
    for r in results:
        cat = r["category"]
        summary.setdefault(cat, {})
        summary[cat][r["kind"]] = summary[cat].get(r["kind"], 0) + 1
    print(json.dumps(summary, indent=2))
    bad = [r for r in results if r["kind"] not in ("clean_reject", "decoded")]
    print(f"sanitizer/crash/timeout events: {len(bad)}")


if __name__ == "__main__":
    main()

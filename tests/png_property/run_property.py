#!/usr/bin/env python3
"""Build and run the libpng malformed/pixel-equivalence property tests."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT / "artifacts" / "png_property"
DEFAULT_MAX_IMAGE_BYTES = 64 * 1024 * 1024


def run(
    command: list[str],
    *,
    cwd: Path = ROOT,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
    check: bool = False,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=check,
    )


def generate_corpus(corpus_dir: Path) -> None:
    run(
        [
            sys.executable,
            str(ROOT / "tests" / "png_property" / "gen_corpus.py"),
            "--repo-root",
            str(ROOT),
            "--out",
            str(corpus_dir),
        ],
        check=True,
    )


def build_local_library(build_dir: Path) -> Path:
    if build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True)
    sanitizer_flags = (
        "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
    )
    configure = [
        "cmake",
        "-S",
        str(ROOT),
        "-B",
        str(build_dir),
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DPNG_SHARED=OFF",
        "-DPNG_STATIC=ON",
        "-DPNG_FRAMEWORK=OFF",
        "-DPNG_TESTS=OFF",
        "-DPNG_EXECUTABLES=OFF",
        f"-DCMAKE_C_FLAGS={sanitizer_flags}",
        f"-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined",
    ]
    configured = run(configure, check=False)
    (build_dir / "configure.log").write_text(
        configured.stdout + configured.stderr, encoding="utf-8"
    )
    if configured.returncode != 0:
        raise SystemExit(
            f"cmake configure failed; see {build_dir / 'configure.log'}"
        )

    built = run(
        ["cmake", "--build", str(build_dir), "--target", "png_static", "--parallel"],
        check=False,
    )
    (build_dir / "build.log").write_text(built.stdout + built.stderr, encoding="utf-8")
    if built.returncode != 0:
        raise SystemExit(f"cmake build failed; see {build_dir / 'build.log'}")

    libraries = sorted(build_dir.glob("libpng*.a"))
    if not libraries:
        raise SystemExit(f"no static libpng library found in {build_dir}")
    return libraries[0]


def compile_probes(
    bin_dir: Path, build_dir: Path, library: Path
) -> tuple[Path, Path]:
    bin_dir.mkdir(parents=True, exist_ok=True)
    source = ROOT / "tests" / "png_property" / "png_probe.c"
    local_probe = bin_dir / "png_probe_asan"
    reference_probe = bin_dir / "png_probe_reference"
    sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]

    local = run(
        [
            os.environ.get("CC", "cc"),
            "-std=c99",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            *sanitizer,
            "-I",
            str(build_dir),
            "-I",
            str(ROOT),
            str(source),
            str(library),
            "-lz",
            "-lm",
            "-o",
            str(local_probe),
        ],
        check=False,
    )
    if local.returncode != 0:
        raise SystemExit(f"local probe build failed:\n{local.stderr}")

    pkg = run(["pkg-config", "--cflags", "--libs", "libpng"], check=False)
    pkg_flags = pkg.stdout.split() if pkg.returncode == 0 else ["-lpng16"]
    ref = run(
        [
            os.environ.get("CC", "cc"),
            "-std=c99",
            "-O2",
            "-g",
            "-Wall",
            "-Wextra",
            str(source),
            *pkg_flags,
            "-lz",
            "-lm",
            "-o",
            str(reference_probe),
        ],
        check=False,
    )
    if ref.returncode != 0:
        raise SystemExit(f"reference probe build failed:\n{ref.stderr}")
    return local_probe, reference_probe


def sanitizer_environment() -> dict[str, str]:
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = (
        "detect_leaks=1:halt_on_error=1:abort_on_error=0:"
        "allocator_may_return_null=1:disable_coredump=1:exitcode=42:"
        "symbolize=1"
    )
    env["LSAN_OPTIONS"] = "exitcode=23:report_objects=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return env


def parse_kv(stdout: str) -> tuple[str, dict[str, str]]:
    line = stdout.strip().splitlines()[0] if stdout.strip() else ""
    fields = line.split("\t")
    status = fields[0] if fields else ""
    values: dict[str, str] = {}
    for field in fields[1:]:
        if "=" in field:
            key, value = field.split("=", 1)
            values[key] = value
    return status, values


def classify_process(
    result: subprocess.CompletedProcess[str] | None, timed_out: bool
) -> tuple[str, str | None]:
    if timed_out:
        return "timeout", "process exceeded timeout"
    assert result is not None
    stderr = result.stderr
    if result.returncode == 23 or "LeakSanitizer" in stderr:
        return "leak", "LeakSanitizer reported leaked memory"
    if result.returncode == 42 or "AddressSanitizer" in stderr:
        return "oob", "AddressSanitizer reported an invalid memory access"
    if "runtime error:" in stderr or "UndefinedBehaviorSanitizer" in stderr:
        return "undefined_behavior", "UBSan reported undefined behavior"
    if result.returncode < 0:
        return "crash", f"killed by signal {-result.returncode}"
    if result.returncode != 0:
        return "harness_error", f"probe exited with status {result.returncode}"

    status, _ = parse_kv(result.stdout)
    if status in {"ACCEPT", "OK"}:
        return "accepted", None
    if status == "REJECT":
        return "rejected", None
    if status == "LIMIT":
        return "limited", None
    return "harness_error", f"unexpected probe output: {result.stdout[:200]!r}"


def probe_one(
    probe: Path,
    path: Path,
    max_image_bytes: int,
    env: dict[str, str],
    timeout: float,
) -> tuple[dict, subprocess.CompletedProcess[str] | None, bool]:
    started = time.monotonic()
    timed_out = False
    try:
        result = run(
            [str(probe), "probe", str(path), str(max_image_bytes)],
            env=env,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        timed_out = True
        result = subprocess.CompletedProcess(
            error.cmd, 124, error.stdout or "", error.stderr or ""
        )
    elapsed = time.monotonic() - started
    outcome, reason = classify_process(result, timed_out)
    status, values = parse_kv(result.stdout)
    return (
        {
            "outcome": outcome,
            "reason": reason,
            "probe_status": status,
            "probe_values": values,
            "returncode": result.returncode,
            "elapsed_seconds": round(elapsed, 6),
            "stdout": result.stdout[:4096],
            "stderr": result.stderr[:16384],
            "timed_out": timed_out,
        },
        result,
        timed_out,
    )


def run_malformed(
    probe: Path,
    corpus_dir: Path,
    out_dir: Path,
    max_image_bytes: int,
    timeout: float,
) -> list[dict]:
    env = sanitizer_environment()
    records = [
        json.loads(line)
        for line in (corpus_dir / "manifest.jsonl").read_text(encoding="utf-8").splitlines()
        if line
    ]
    results = []
    for index, record in enumerate(records, 1):
        path = corpus_dir / record["path"]
        probe_result, _, _ = probe_one(
            probe, path, max_image_bytes, env, timeout
        )
        result = {**record, **probe_result}
        results.append(result)
        print(
            f"malformed {index:03d}/200 {record['case_id']} "
            f"{record['category']:<18} {result['outcome']}",
            flush=True,
        )

    with (out_dir / "malformed_results.jsonl").open("w", encoding="utf-8") as stream:
        for result in results:
            stream.write(json.dumps(result, sort_keys=True) + "\n")
    return results


def decode_one(
    probe: Path,
    path: Path,
    raw_path: Path,
    max_image_bytes: int,
    env: dict[str, str] | None,
    timeout: float,
) -> tuple[dict, bool]:
    timed_out = False
    started = time.monotonic()
    try:
        result = run(
            [str(probe), "decode", str(path), str(raw_path), str(max_image_bytes)],
            env=env,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        timed_out = True
        result = subprocess.CompletedProcess(
            error.cmd, 124, error.stdout or "", error.stderr or ""
        )
    elapsed = time.monotonic() - started
    outcome, reason = classify_process(result, timed_out)
    status, values = parse_kv(result.stdout)
    return (
        {
            "outcome": outcome,
            "reason": reason,
            "probe_status": status,
            "probe_values": values,
            "returncode": result.returncode,
            "elapsed_seconds": round(elapsed, 6),
            "stdout": result.stdout[:4096],
            "stderr": result.stderr[:16384],
            "timed_out": timed_out,
        },
        status == "OK" and result.returncode == 0,
    )


def compare_pixels(
    local_path: Path,
    reference_path: Path,
    width: int,
    height: int,
    bytes_per_component: int,
) -> dict:
    local = local_path.read_bytes()
    reference = reference_path.read_bytes()
    pixel_stride = 4 * bytes_per_component
    expected = width * height * pixel_stride
    result = {
        "equal": False,
        "local_bytes": len(local),
        "reference_bytes": len(reference),
        "expected_bytes": expected,
        "bytes_per_component": bytes_per_component,
        "first_diff_byte": None,
        "first_diff_pixel": None,
        "first_diff_channel": None,
        "first_diff_component_byte": None,
        "local_value": None,
        "reference_value": None,
    }
    if len(local) != len(reference):
        result["first_diff_byte"] = min(len(local), len(reference))
        result["first_diff_pixel"] = result["first_diff_byte"] // pixel_stride
        result["first_diff_channel"] = (
            result["first_diff_byte"] % pixel_stride
        ) // bytes_per_component
        return result
    if len(local) != expected:
        result["first_diff_byte"] = min(len(local), expected)
        result["first_diff_pixel"] = result["first_diff_byte"] // pixel_stride
        result["first_diff_channel"] = (
            result["first_diff_byte"] % pixel_stride
        ) // bytes_per_component
        return result

    # Compare every pixel, every RGBA component and every component byte;
    # report the first mismatch in byte, pixel, channel and component-byte
    # coordinates.
    for pixel in range(width * height):
        base = pixel * pixel_stride
        for channel in range(4):
            for component_byte in range(bytes_per_component):
                offset = base + channel * bytes_per_component + component_byte
                if local[offset] != reference[offset]:
                    result["first_diff_byte"] = offset
                    result["first_diff_pixel"] = pixel
                    result["first_diff_channel"] = channel
                    result["first_diff_component_byte"] = component_byte
                    result["local_value"] = local[offset]
                    result["reference_value"] = reference[offset]
                    return result

    result["equal"] = True
    return result


def run_valid(
    local_probe: Path,
    reference_probe: Path,
    corpus_dir: Path,
    out_dir: Path,
    max_image_bytes: int,
    timeout: float,
    pixel_format: str,
) -> list[dict]:
    local_env = sanitizer_environment()
    local_env["PNG_PROBE_FORMAT"] = pixel_format
    reference_env = os.environ.copy()
    reference_env["PNG_PROBE_FORMAT"] = pixel_format
    raw_dir = out_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    records = [
        json.loads(line)
        for line in (corpus_dir / "valid_manifest.jsonl")
        .read_text(encoding="utf-8")
        .splitlines()
        if line
    ]
    results = []

    for index, record in enumerate(records, 1):
        path = corpus_dir / record["path"]
        local_raw = raw_dir / f"{record['case_id']}_local.rgba"
        reference_raw = raw_dir / f"{record['case_id']}_reference.rgba"
        local_result, local_ok = decode_one(
            local_probe, path, local_raw, max_image_bytes, local_env, timeout
        )
        reference_result, reference_ok = decode_one(
            reference_probe,
            path,
            reference_raw,
            max_image_bytes,
            reference_env,
            timeout,
        )
        comparison = {
            "equal": False,
            "first_diff_byte": None,
            "first_diff_pixel": None,
            "first_diff_channel": None,
            "first_diff_component_byte": None,
            "local_value": None,
            "reference_value": None,
            "bytes_per_component": 2 if pixel_format == "rgba16" else 1,
        }
        if local_ok and reference_ok:
            width = int(local_result["probe_values"]["w"])
            height = int(local_result["probe_values"]["h"])
            reference_width = int(reference_result["probe_values"]["w"])
            reference_height = int(reference_result["probe_values"]["h"])
            if (width, height) != (reference_width, reference_height):
                comparison = {
                    "equal": False,
                    "first_diff_byte": 0,
                    "first_diff_pixel": 0,
                    "first_diff_channel": 0,
                    "first_diff_component_byte": 0,
                    "local_value": None,
                    "reference_value": None,
                    "bytes_per_component": 2 if pixel_format == "rgba16" else 1,
                    "dimension_mismatch": {
                        "local": [width, height],
                        "reference": [reference_width, reference_height],
                    },
                }
            else:
                comparison = compare_pixels(
                    local_raw,
                    reference_raw,
                    width,
                    height,
                    2 if pixel_format == "rgba16" else 1,
                )
        result = {
            **record,
            "local": local_result,
            "reference": reference_result,
            "comparison": comparison,
            "pass": local_ok and reference_ok and comparison["equal"],
        }
        results.append(result)
        print(
            f"valid {index:02d}/50 {record['case_id']} "
            f"{'match' if result['pass'] else 'FAIL'}",
            flush=True,
        )

    with (out_dir / "valid_results.jsonl").open("w", encoding="utf-8") as stream:
        for result in results:
            stream.write(json.dumps(result, sort_keys=True) + "\n")
    return results


def failure_record(result: dict, kind: str) -> dict:
    if kind == "malformed":
        return {
            "kind": kind,
            "case_id": result["case_id"],
            "category": result["category"],
            "path": result["path"],
            "offset": result["offset"],
            "offset_hex": result["offset_hex"],
            "detail": result["detail"],
            "outcome": result["outcome"],
            "reason": result["reason"],
            "stderr": result["stderr"],
        }
    comparison = result["comparison"]
    return {
        "kind": kind,
        "case_id": result["case_id"],
        "path": result["path"],
        "offset": comparison.get("first_diff_byte"),
        "offset_hex": (
            f"0x{comparison['first_diff_byte']:08x}"
            if comparison.get("first_diff_byte") is not None
            else None
        ),
        "pixel": comparison.get("first_diff_pixel"),
        "channel": comparison.get("first_diff_channel"),
        "component_byte": comparison.get("first_diff_component_byte"),
        "local_value": comparison.get("local_value"),
        "reference_value": comparison.get("reference_value"),
        "local_outcome": result["local"]["outcome"],
        "reference_outcome": result["reference"]["outcome"],
    }


def write_summary(
    out_dir: Path,
    malformed: list[dict],
    valid: list[dict],
    local_version: str,
    reference_version: str,
    max_image_bytes: int,
    git_commit: str,
    pixel_format: str,
) -> bool:
    safety_failures = [
        result
        for result in malformed
        if result["outcome"]
        in {"crash", "oob", "leak", "undefined_behavior", "timeout", "harness_error"}
    ]
    valid_failures = [result for result in valid if not result["pass"]]
    malformed_counts = Counter(result["outcome"] for result in malformed)
    category_counts: dict[str, Counter] = defaultdict(Counter)
    for result in malformed:
        category_counts[result["category"]][result["outcome"]] += 1

    total_pixels = sum(result["width"] * result["height"] for result in valid)
    bytes_per_component = 2 if pixel_format == "rgba16" else 1
    passed = not safety_failures and not valid_failures
    lines = [
        "# libpng property-test summary",
        "",
        f"- Source commit: `{git_commit}`",
        f"- Local libpng: `{local_version}`",
        f"- Reference libpng: `{reference_version}`",
        f"- Host: `{platform.platform()}`",
        f"- Sanitizers: ASan + UBSan + LeakSanitizer",
        f"- Per-image allocation guard: `{max_image_bytes}` bytes",
        f"- Pixel comparison format: `{pixel_format}`",
        "",
        "## Malformed corpus",
        "",
        f"- Total: {len(malformed)} (50 per category, categories interleaved)",
        f"- Accepted: {malformed_counts['accepted']}",
        f"- Gracefully rejected: {malformed_counts['rejected']}",
        f"- Resource-limit guard before pixel allocation: {malformed_counts['limited']}",
        f"- Crashes/OOB/leaks/UB/timeouts: {len(safety_failures)}",
        "",
        "| Category | Accepted | Rejected | Limited | Failures |",
        "|---|---:|---:|---:|---:|",
    ]
    for category in (
        "truncated",
        "invalid_chunk",
        "huge_dimensions",
        "duplicate_palette",
    ):
        counts = category_counts[category]
        failures = sum(
            counts[name]
            for name in (
                "crash",
                "oob",
                "leak",
                "undefined_behavior",
                "timeout",
                "harness_error",
            )
        )
        lines.append(
            f"| {category} | {counts['accepted']} | {counts['rejected']} | "
            f"{counts['limited']} | {failures} |"
        )

    lines.extend(
        [
            "",
            "A malformed input may be accepted or rejected; the safety property is "
            "that either path is memory-safe and leak-free.",
            "",
            "## Valid corpus",
            "",
            f"- Total: {len(valid)}",
            f"- Pixel comparisons: {total_pixels} pixels "
            f"({total_pixels * 4 * bytes_per_component} RGBA bytes, "
            f"{bytes_per_component * 8}-bit components)",
            f"- Byte-identical decodes: {len(valid) - len(valid_failures)}",
            f"- Mismatches: {len(valid_failures)}",
            "",
            "## Verdict",
            "",
        ]
    )
    if passed:
        lines.append(
            "**PASS**: zero crashes, zero out-of-bounds accesses, zero leaks, zero "
            "UBSan findings, and zero pixel mismatches."
        )
    else:
        lines.append(
            f"**FAIL**: {len(safety_failures)} malformed-input safety failures and "
            f"{len(valid_failures)} valid-image mismatches. See "
            "`counterexamples.jsonl`."
        )

    lines.extend(
        [
            "",
            "## Artifacts",
            "",
            "- `corpus/malformed/`: 200 reproducible malformed PNGs",
            "- `corpus/valid/`: 50 valid PNGs",
            "- `corpus/manifest.jsonl`: malformed-case offsets and mutations",
            "- `malformed_results.jsonl`: per-case outcome and sanitizer output",
            "- `valid_results.jsonl`: per-image decode results and first mismatch",
            "- `COUNTEREXAMPLES.md`: representative negative inputs and failures",
            "- `counterexamples.jsonl`: failing cases with byte offsets (empty on PASS)",
            "",
        ]
    )
    (out_dir / "SUMMARY.md").write_text("\n".join(lines), encoding="utf-8")

    counterexamples = [
        failure_record(result, "malformed") for result in safety_failures
    ] + [failure_record(result, "valid") for result in valid_failures]
    with (out_dir / "counterexamples.jsonl").open("w", encoding="utf-8") as stream:
        for record in counterexamples:
            stream.write(json.dumps(record, sort_keys=True) + "\n")

    write_counterexample_report(
        out_dir, malformed, valid, safety_failures, valid_failures
    )
    return passed


def write_counterexample_report(
    out_dir: Path,
    malformed: list[dict],
    valid: list[dict],
    safety_failures: list[dict],
    valid_failures: list[dict],
) -> None:
    lines = [
        "# Counterexamples and negative inputs",
        "",
        "The serialized safety/pixel failures are in `counterexamples.jsonl`. "
        "When the verdict is PASS that file is empty.",
        "",
        "## Safety failures",
        "",
    ]
    if safety_failures:
        for result in safety_failures:
            lines.append(
                f"- `{result['case_id']}` ({result['category']}), offset "
                f"`{result['offset_hex']}`: {result['outcome']} - "
                f"{result['reason']} ({result['detail']})"
            )
    else:
        lines.append(
            "None: no crash, OOB, leak, UBSan finding, timeout or harness error."
        )

    lines.extend(["", "## Pixel mismatches", ""])
    if valid_failures:
        for result in valid_failures:
            comparison = result["comparison"]
            lines.append(
                f"- `{result['case_id']}` (`{result['path']}`), first diff at "
                f"byte `{comparison['first_diff_byte']}`, pixel "
                f"`{comparison['first_diff_pixel']}`, channel "
                f"`{comparison['first_diff_channel']}`: local="
                f"{comparison['local_value']}, reference="
                f"{comparison['reference_value']}"
            )
    else:
        lines.append("None: every compared component matched the reference.")

    lines.extend(
        [
            "",
            "## Representative malformed inputs",
            "",
            "These are reproducible negative inputs, not safety failures. "
            "Offsets are zero-based file offsets.",
            "",
            "| Case | Category | Offset | Mutation | Observed |",
            "|---|---|---|---|---|",
        ]
    )
    categories = (
        "truncated",
        "invalid_chunk",
        "huge_dimensions",
        "duplicate_palette",
    )
    for category in categories:
        selected = [result for result in malformed if result["category"] == category][:2]
        for result in selected:
            detail = result["detail"].replace("|", "\\|")
            lines.append(
                f"| `{result['case_id']}` | {category} | "
                f"`{result['offset_hex']}` | {detail} | {result['outcome']} |"
            )

    accepted = [result for result in malformed if result["outcome"] == "accepted"]
    lines.extend(
        [
            "",
            f"## Accepted malformed inputs ({len(accepted)})",
            "",
            "These are not memory-safety failures; libpng chose a lenient "
            "acceptance path for them.",
            "",
            "| Case | Category | Offset | Mutation |",
            "|---|---|---|---|",
        ]
    )
    for result in accepted:
        detail = result["detail"].replace("|", "\\|")
        lines.append(
            f"| `{result['case_id']}` | {result['category']} | "
            f"`{result['offset_hex']}` | {detail} |"
        )
    if not accepted:
        lines.append("| - | - | - | - |")
    lines.append("")
    (out_dir / "COUNTEREXAMPLES.md").write_text(
        "\n".join(lines), encoding="utf-8"
    )


def git_commit() -> str:
    result = run(["git", "rev-parse", "HEAD"], check=False)
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument(
        "--max-image-bytes", type=int, default=DEFAULT_MAX_IMAGE_BYTES
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument(
        "--pixel-format",
        choices=("rgba16", "rgba8"),
        default="rgba16",
        help="canonical format used for the reference pixel comparison",
    )
    args = parser.parse_args()

    out_dir = args.out.resolve() if args.out.is_absolute() else (ROOT / args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    corpus_dir = out_dir / "corpus"
    build_dir = out_dir / "build"
    bin_dir = out_dir / "bin"

    print("generating corpus", flush=True)
    generate_corpus(corpus_dir)

    if args.skip_build:
        library = next(iter(sorted(build_dir.glob("libpng*.a"))), None)
        if library is None:
            raise SystemExit(f"no static library under {build_dir}")
        local_probe = bin_dir / "png_probe_asan"
        reference_probe = bin_dir / "png_probe_reference"
        if not local_probe.exists() or not reference_probe.exists():
            raise SystemExit(f"missing probes under {bin_dir}")
    else:
        print("building local libpng with ASan/UBSan", flush=True)
        library = build_local_library(build_dir)
        print("building probes", flush=True)
        local_probe, reference_probe = compile_probes(bin_dir, build_dir, library)

    local_version = run([str(local_probe), "version"], check=True).stdout.strip()
    reference_version = run(
        [str(reference_probe), "version"], check=True
    ).stdout.strip()

    print("running 200 malformed inputs", flush=True)
    malformed = run_malformed(
        local_probe,
        corpus_dir,
        out_dir,
        args.max_image_bytes,
        args.timeout,
    )

    print("running 50 valid-image comparisons", flush=True)
    valid = run_valid(
        local_probe,
        reference_probe,
        corpus_dir,
        out_dir,
        args.max_image_bytes,
        args.timeout,
        args.pixel_format,
    )

    passed = write_summary(
        out_dir,
        malformed,
        valid,
        local_version,
        reference_version,
        args.max_image_bytes,
        git_commit(),
        args.pixel_format,
    )
    print((out_dir / "SUMMARY.md").read_text(encoding="utf-8"))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

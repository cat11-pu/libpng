#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LAB="$ROOT/fuzzlab"
BUILD_ASAN="$ROOT/build-asan"
LAB_BUILD="$LAB/build"
BASELINE_SRC="$LAB_BUILD/baseline-src"

CFLAGS=(-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
        -DNDEBUG -std=c99)

echo "== configure and build libpng with ASan/UBSan =="
cmake -S "$ROOT" -B "$BUILD_ASAN" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" >/dev/null
cmake --build "$BUILD_ASAN" -j2 >/dev/null

mkdir -p "$LAB_BUILD" "$LAB/reports"

echo "== build strict harness =="
cc "${CFLAGS[@]}" -I"$ROOT" -I"$BUILD_ASAN" \
    "$LAB/harness.c" "$BUILD_ASAN/libpng16d.a" -lz -lm \
    -o "$LAB_BUILD/harness"

echo "== build pre-fix baseline harness =="
mkdir -p "$BASELINE_SRC"
git -C "$ROOT" archive HEAD | tar -x -C "$BASELINE_SRC"
mapfile -t BASELINE_SRCS < <(
    find "$BASELINE_SRC" -maxdepth 1 -name 'png*.c' ! -name 'pngtest.c'
)
cc "${CFLAGS[@]}" -I"$BASELINE_SRC" -I"$BUILD_ASAN" \
    "$LAB/harness.c" "${BASELINE_SRCS[@]}" \
    -lz -lm -o "$LAB_BUILD/harness.baseline"

echo "== generate 500 valid, 3000 malformed and 9 minimal samples =="
python3 "$LAB/gen_corpus.py"

echo "== run sanitizer suite =="
python3 "$LAB/run_suite.py"

echo "reports: fuzzlab/reports/"

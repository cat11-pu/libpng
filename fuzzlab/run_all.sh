#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

echo "== building ASan/UBSan/LSan harness =="
SRCS="harness.c $(cd .. && ls png*.c | grep -v pngtest | sed 's|^|../|' | tr '\n' ' ')"
gcc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DNDEBUG \
    -I.. -std=c99 $SRCS -lz -lm -o build/harness

echo "== generating corpora =="
python3 gen_valid.py
python3 gen_malformed.py
python3 gen_min_examples.py

echo "== fuzzing 3000 malformed PNGs =="
python3 run_malformed.py

echo "== pixel comparison on 500 valid PNGs (reference: Pillow 12) =="
python3 run_valid_compare.py

echo "reports in: reports/"

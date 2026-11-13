#!/usr/bin/env bash
# Build both driver variants against the in-tree static libpng, regenerate the
# corpus, run the pixel comparison and render the visual reproductions.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LAB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "== building static libpng (current tree) =="
mkdir -p "$LAB/build"
SRCS=(png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c
      pngrtran.c pngrutil.c pngset.c pngtrans.c pngwio.c pngwrite.c
      pngwtran.c pngwutil.c)
for s in "${SRCS[@]}"; do
  gcc -O2 -g -I"$ROOT" -c "$ROOT/$s" -o "$LAB/build/${s%.c}.o"
done
ar rcs "$LAB/build/libpng.a" "$LAB/build"/*.o

LIBS="$LAB/build/libpng.a -lz -lm"
echo "== building buggy and fixed decoders =="
gcc -O2 -g -I"$ROOT" "$LAB/src/png_decode.c" $LIBS \
    -o "$LAB/src/decode_buggy"
gcc -O2 -g -I"$ROOT" -DDECIDE_GAMMA -DZERO_TRANSPARENT \
    "$LAB/src/png_decode.c" $LIBS -o "$LAB/src/decode_fixed"

echo "== generating corpus =="
python3 "$LAB/src/gen_corpus.py"

echo "== pixel comparison against independent reference =="
python3 "$LAB/src/compare.py"

echo "== rendering visual reproductions =="
python3 "$LAB/src/make_visual.py"
python3 "$LAB/src/make_fringe_map.py"

echo "done; see $LAB/report"

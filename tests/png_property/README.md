# PNG malformed-input and pixel-equivalence tests

This directory contains a deterministic property test for libpng:

- 200 malformed PNGs: 50 truncations, 50 invalid chunks, 50 oversized
  dimensions, and 50 duplicate-palette files. Cases are interleaved by category.
- 50 valid PNGs selected across `contrib/testpngs`, decoded to canonical RGBA16
  by this checkout and by the system libpng, then compared pixel by pixel at
  full 16-bit precision.
- Every malformed case runs in its own process under ASan, UBSan and
  LeakSanitizer. A crash, out-of-bounds access, leak, undefined behaviour or
  timeout is reported with the mutation byte offset from `manifest.jsonl`.
- The local library is built with `-fsanitize=address,undefined`; the reference
  probe is linked against the system libpng.

Run from the repository root:

```sh
python3 tests/png_property/run_property.py
```

The run writes its corpus, logs, JSONL results and `SUMMARY.md` under
`artifacts/png_property/`. The exit status is non-zero if any safety property
or pixel comparison fails.

Useful options:

```sh
python3 tests/png_property/run_property.py --timeout 5
python3 tests/png_property/run_property.py --max-image-bytes 67108864
python3 tests/png_property/run_property.py --pixel-format rgba8
python3 tests/png_property/gen_corpus.py --out /tmp/png-corpus
```

`--max-image-bytes` is a test-harness guard, not a libpng limit. Inputs whose
decoded RGBA allocation would exceed it are rejected before the output buffer
is allocated; their header has still been parsed by libpng. This keeps giant
dimension cases from turning into allocator stress tests.

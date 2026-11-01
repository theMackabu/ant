# Fuzz and Property Targets

Each `fuzz_<name>.c` is a libFuzzer entry point (`LLVMFuzzerTestOneInput`)
that checks properties of one parser or codec, not just "does not crash".
A failed property aborts with `property failed: <condition>`.

| Target | Code under test | Properties |
| --- | --- | --- |
| `http1` | `src/http/http1_parser.c` | one-shot, prefix-fed (`Ant.serve`) and chunk-fed (`node:http`) parsing agree on every message, however the bytes are split |
| `websocket` | `src/http/websocket.c` | parsing is prefix-stable; in-place and copying parses agree; unmask matches a byte-wise XOR; encode/parse round-trips |
| `numbers` | `src/numbers.cc` | shortest, `toPrecision(17)` and `toExponential(16)` round-trip; formatters truncate like `snprintf`; parse modes agree |
| `base64` | `src/base64.c` | every decoder inverts both encoders; resumable `decode_into` matches a single call; libbase64 and spec decoders agree |
| `text` | `src/escape.c`, `src/utf8.c`, `src/modules/url/state.c` | escape decoding stays in bounds; repairs yield valid UTF-8/WTF-8; TextDecoder fatal mode; Latin-1, JSON-quote and form-urlencoding round-trips |
| `regexp` | `src/modules/regex/syntax*.c`, `js_to_pcre2_pattern` | translation truncates like `snprintf`; a pattern the JS syntax check accepts compiles in PCRE2 |
| `js` | `src/silver/` parser and compiler | parse and compile never crash, and operand depth analysis accepts every emitted function |

Seed inputs live in `corpus/<name>/`. Most targets read control bytes (flags,
split seeds, modes) from the front of the input; the comment at the top of
each target describes its layout.

## Replay (any compiler)

By default the targets link `driver.c` instead of libFuzzer. Their meson
tests replay the corpus, then run 500 seeded mutations of it:

```console
$ meson test -C build --suite fuzz
$ ANT_FUZZ_RUNS=50000 ANT_FUZZ_SEED=7 ./build/meson/tests/fuzz/fuzz-http1 tests/fuzz/corpus/http1
```

A failing input is written to `./fuzz-failure.bin` (or `ANT_FUZZ_ARTIFACT`);
pass that file to the target to replay it alone.

## Fuzzing (clang)

`-Dfuzz=enabled` links libFuzzer and instruments the runtime for coverage. Use
ASan and UBSan with it:

```console
$ maid fuzz_setup              # configures build-fuzz/
$ maid fuzz http1 300          # five minutes on one target
```

`maid fuzz` keeps new inputs in `build-fuzz/corpus/<name>/` and reads the
checked-in seeds. When running a target by hand, set
`UBSAN_OPTIONS=suppressions=tests/fuzz/ubsan.supp`, and for `js` also
`ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0`: LeakSanitizer
does not scan the GC heap, and the GC's stack scan needs real stack frames.
libFuzzer writes failures to `crash-*` in the working directory; replay one
with `./build-fuzz/meson/tests/fuzz/fuzz-<name> crash-...`.
In that build, `meson test -C build-fuzz --suite fuzz` replays the corpus once.

## Keeping the corpus useful

- Add a minimized input for every fixed failure (`-minimize_crash=1` or a
  hand-reduced file) so the replay test guards the regression.
- Merge coverage-increasing inputs back occasionally:
  `./build-fuzz/meson/tests/fuzz/fuzz-<name> -merge=1 tests/fuzz/corpus/<name> build-fuzz/corpus/<name>`.
  Keep files small; the replay test runs on every `meson test`.

## Adding a target

1. Write `tests/fuzz/fuzz_<name>.c` with a property comment at the top, using
   `FUZZ_CHECK` from `fuzz.h`. Copy inputs into exactly sized buffers when
   the code under test takes a length, so ASan sees reads past it.
2. Add `<name>` to `fuzz_targets` in `meson/tests/fuzz/meson.build`.
3. Add seeds under `corpus/<name>/` and run the replay test.

JavaScript-level properties with Node as the oracle live in
[tests/differential](../differential/README.md) instead.

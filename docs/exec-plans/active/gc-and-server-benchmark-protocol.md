# GC and Server Benchmark Protocol

Status: active
Last reviewed: 2026-07-13
Owner: theMackabu

## Goal

Keep the GC, HTTP framework, and React Server Components measurements used for
runtime and collector changes reproducible. A valid comparison must measure
throughput and memory together, use equivalent release binaries, run one server
at a time, and preserve the workload parameters below.

This protocol covers three groups:

1. GC correctness, runtime, and peak-RSS workloads under `tests/`.
2. Elysia 1, Elysia 2, and Hono HTTP throughput and server RSS.
3. The external React Server Components reproduction in
   `/private/tmp/test-rsc`.

## Comparison Binaries

Use these labels consistently:

| Label | Binary |
| --- | --- |
| `build` | `/Users/sf/Developer/ant/build/ant` |
| `installed` | `$(which ant)`, normally `/Users/sf/.ant/bin/ant` |
| `older` | `/tmp/ant/ant_12.3.dbfffe9b.1` |

Record `git rev-parse HEAD`, the absolute binary paths, and whether each binary
uses PGO before recording results. Do not interpret a non-PGO current build
against a PGO release binary as a final throughput comparison.

Build the configured tree through Maid:

```sh
maid shell
maid build
```

Run those as two commands in the interactive Maid shell. For a final PGO
comparison, generate the profile and rebuild from that same shell:

```sh
ANT_PGO_IN_NIX_SHELL=1 ./meson/pgo/build.sh
```

Do not change GC/JIT control flow after generating the PGO profile. If it does
change, regenerate PGO before the final measurements.

Common prerequisites:

```sh
command -v ant oha curl jq
test -x ./build/ant
test -x /tmp/ant/ant_12.3.dbfffe9b.1
```

Create one output directory per comparison and define the binaries before
running any workload:

```sh
cd /Users/sf/Developer/ant
RUN_ID="$(date +%Y%m%d-%H%M%S)"
RESULTS="/tmp/ant-bench-$RUN_ID"
mkdir -p "$RESULTS"

BUILD_ANT="$PWD/build/ant"
INSTALLED_ANT="$(command -v ant)"
OLDER_ANT="/tmp/ant/ant_12.3.dbfffe9b.1"

printf 'commit\t%s\n' "$(git rev-parse HEAD)" | tee "$RESULTS/metadata.tsv"
printf 'build\t%s\ninstalled\t%s\nolder\t%s\n' \
  "$BUILD_ANT" "$INSTALLED_ANT" "$OLDER_ANT" | tee -a "$RESULTS/metadata.tsv"
```

The server procedures below use this 50 ms RSS sampler. `ps` reports KiB on
macOS; the final `awk` expression converts the highest sample to MiB.

```sh
sample_rss() {
  pid="$1"
  output="$2"
  : >"$output"
  while kill -0 "$pid" 2>/dev/null; do
    ps -o rss= -p "$pid" | awk -v now="$(date +%s)" 'NF { print now, $1 }' \
      >>"$output"
    sleep 0.05
  done
}

peak_rss_mib() {
  awk 'BEGIN { max = 0 } $2 > max { max = $2 } END { printf "%.1f\n", max / 1024 }' "$1"
}
```

Run benchmarks with other Ant servers stopped. Use the same machine power mode,
avoid concurrent builds, and run comparison binaries serially rather than in
parallel.

## GC Suite

Run the following 29 files for every binary being compared:

```text
tests/bench_gc.js
tests/bench_gc_mark_func.js
tests/repro_typedarray_metadata_leak.cjs
tests/test_array_sort_gc_roots.cjs
tests/test_async_gc.js
tests/test_async_generator_gc_liveness.cjs
tests/test_gc.js
tests/test_gc_async.js
tests/test_gc_comprehensive.js
tests/test_gc_coro.js
tests/test_gc_cycles.js
tests/test_gc_debug.js
tests/test_gc_in_coro.js
tests/test_gc_large.js
tests/test_gc_minimal8.js
tests/test_gc_simple.js
tests/test_gc_stress10.js
tests/test_gc_stress7.js
tests/test_gc_tco.js
tests/test_gc_template_const_cache.cjs
tests/test_gc_timer.js
tests/test_jit_put_field_gc_barrier.cjs
tests/test_jit_string_concat_gc_roots.cjs
tests/test_map_gc.js
tests/test_timer_fired_timeout_gc.cjs
tests/test_upvalue_gc.cjs
tests/test_wasm_imported_memory_shared.cjs
tests/test_wasm_memory_constructor.cjs
tests/test_windows_float64array_tco_gc.cjs
```

Put that exact list in `"$RESULTS/gc-files.txt"`, then run the normal files
serially with the following loop. Use `build`, `installed`, and `older` as the
runtime labels and the corresponding variables defined above as the binary
paths.

```sh
run_gc_suite() {
  label="$1"
  runtime="$2"
  while IFS= read -r test_file; do
    test_name="${test_file##*/}"
    stdout="$RESULTS/gc-$label-$test_name.out"
    timing="$RESULTS/gc-$label-$test_name.time"
    if [ "$test_file" = tests/test_gc_stress10.js ]; then
      /usr/bin/time -lp "$runtime" "$test_file" >"$stdout" 2>"$timing" &
      pid=$!
      sleep 20
      kill -TERM "$pid" 2>/dev/null || true
      sleep 1
      kill -KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      printf '%s\t%s\tEXPECTED_TIMEOUT\n' "$label" "$test_file"
      continue
    fi

    /usr/bin/time -lp "$runtime" "$test_file" >"$stdout" 2>"$timing"
    status=$?
    printf '%s\t%s\t%s\n' "$label" "$test_file" "$status"
  done <"$RESULTS/gc-files.txt"
}

run_gc_suite build "$BUILD_ANT" | tee "$RESULTS/gc-build-status.tsv"
run_gc_suite installed "$INSTALLED_ANT" | tee "$RESULTS/gc-installed-status.tsv"
run_gc_suite older "$OLDER_ANT" | tee "$RESULTS/gc-older-status.tsv"
```

For each normal workload, capture stdout, exit status, elapsed time, and peak
RSS:

```sh
/usr/bin/time -lp ./build/ant tests/test_gc_async.js \
  >/tmp/gc-build-test_gc_async.out \
  2>/tmp/gc-build-test_gc_async.time
```

On macOS, convert the `maximum resident set size` value from bytes to MiB by
dividing by `1048576`. Preserve stdout because several tests report their own
runtime or memory progression.

`tests/test_gc_stress10.js` is intentionally long-running. Stop it after 20
seconds and record `EXPECTED_TIMEOUT`, not a failure. Signal the process
group or the `ant` process itself, not just `/usr/bin/time`: killing the
`time` wrapper orphans the runtime, which keeps burning CPU and skews any
concurrent measurement (`pkill -TERM -f test_gc_stress10` after the wrapper
kill is sufficient):

```sh
/usr/bin/time -lp ./build/ant tests/test_gc_stress10.js \
  >/tmp/gc-build-stress10.out \
  2>/tmp/gc-build-stress10.time &
pid=$!
sleep 20
kill -TERM "$pid" 2>/dev/null
sleep 1
kill -KILL "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
```

Record one TSV row per workload:

```text
runtime  test  status  real_s  max_rss_mb
```

Correctness is mandatory: every normal workload must exit zero. Compare peak
RSS and elapsed time individually; an aggregate average can hide a severe
regression in one allocation pattern.

For collector-policy investigation, repeat selected workloads with telemetry:

```sh
ANT_DEBUG=gc:stats ./build/ant tests/test_gc_template_const_cache.cjs \
  >/tmp/gc-stats.out 2>/tmp/gc-stats.err
rg 'gc-stats:' /tmp/gc-stats.err
```

At minimum inspect minor/major counts, major time, promoted objects, remembered
set peak, survival/reclaim EWMAs, live objects, and arena committed/watermark
peaks. Telemetry runs are diagnostic and should not be used as final timing
samples.

## Elysia And Hono RPS

These fixtures live in the repository:

| Framework | Entry point |
| --- | --- |
| Elysia 1 | `examples/npm/elysia1/bench-server.ts` |
| Elysia 2 | `examples/npm/elysia2/bench-server.ts` |
| Hono | `examples/npm/hono/bench-server.ts` |

Do not use these results for the current GC branch until the separate Elysia
fixes are present. Once they are present, run at least three paired rounds per
framework. Alternate `build` and `installed` within each round to reduce drift.

For each binary and framework:

1. Start the server and redirect its output to a per-run log.
2. Wait until `curl -fsS http://127.0.0.1:3000/` succeeds.
3. Warm up with 100,000 requests at concurrency 50.
4. Capture steady RSS immediately after warmup.
5. Sample server RSS every 50 ms during a five-second measured run.
6. Stop the sampler and server, then verify no process remains on port 3000.

Example using Elysia 2 and the current build:

```sh
./build/ant examples/npm/elysia2/bench-server.ts >/tmp/elysia2-build.log 2>&1 &
server_pid=$!

until curl -fsS http://127.0.0.1:3000/ >/dev/null; do sleep 0.05; done

NO_COLOR=false oha http://127.0.0.1:3000/ \
  -n 100000 -c 50 -H 'Accept-Encoding: identity' --no-tui

ps -o rss= -p "$server_pid"

sample_rss "$server_pid" /tmp/elysia2-build-rss.tsv &
sampler_pid=$!
NO_COLOR=false oha http://127.0.0.1:3000/ \
  -z 5s -w -c 50 -H 'Accept-Encoding: identity' --no-tui \
  --output-format json -o /tmp/elysia2-build.json
kill -TERM "$sampler_pid" 2>/dev/null || true
wait "$sampler_pid" 2>/dev/null || true
peak_rss_mib /tmp/elysia2-build-rss.tsv

kill -TERM "$server_pid"
wait "$server_pid" 2>/dev/null
```

Repeat this for all three fixture entry points and at least three paired rounds
per runtime. Replace the `/tmp` output paths with paths under `"$RESULTS"` for
retained runs. Record:

```text
framework  runtime  round  requests_per_sec  peak_rss_mb  steady_rss_mb  success_pct
```

Require 100% success. Compare medians across paired rounds and retain each raw
round so variance remains visible. Keep `Accept-Encoding: identity`; compression
changes the workload.

## React Server Components

The RSC fixture is external to this repository:

```text
/private/tmp/test-rsc
```

It must contain dependencies plus `ant-vite-launcher.mjs`. The benchmarked URL
is `http://127.0.0.1:4173/_.rsc`. Do not use `/`: the current fixture's SSR root
returns HTTP 500 because of a separate React/Vite proxy invariant, while the
RSC stream returns HTTP 200.

Run `build`, `installed`, and `older` serially. Start each server with:

```sh
cd /private/tmp/test-rsc
RUNTIME=/Users/sf/Developer/ant/build/ant
"$RUNTIME" ant-vite-launcher.mjs --host 127.0.0.1 --port 4173 \
  >/tmp/rsc-build-server.log 2>&1 &
server_pid=$!
```

Wait for readiness and require HTTP 200:

```sh
until [ "$(curl --max-time 2 -sS -o /dev/null -w '%{http_code}' \
  http://127.0.0.1:4173/_.rsc)" = 200 ]; do sleep 0.05; done
```

Capture steady RSS, start a 50 ms RSS sampler, and run exactly 30 seconds at
one connection:

```sh
ps -o rss= -p "$server_pid"
sample_rss "$server_pid" "$RESULTS/rsc-build-rss.tsv" &
sampler_pid=$!
NO_COLOR=false oha http://127.0.0.1:4173/_.rsc \
  -z 30s -c 1 --no-tui --output-format json \
  -o "$RESULTS/rsc-build.json"
kill -TERM "$sampler_pid" 2>/dev/null || true
wait "$sampler_pid" 2>/dev/null || true
peak_rss_mib "$RESULTS/rsc-build-rss.tsv"

kill -TERM "$server_pid"
wait "$server_pid" 2>/dev/null || true
```

Do not replace this with a fixed request count. RSC throughput changes as the
heap reaches later phases, so short or fixed-count runs can materially
overstate performance.

Extract and record:

```sh
jq -r '.summary.requestsPerSec' "$RESULTS/rsc-build.json"
jq -r '.statusCodeDistribution["200"] // 0' "$RESULTS/rsc-build.json"
jq -r '.summary.successRate' "$RESULTS/rsc-build.json"
```

The result schema is:

```text
runtime  requests_per_sec  responses_200  peak_rss_mb  steady_rss_mb  success_rate
```

Require success rate `1.0`. Report throughput and peak RSS together. A GC
policy that reduces peak memory but materially lowers RPS, or raises RPS by
allowing unbounded growth, is not a complete improvement.

## Validation And Reporting

Before accepting a GC policy change:

1. Build a release/PGO-equivalent current binary.
2. Pass all 29 GC workloads.
3. Compare the GC suite against `installed` and `older` where supported.
4. Run the 30-second RSC matrix against all three binaries.
5. Run the Elysia/Hono matrix only when its required framework fixes are in the
   tested branch.
6. Run `maid preflight` and the full spec suite required by
   `docs/repo/testing.md`.

Keep raw outputs under `/tmp` for the duration of the investigation and put the
summary table, commit, build mode, and noteworthy outliers in this plan's
validation log or the plan for the specific change.

## Validation Log (2026-07-14, post-master-merge PGO build)

Merge commit `66e42515` plus the working-tree GC/buffer/json changes, fresh
PGO. Raw: `/tmp/ant-gc-ab/fw2/matrix.tsv`, `/tmp/ant-gc-ab/rsc30-v2-*`,
`/tmp/ant-gc-ab/rsc-v2-3000-*`. Medians of three paired rounds
(build/installed alternating), 100% success everywhere:

- Elysia 1: build 161.7k rps / 75.3 peak / 74.8 steady MiB; installed
  161.5k / 78.4 / 77.8 (rps parity, RSS -4%).
- Elysia 2: build 154.8k / 71.4 / 71.2; installed 156.4k / 75.4 / 74.6
  (rps parity within round overlap, RSS -5%; the prior session's +10.7%
  did not reproduce — installed measured higher today, so treat that
  delta as day-drift, not a regression).
- Hono: build 110.6k / 38.9 / 38.7; installed 111.1k / 51.2 / 48.3
  (rps parity, peak RSS -24%).
- RSC 30 s: build 325.1 rps / 9,754 responses / 4,054.8 peak / 294.4
  steady (0.416 MiB per response); installed 251.9 / 7,556 / 3,319.5 /
  340.0 (0.439); older 217.9 / 6,536 / 3,166.4 / 334.9 (0.484).
- RSC fixed-3,000 (equal work): build 398.8 rps / 1,469.5 MiB peak;
  installed 347.1 / 1,562.0; older 293.7 / 1,588.9.

## Validation Log (2026-07-14 pm, fresh PGO after buffer/path/regex fixes)

Raw: `/tmp/ant-gc-ab/fw3/matrix.tsv`, `rsc30-v3-*`, `rsc30-v3prod-*`. All
same-session serial, 100% success. Medians of three paired rounds:

- RSC prod (`vite preview`, the clean cross-binary metric): build
  1,457.6 rps / 205.8 peak / 107.3 steady MiB; installed 1,422.3 / 217.3 /
  110.8 (+2.5% rps, -5% peak, flat memory both).
- RSC dev 30 s: build 265.5 rps at 5,740 B/request = 1.52 MB/s delivered,
  289 MiB steady; installed 252.3 rps at 4,908 B/request = 1.24 MB/s,
  347 MiB steady. The regex index fix made React's dev owner-stack
  parsing work, growing build's payloads +17% — dev-mode rps is no longer
  comparable across fix levels; compare bytes/sec (+23%) or prod mode.
- Elysia 1: build 155.0k rps / 75.9 peak; installed 156.2k / 78.5
  (parity, RSS -3%). Elysia 2: 150.2k / 70.9 vs 150.1k / 74.7 (parity,
  RSS -5%). Hono: 108.2k / 38.8 vs 105.2k / 51.4 (parity rps, peak RSS
  -25%, steady -21%; one installed outlier round at 67.8k discarded by
  median).
- Typed-array churn 0.23 s / 19.7 MiB; gc_async 0.76-0.77 s / 23 MiB.
- Regex utf16, path.relative, buffer registry, webcrypto tests pass on
  this binary; machine measured ~10% slower overall than yesterday's
  session, so compare within-session only.

## Decision Log

- 2026-07-14: Pre-PR three-axis review (correctness/optimality/elegance
  subagents over the full working diff) found and fixed before commit:
  - CRITICAL: `regex_subject_mark_validated` ran after `pcre2_jit_match`,
    which performs no UTF validation — a WTF-8 (lone-surrogate) subject
    matched first by JIT could poison the cache and send invalid UTF-8
    into an interpreted `pcre2_match` under PCRE2_NO_UTF_CHECK (documented
    UB). Marking now happens only on the interpreted branch, at all three
    sites.
  - CRITICAL: empty-match loops advanced `match_end + 1` bytes
    (`string_split_impl` x3, `do_regex_match_pcre2`), which under the new
    NO_UTF_CHECK could hand pcre2 a mid-UTF-8-character start offset (UB;
    previously just a BADUTFOFFSET early-exit). All four advance by
    `utf8_char_len_at` now, which also fixes the truncated-result bug.
  - The validated-subject cache and it alone is now keyed on a new
    `gc_get_string_epoch()` (bumped only by major collections, where
    string pool memory is actually swept) instead of the minor-bumped GC
    epoch, so minor collections no longer kill it mid-loop.
  - NaN/huge-double guards in `regexp_units_to_byte` /
    `regexp_advance_units` (`lastIndex = NaN` now matches from 0 per
    ToLength, instead of arch-dependent cast UB); mid-surrogate index at
    end-of-string clamps to the end (whole-character convention).
  - `Buffer.write` utf8 path no longer writes partial multibyte sequences
    (Node contract); ucs2 encoder is bounded by dst (no full-source
    utf16_strlen) with an ASCII widening fast path.
  - WebCrypto `[[extractable]]` moved to a native field on
    `ant_crypto_key_t`; flipping the JS `extractable` property no longer
    permits export.
  - Split fast path: dropped unused parameter and the misleading in-loop
    `vstr` refetches (strings are non-moving mark/sweep).
  - Deferred (logged, not landed): `gc_maybe` tick re-arm (A/B showed no
    measurable difference under load; keeping the exact benchmarked
    cadence, revisit with quiet-machine benchmarks), d-flag indices
    descending-order conversions (rare, bounded), bounded base64
    write decode, `path_absolutize` pass-through/single-getcwd,
    unknown-encoding throw semantics, stats survivors/promoted merge.
  - Post-fix: spec 3,639/3,639, all regex/buffer/crypto/path/GC spot
    tests pass, split parity vs Node holds, preflight green.

- 2026-07-14: Regex split fast path + UTF validation cache
  (`src/modules/regex.c`, `src/ant.c`):
  - Profiling the split gap vs Node found `_pcre2_valid_utf_8` at 99% of
    samples: with PCRE2_UTF every interpreted `pcre2_match` re-validates
    the subject from the search offset to the end, making every
    exec/split/matchAll loop quadratic in subject size. Added an
    epoch-guarded validated-subject cache (`regex_subject_match_options` /
    `regex_subject_mark_validated`): ASCII subjects (memoized header flag)
    and subjects validated by a from-offset-zero call within the current
    GC epoch pass PCRE2_NO_UTF_CHECK. The epoch guard covers pool-pointer
    reuse after collection; lone-surrogate (WTF-8) subjects are never
    marked validated by a failed validation, so NO_UTF_CHECK is never
    applied to unvalidated non-UTF-8 bytes.
  - `RegExp.prototype[Symbol.split]` gained a scan-and-slice fast path
    (guarded on internal slots, default species via `same_ctor_identity`,
    builtin `exec` via `js_cfunc_same_entrypoint`, and numeric/absent
    limit): one leftmost pcre2 search per separator using the cached
    JIT-compiled pattern, all in byte space. Empty matches advance by one
    whole character regardless of the u flag, matching the engine's
    existing whole-character semantics (Node splits surrogate halves into
    lone-surrogate strings, which UTF-8 storage cannot express by
    slicing — pre-existing, unchanged divergence, now consistent between
    paths). 27 of 28 split parity cases match Node bytewise; the 28th
    matches installed.
  - Measured: warm `"str".split(regex)` on 50 KB went from 304 ms
    (installed) to 0.02-0.04 ms (~7,000x; Node 0.002 ms). The dense
    exec-loop microbench (`bench_regex.cjs` identifier split, 57k
    matches/round, ASCII) read +11% vs installed on stale-PGO incremental
    builds; after regenerating PGO the paired-round ratio median returned
    to ~1.0 (1.11/0.93/0.98/1.00/1.06 under heavy machine noise) — the
    delta was the stale-LTO-profile artifact, not the conversion code.
    Spec suite and preflight pass on the fresh-PGO binary.
  - Audited and fixed the `js_get` proto-walk gap: `js_try_get`'s plain
    T_OBJ branch does own-property lookup only (no `lkp_proto` walk,
    unlike its T_FUNC/T_CFUNC branches), so `js_get(rx, "exec")` always
    returned undefined for the prototype-resident builtin. Four regex.c
    sites switched to `js_getprop_fallback`: `regexp_exec_abstract` and
    `builtin_regexp_test` (proto-overridden `exec` is now dispatched per
    spec — verified Node-identical — and `test()` finally reaches its
    truthy-only branch instead of building a full match-result array),
    plus the replace/search plain-literal fast-path guards, whose
    entrypoint checks had been permanently false. Measured effect is
    modest (test() hit -10%, literal replace -8%; the disengaged guards
    had a working detour through the direct builtin call) — the
    substantive change is spec correctness. `js_get`'s own-only object
    semantics remain as-is: hundreds of call sites depend on it and a
    global change is out of scope; treat `js_get` as own-ish lookup and
    `js_getprop_fallback` as the spec-correct get.

- 2026-07-14: Production-mode RSC (`vite build` + `vite preview` on the same
  fixture, `/_.rsc`, 30 s, c=1) resolves the memory-growth attribution
  conclusively. Build binary: 1,620-1,632 rps / 48.6-49.0k responses /
  203-204 MiB peak / 107 MiB steady, RSS flat after warmup
  (107 -> ~202 MiB plateau); installed: 1,619 rps / 216 MiB peak / 111 MiB
  steady. Dev mode on the same binary in the same session: 327 rps /
  4,221 MiB peak / 287 MiB steady, linearly growing. Payload 2,287 B/req
  (prod) vs 4,908 B/req (dev). The multi-gigabyte linear retention is
  React/Vite dev-mode machinery, not the runtime or collector; the
  collector holds the heap flat for 48k responses in prod. Dev-mode RSC
  numbers remain useful for relative comparisons but must not be read as
  runtime memory behavior.
- 2026-07-14: Implemented `crypto.subtle.generateKey` (AES-GCM 128/192/256,
  HMAC with default-block-size or explicit bit length) and
  `crypto.subtle.exportKey("raw")`, honoring the extractable flag (which
  `importKey` now also records, along with usages). This was the root cause
  of `ant x --ant vite dev` crashing with "undefined is not a function":
  `@vitejs/plugin-rsc` generates an AES-GCM-256 action-encryption key at
  startup. The RSC benchmark fixture's `ant-vite-launcher.mjs` polyfill
  preamble exists solely to paper over this and is now a no-op; plain
  `ant node_modules/vite/bin/vite.js dev` and `ant x --ant vite dev`
  (with a fixed `ant` first in PATH — `x --ant` re-execs `ant` via PATH)
  both boot and serve `/_.rsc`. Covered by
  `tests/test_webcrypto_generate_export.cjs`; crypto spec 21/21, full spec
  suite passes. Asymmetric generateKey and non-raw export formats remain
  unsupported and reject with clear TypeErrors.
- 2026-07-14: The `vite build` 0-byte `dist/rsc/index.js` bug is fully
  root-caused and fixed. It was a chain of three independent Ant runtime
  bugs, each isolated by bisecting the fixture and diffing against Node:
  1. `Buffer.prototype.write` ignored its encoding argument (wrote UTF-8
     bytes for "utf16le"), so es-module-lexer's wasm parser received
     byte-packed ASCII and silently returned zero imports, collapsing the
     plugin-rsc scan pass (2 modules crawled vs 24 under Node). Fixed in
     `src/modules/buffer.c`: `write` now honors Node's flexible
     `(string[, offset[, length]][, encoding])` signature via per-encoding
     `buffer_encode_*_into` helpers (ucs2, bounded no-alloc hex, base64,
     utf8/latin1 memcpy), and `Buffer.byteLength` shares
     `buffer_encoded_byte_length`. Covered by extended checks and the
     byte-identical dist comparison below.
  2. `path.relative` did not resolve its arguments against the working
     directory (`relative('.', 'm.js')` returned `"../m.js"`), producing a
     wrong `../__vite_rsc_assets_manifest.js` import (also the cause of the
     earlier preview-server manifest failure). Fixed in
     `src/modules/path.c` via `path_absolutize` before normalization,
     matching Node's `relative(resolve(from), resolve(to))`. Covered by
     `tests/test_path_relative_resolve.cjs` (Node-verified).
  3. Regex `match.index`, `lastIndex`, d-flag `indices`, replacer offsets,
     and `search` results were PCRE2 byte offsets, not UTF-16 code units
     (`/M/.exec("é".repeat(100)+"M").index` returned 200). In the rsc
     chunk, vite:asset spliced a public-asset URL 136 chars off target,
     corrupting the JS so rolldown's native re-parse emptied the chunk.
     Fixed in `src/modules/regex.c`: all JS-visible positions convert at
     the pcre2 boundary (`regexp_byte_to_units` /
     `regexp_units_to_byte` / `regexp_advance_units`), with byte-space
     retained internally for slicing (replace/split) and an ASCII fast
     path making conversion free for ASCII subjects. Covered by
     `tests/test_regex_utf16_positions.cjs` (Node-verified, 20 checks).
     Perf note: the utf16 scan cache only resumes forward, so exec paths
     convert match-start before match-end (descending targets caused a
     full-prefix rescan per match: 100x slowdown on non-ASCII matchAll,
     caught by profiling and fixed the same day). Measured after the
     ordering fix, vs installed: all ASCII regex benches equal within
     noise (bench_regex.cjs paired medians 108.1 vs 105.6 ms on route
     matches, rest identical); non-ASCII subjects pay ~2.7-3.1x per-match
     conversion on matchAll/replace (about 1 microsecond per match
     absolute) and +13% on dense exec — the price of correct indices;
     unicode split is 19% faster than installed. Split on large inputs is
     slow on BOTH binaries (~305 ms/50 KB ASCII, probe-per-position
     exec loop) — pre-existing, separate follow-up.
  After all three fixes, `ant node_modules/vite/bin/vite.js build`
  produces a dist tree BYTE-IDENTICAL to Node v26's, and `vite preview`
  under Ant serves the Ant-built bundle (HTTP 200 Flight payload).
  Spec suite 3,639/3,639, preflight green, no perf regressions on the
  typed-array/gc_async/RSC spot checks. PGO must be regenerated before
  final benchmark numbers.

- 2026-07-14: Undecorated `Error.stack` was implemented, measured, and then
  REVERTED to preserve the existing error-display UX. Corrected findings
  from that experiment:
  - The earlier "~173 KB per stack string" attribution was wrong. Rich
    stacks are ~2-4 KB (the source snippet is clipped to roughly half the
    terminal width with a 120-column fallback, and frame walks self-limit
    in practice). The ~90 MiB `js_build_stack_text` figure in
    malloc_history was string-POOL CHUNKS whose creation happened to be
    triggered during stack capture; the chunks mostly hold unrelated live
    strings. Leaf-frame attribution over pool allocators is misleading —
    treat `js_type_alloc -> mmap` tails as pool-chunk creation, not as the
    leaf's own footprint.
  - Same-build-state 30 s RSC A/B was still reproducibly different with
    plain stacks: peak RSS 4,005-4,327 MiB (rich) vs 2,977-3,054 MiB
    (plain), steady 292-296 vs 260-263 MiB, and `sizePerRequest` grew
    4,908 to 6,876 bytes with bytes/sec up (1.64 to 1.69 MB/s): React's
    Flight dev machinery parses V8-shaped stacks and changes what it
    retains/serializes. The memory delta is a React-dev interaction, not
    stack-string size. Open follow-up if ever revisited (for example as an
    opt-in Node-compat stack format); default UX stays as is.
  - Lazy stack materialization was evaluated and rejected: React reads
    every owner-stack Error it creates, so laziness materializes them all
    anyway, and a snapshot faithful enough to reproduce the rich text is
    about the same size as the string it defers.

- 2026-07-13: The typed-array/Buffer churn slowdown was not collector policy.
  `unregister_buffer` in `src/modules/buffer.c` did a linear scan of the
  global buffer registry for every freed ArrayBuffer, making every GC sweep
  of dead buffers quadratic in the live-buffer population. Replaced with an
  intrusive `registry_slot` index (O(1) swap-removal; slot verification keeps
  never-registered wasm/wasi/structured-clone buffers safe).
  `tests/repro_typedarray_metadata_leak.cjs`: 0.95 s before, 0.29 s after,
  versus 0.45 s for both reference binaries. Covered by
  `tests/test_buffer_registry_slots.cjs`.
- 2026-07-13: The 16K nursery start was calibrated against that registry
  defect (smaller nursery meant smaller quadratic sweeps). With the fix, a
  32K start wins the same A/B (0.29 s vs 0.39 s), so the start returns to
  `GC_NURSERY_THRESHOLD`.
- 2026-07-13: Nursery growth cap reduced from 8x to 2x (64K), matching the
  documented policy. Fixed-3000-response RSC: 64K cap 275.0 rps / 1675.7 MiB
  peak, 256K cap 273.7 rps / 1672.5 MiB (equal within noise), fixed 32K
  251.2 rps / 1728.5 MiB (worse). Remembered-writer growth stays: the signal
  pays for itself but saturates by 64K.
- 2026-07-13: Rejected re-adding minor-first collection on the live-pressure
  trigger and judging majors on post-minor live. Measured on fixed-3000 RSC:
  223.7-236.1 rps / 1777 MiB versus 261.9-273.7 rps / 1556-1672 MiB for the
  existing policy. Majors fell 80 to 32 and object-reclaim EWMA rose to 85,
  yet throughput dropped: this fixture rewards a compact heap (strings,
  ropes, shapes are only swept by majors, and object-count reclaim EWMA is
  blind to pool bytes). The original minor-first motivation (about 1,100
  pool-pressure majors on the allocation-heavy bench) no longer reproduces
  after the registry fix (14 majors, about 1.3 ms each).
- 2026-07-13: `GC_POOL_PRESSURE_FLOOR` reduced from 8 MiB to 1 MiB. The floor
  only governs pools whose live bytes are under `floor / growth`; at 8 MiB it
  allowed a 1.4 MiB-live pool to grow about 7x between collections while
  large pools were held to 1.25-1.5x growth. On `tests/test_gc_async.js`
  (string/shape-heavy coroutine churn) the accumulation slowed the mutator
  itself (regex/interning/shape walks over a bloated pool): 1.26 s at 8 MiB,
  0.83 s at 2 MiB, 0.68 s at 1 MiB versus 0.66 s installed, with RSS 32 to
  23 MiB. Typed-array churn RSS fell 36.7 to 19.9 MiB at equal runtime, and
  fixed-3000 RSC improved to 360.5 rps / 1472 MiB peak. Majors this small are
  about 2 ms, so the added frequency is cheap where the floor applies.
- 2026-07-14: The per-response RSS growth attribution was corrected. A no-JIT
  build (after gating `sv_tfb_*` stubs so `-Djit=false` compiles again) shows
  the same linear growth (0.41 MiB/response vs 0.49 with JIT), so MIR
  compilation is a secondary component, not the driver. The no-JIT build is
  also faster on RSC (317.7 vs 283.2 rps) — JIT compile cost does not pay
  for itself on this workload; separate follow-up.
- 2026-07-14: Found and fixed a real `JSON.parse` leak dating to `f544621a`
  (2026-01-25, present in all reference binaries): four unbraced
  `HASH_ITER(...) HASH_DEL(...); free(...)` cleanups in
  `src/modules/json.c` ran `free` once after the loop (on NULL) instead of
  per entry, leaking one ~48-byte `json_key_entry_t` per object key per
  parse. On the RSC fixture: 880,740 leaked blocks / 84.3 MB per 1,500
  requests before; 4,507 / 196 KB after (zero `yyjson_to_jsval` stacks).
  Per-response RSS growth fell from 0.486 to 0.432 MiB. JSON spec 36/36.
  The other ~60 `HASH_ITER` sites in the tree are single-statement bodies
  and correct.
- 2026-07-14: Live-allocation attribution of the remaining reachable growth
  (malloc_history after 1,000 RSC requests): shapes ~207 MiB across
  `ant_shape_clone`/`ant_shape_add_interned_tr`/`shape_add_key` (~1M
  blocks; a JS repro shows shape sharing works and stabilizes for repeated
  key sets, so this is floating-until-major plus possibly novel keys —
  unresolved), `js_build_stack_text` ~90 MiB (517 live stack strings
  averaging ~173 KB: `Error.stack` stores the fully rendered ANSI error
  report with source context, and React dev/RSC creates Errors per element
  for owner stacks — candidate fix is a plain V8-style `.stack` string with
  rich rendering only in the reporter; user-visible, needs a decision),
  MIR compile memory ~130 MiB (JIT builds only), rolldown `dlopen` ~1 GiB
  VA (one-time). Peak-RSS ratcheting is amplified by the allocator keeping
  freed small/large pages dirty (MALLOC_LARGE empty 312 MB dirty observed),
  so reducing transient malloc traffic (shapes, stack text) lowers the
  ratchet even where nothing leaks.
- 2026-07-13: 30-second RSC RSS growth (about 80 MiB/s, MALLOC_SMALL dirty
  647 MB to 1.6 GB in vmmap) is malloc-side while the GC object arena stays
  bounded (about 110 MB committed). Consistent with the MIR/module-metadata
  attribution in the JIT PUT_FIELD plan; not addressable by collector policy.
  A `-Djit=false` falsification build does not compile on this branch
  (`sv_tfb_record_local` is not gated), so attribution rests on the earlier
  allocation-stack logging plus the vmmap split.
- 2026-07-13: Standardized GC coverage on the 29-file set used in the JIT
  write-barrier investigation.
- 2026-07-13: Standardized Elysia/Hono measurements on a 100,000-request warmup
  followed by a five-second run at concurrency 50.
- 2026-07-13: Standardized the original RSC reproduction on `/_.rsc`, 30
  seconds, and concurrency 1. Fixed-count RSC runs are not comparable.
- 2026-07-13: Throughput and peak/steady RSS are joint acceptance criteria.

## Validation Status

- 2026-07-13, commit `bd4718a1` plus the working-tree collector/buffer
  changes, PGO regenerated after the final policy
  (`ANT_PGO_IN_NIX_SHELL=1 ./meson/pgo/build.sh`). Raw outputs:
  `/tmp/ant-bench-20260713-182625` (GC suite TSVs) and `/tmp/ant-gc-ab`
  (RSC runs, RSS samples, profiles). All comparisons below were run
  serially in the same session; `installed` = `0f5e061c`,
  `older` = `dbfffe9b`, both PGO release binaries.
- GC suite: 29/29 pass on build, installed, and older
  (`gc-final-status.tsv`, `gc-installed-status.tsv`, `gc-older-status.tsv`);
  `test_gc_stress10.js` recorded as EXPECTED_TIMEOUT.
- 30-second RSC, `/_.rsc`, c=1, 50 ms sampling, success 1.0 everywhere:
  build 283.2 rps / 8497 responses / 4127 MiB peak / 356 MiB steady;
  installed 214.5 rps / 6434 / 2946 / 352; older 209.1 rps / 6271 / 2970 /
  341. Peak RSS per completed response is 0.486 / 0.458 / 0.474 MiB —
  equal within 6%; the higher absolute peak is more completed work inside
  the window (the per-response malloc retention is the documented non-GC
  MIR/module growth).
- Equal-work RSC (fixed 3,000 responses, diagnostic): build 370.6 rps /
  1570 MiB peak; installed 289.6 rps / 1707 MiB; older 268.7 rps /
  1514 MiB.
- Typed-array churn (`repro_typedarray_metadata_leak.cjs`): build 0.25 s /
  19.6 MiB versus installed 0.40 s / 36.8 MiB and older 0.43 s / 34.8 MiB.
- `test_gc_async.js`: build 0.68-0.86 s at 23 MiB versus installed
  0.66-0.75 s at 24 MiB (was 1.26 s before the pool-floor change; the
  residual delta is within same-machine drift observed on installed).
- Telemetry: `ANT_DEBUG=gc:stats` runs measure identically to disabled runs
  on the typed-array workload (0.25-0.26 s both ways); counters are exact
  totals, pause times are per-collection monotonic clock sums, and the one
  dump happens at exit.
- Spec suite 3,639/3,639 across 97 files; `maid preflight` and
  `maid knowledge` pass; buffer spec 197/197; new
  `tests/test_buffer_registry_slots.cjs` passes on all three binaries and
  under MallocScribble/MallocPreScribble (ASan builds are currently blocked
  in this environment: nix clang-21 lacks the sanitizer interface header
  and the system linker rejects the Zig-built `libpkg.a`).
- Elysia/Hono RPS (2026-07-13, after confirmation that the framework fixes
  are on this branch): three paired rounds per framework, 100k-request
  warmup at c=50, 5 s measured runs, alternating build/installed, 100%
  success everywhere. Medians (rps / peak RSS / steady RSS):
  - Elysia 1: build 159.8k / 76.2 / 75.9 MiB; installed 162.5k / 79.3 /
    78.7 MiB (rps within round-to-round overlap, RSS -4%).
  - Elysia 2: build 156.8k / 70.5 / 70.2 MiB; installed 141.6k / 74.2 /
    73.3 MiB (build +10.7% rps, -5% RSS).
  - Hono: build 109.9k / 38.7 / 38.0 MiB; installed 109.8k / 51.5 /
    49.8 MiB (equal rps, -25% peak / -24% steady RSS; the 1 MiB pool
    floor trims this small-heap server where the 8 MiB floor let pools
    bloat).
  Raw rounds: `/tmp/ant-gc-ab/fw/matrix.tsv` plus per-run JSON/RSS
  samples in `/tmp/ant-gc-ab/fw/`.

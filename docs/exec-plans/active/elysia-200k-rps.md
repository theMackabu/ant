# Elysia 200k RPS

Status: active
Last reviewed: 2026-08-30
Owner: theMackabu
Owner areas: `src/modules/response.c`, `src/modules/headers.c`,
`src/modules/server.c`, `src/silver/`, object layout, PGO

## Goal

Raise Ant's canonical Elysia 1 and Elysia 2 plain-text HTTP throughput to at
least 200,000 requests per second on the dedicated Darwin ARM64 benchmark host,
without weakening JavaScript or Web API semantics, hiding deferred work, or
regressing Hono, bench-v8, memory, startup, or correctness.

The work has two distinct milestones:

1. Recover the useful performance represented by the stale PR #44 archive
   (`/private/tmp/ant`) through isolated current-tree implementations.
2. Profile and remove the remaining current-runtime and server bottlenecks
   needed to move from the historical PR #44 range of roughly 150--162k RPS to
   200k RPS.

Porting all old PR #44 code is not the objective. Every retained change must be
safe against current layouts and semantics and must independently earn its
place through controlled measurement.

## Success Criteria

The plan is complete only when all of the following hold:

- Elysia 1 and Elysia 2 each sustain a median of at least 200k RPS in the
  canonical server protocol with 100% successful responses.
- The final result survives at least seven serial, order-rotated paired rounds
  and a longer confirmation run after cooldown.
- Peak and steady-state RSS plateau. A throughput gain obtained by allowing
  unbounded memory growth does not count.
- Hono does not regress outside measured noise. The final bench-v8 geometric
  mean and every materially affected test must also remain at parity or improve.
- The no-server and stage harnesses return the same observable values on every
  compared binary.
- Focused Response, Headers, string, Proxy, subclassing, optional chaining,
  exception, GC, and server tests pass, followed by the full validation gates
  required by `docs/repo/testing.md`.
- The final binaries are source-matched, profile-matched PGO/LTO builds. A
  result from a stale or structurally incompatible profile is diagnostic only.

## Recorded Starting Evidence

These measurements motivate the plan but are not yet the pinned AB/BA baseline
required for a final performance claim.

### Server throughput

| Workload | Current reported RPS | Historical PR #44 plan range |
| --- | ---: | ---: |
| Elysia 1 | 122k | 155.0k--161.7k |
| Elysia 2 | 126k | 150.2k--156.8k |
| Hono | 98k | 108.2k--110.6k |

Moving the reported current results to 200k requires approximately +64% for
Elysia 1 and +59% for Elysia 2. Recovering the historical PR #44 peak would
still leave roughly another 24--33% improvement to find, depending on the
framework and same-session baseline.

The historical plan contains only real HTTP `bench-server` results. It has no
authoritative `bench-no-server` result, and its informal approximately 175k RPS
claim also refers to server throughput.

### Compared binaries on 2026-08-30

| Label | Binary | Embedded version | SHA-256 |
| --- | --- | --- | --- |
| PR #44 archive | `/private/tmp/ant/build/ant` | `13.0.ca8e720d.0` plus the dirty PR diff | `3b0f0997b0f9a571a4acd9862934944af0a661e25cb6ffddb0feee0692515c53` |
| Current | `build/ant` and `/Users/themackabu/.ant/bin/ant` | `14.1.40f8ea68.0` | `1c05b58c475d80b0987eafb64ce298bba3b9da07980e94c921ecc9023098af90` |

Both configured trees use release `-O3`, full LTO, and an explicit
revision-local PGO profile. Their profiles are different, so these results show
the performance represented by each complete artifact but cannot by themselves
attribute a source change.

### No-server fixed-work measurements, 50,000 calls

| Workload | PR #44 archive | Current |
| --- | ---: | ---: |
| Elysia 1 `app.fetch`, no logger | 17.03ms | 38.06ms |
| Elysia 2 `app.fetch`, no logger | 22.82ms | 47.26ms |

### Elysia 2 stage measurements, 50,000 calls

| Stage | PR #44 archive | Current | Initial interpretation |
| --- | ---: | ---: | --- |
| Context construction | 7.38ms | 5.47ms | Current is already faster; do not revive old context machinery. |
| Request URL path parse | 2.48ms | 4.94ms | PR string-call specialization and cached one-byte results are leading candidates. |
| Response tag | 0.99ms | 0.86ms | Already at parity; not a target. |
| Compact string response | 6.55ms | 20.47ms | Dominated by `new Response(string, { headers })`; lazy Headers/native allocation are leading candidates. |
| Inline handler, reused context | 6.86ms | 19.41ms | The handler call is negligible; this measures the same Response path. |
| Context + path + inline handler | 17.02ms | 31.43ms | Confirms multiple independent costs rather than one global slowdown. |

The compact-response harness does not read `response.headers`. Lazy Headers
therefore defers work beyond that timed region. This is a valid implementation
strategy, but the microbenchmark overstates its HTTP benefit unless a forced
header-materialization stage and the real server benchmark improve too.

## Constraints

- Treat `/private/tmp/ant` as a read-only hypothesis archive, not mergeable
  source. Reimplement ideas against the current tree.
- One performance mechanism per retained commit. Do not bundle a string
  intrinsic, Response layout, object compaction, allocator policy, and PGO into
  one un-attributable result.
- Pin the base binary before each candidate. Record commit, dirty diff hash,
  compiler, linker, build options, PGO profile hash, executable hash, fixture
  lockfiles, power state, and raw samples.
- First compare equivalent no-PGO LTO builds to isolate source behavior. Final
  claims require separately self-profiled base and candidate PGO builds from
  the same training protocol. Use a cross-profile matrix when control-flow or
  layout changes make profile compatibility questionable.
- Never benchmark immediately after compiling or generating PGO. Allow at
  least five minutes of cooldown and stop other builds, Ant servers, and
  CPU-heavy applications.
- Run servers serially and rotate AB/BA order. Never run comparison servers or
  load generators concurrently.
- Preserve property overrides, accessors, Proxies, prototype mutation,
  subclass/new-target behavior, coercion and argument evaluation order,
  optional chaining, `try`/`finally`, explicit-resource cleanup, GC rooting,
  UTF-16 indexing, and exception propagation.
- Do not optimize the harness, recognize benchmark source, special-case
  framework names, or omit work required by real HTTP serialization.
- Keep throughput and memory together. Any GC or allocation policy must show a
  stable RSS plateau under a longer fixed-duration run.
- Do not resurrect PR #44's old GC policy, untraced dynamic-key IC, stale object
  site propagation, hard-coded MIR layouts, or unchecked fallible APIs.

## Canonical Benchmark Protocol

### Durable fixtures

The Elysia fixtures live under `examples/bench-elysia/elysia1` and
`examples/bench-elysia/elysia2`. Preserve their lockfiles and exact resolved
versions. Keep Hono pinned through `examples/npm/hono`.

The durable suite must include:

- `bench-server` for Elysia 1, Elysia 2, and Hono;
- `bench-no-server` modes for normal fetch, cached fetch, cached Response,
  response mapping, and raw Response construction;
- `bench-stages` for context construction, URL parsing, response tagging,
  compact response, inline handler, and the combined path;
- a forced `response.headers` access stage;
- a header-iteration/serialization stage so deferred materialization is charged;
- result/status/body/header checks or checksums outside the timed loop.

### Binary and profile identity

For every retained candidate, record:

```text
label
commit
dirty_diff_sha256
compiler_and_linker
build_options
pgo_training_revision
pgo_profile_sha256
binary_sha256
fixture_lockfile_sha256
```

Run two matrices:

1. Identical-toolchain no-PGO LTO base versus candidate.
2. Self-profiled PGO/LTO base versus self-profiled PGO/LTO candidate.

If the direction flips, generate a cross-profile matrix and inspect discarded
counter warnings before attributing the change.

### No-server and stage runs

- Warm each function for at least 2,000 iterations.
- Use at least 50,000 measured iterations, increasing the count until a sample
  is long enough to be stable on the benchmark host.
- Alternate binaries in AB/BA order for at least five paired rounds.
- Preserve every raw sample; report median, minimum, maximum, and median
  absolute deviation.
- Run one selected stage per process for final attribution so one stage's GC or
  JIT state does not contaminate the next.

### Server runs

For each framework and binary:

1. Start exactly one server and wait for a successful readiness request.
2. Warm with 100,000 requests at concurrency 50.
3. Capture steady RSS after warmup.
4. Measure with `oha`, concurrency 50, and `Accept-Encoding: identity`.
5. Sample RSS every 50ms during the measured interval.
6. Require 100% success and verify the expected response body and headers.
7. Stop the sampler and server and prove port 3000 has no surviving process.

Five-second runs are acceptable for iteration. Retained milestones require at
least five paired 15-second rounds. The final 200k claim requires at least
seven paired rounds plus a 60-second confirmation that preserves throughput and
shows a stable memory plateau.

## PR #44 Silver Idea Disposition

| PR #44 idea | Plan decision |
| --- | --- |
| Widen `GET_ELEM` with an IC index | Do not port. The PR caches an untraced `ant_value_t` key, while current master already specializes dense numeric elements without widening the opcode. |
| Widen `DEFINE_FIELD` for object-literal ICs | Superseded by current precomputed object-literal key sequences and `obj_sites`. |
| Widen `NEW` for constructor/prototype ICs | Redesign only if post-Response profiles show construction dispatch or prototype lookup remains material. Use current construction metadata. |
| Widen `IMPORT_NAMED` with an IC | Valid concept but low-value for steady-state RPS. Revisit only for a separately measured startup target. |
| Dedicated `CALL_STRING_INDEXOF` and `CALL_STRING_SUBSTRING` | Preserve the idea, not the encoding. Use one operation-driven current call/intrinsic design if it wins. |

`include/silver/glue.h` has no independent optimization. Add helper declarations
only with a selected implementation; never grow the glue surface in advance.

## Phase 0: Freeze the Reproducible Baseline

- [x] Move the Elysia fixtures out of `examples/tmp` and document their pinned
      dependency versions and invocation.
- [x] Add forced Headers materialization and serialization stages.
- [ ] Pin current no-PGO and self-profiled PGO binaries.
- [ ] Pin the PR #44 archive binary only as an orientation/reference artifact.
- [ ] Run five paired AB/BA rounds for no-server, every stage, and all three
      servers.
- [ ] Record response correctness, RSS, GC counts, and raw samples.
- [ ] Add opt-in aggregate counters for Response/Headers allocations,
      materializations, string intrinsic hits/fallbacks, one-character string
      allocations, generic calls, and server header serialization.

Exit criteria:

- Variance is low enough to distinguish a 2% change.
- Repeated runs reproduce the major stage gaps.
- All binaries execute the same locked fixtures and return identical results.
- The baseline distinguishes deferred Headers work from completed serialization.

## Phase 1: Lazy, Fallible Response Headers

This is the highest-value measured candidate. Current `mapCompactResponse`
eagerly creates a JS `Headers` object for every string response. PR #44 keeps
native pending header data and materializes the wrapper only when observed.

### Design

- [x] Give `response_data_t` an explicitly owned pending-headers state without
      ambiguous sentinels or unchecked ownership transfer.
- [x] Construct a Response without a public `Headers` wrapper until JS observes
      `.headers` or an API requires the object.
- [x] Let the server serialize native pending header data directly when safe,
      avoiding materialization solely for native output. Once materialized,
      preserve the stable JS object identity and serialize its current data.
- [x] Make every native pending-header create/copy/append/materialize operation
      fallible.
- [x] Replace the old ambiguous `response_get_headers` contract with an API
      whose error result is unavoidable at the call site. Both server
      serialization paths must check and propagate failure before enumeration.
- [x] Clone pending data before materialization and clone materialized Headers
      after materialization, preserving mutation independence and guard state.
- [x] Preserve the already-landed borrowed immutable string-body ownership.

### Guarded simple-init path

The first implementation deliberately omitted the old PR shortcut because a
pre-conversion absence check is invalid: converting `headers` can execute JS
and add inherited `status` or `statusText`. The current candidate uses a
stronger proof that does not require a general prototype epoch:

- [x] Recognize only an ordinary, non-exotic exact shape whose sole own data
      property is interned `headers` and whose prototype is the canonical
      Object prototype.
- [x] Convert `headers` in specification order, then revalidate the init shape,
      its prototype, the Object prototype chain, and the continued absence of
      inherited `status` and `statusText` before skipping their generic reads.
- [x] Fall back for accessors, Proxies, subclasses, altered prototypes, extra
      properties, or any uncertain state.
- [x] Preserve specification evaluation order and exceptions, including a
      regression where nested HeadersInit conversion mutates
      `Object.prototype.status`.

### Correctness gates

- `.headers` object identity and mutation before/after native serialization;
- clone before and after materialization;
- plain object, accessor, Proxy, inherited `headers`, `status`, and `statusText`;
- Response subclasses and custom `new.target.prototype`;
- content-type insertion, duplicate headers, ByteString/obs-text, immutable
  fetched headers, and OOM/failure propagation;
- server serialization must never enumerate an error value.

Performance gate:

- Compact Response improves materially when headers remain unobserved.
- Forced materialization and real HTTP also improve or remain neutral.
- Elysia 1 and 2 both improve; Hono and RSS do not regress.

### Phase 1 no-PGO LTO result

Six serial order-rotated pairs used 1,000,000 iterations per process after the
required build cooldown. Lower elapsed time is better. All six pairs favored
the candidate for every row.

| Workload | Base median (min--max), ms | Candidate median (min--max), ms | MAD base/candidate | Time reduction | Throughput gain |
| --- | ---: | ---: | ---: | ---: | ---: |
| Compact string Response | 451.36 (436.93--479.28) | 371.24 (354.04--384.82) | 9.38 / 2.56 | 17.75% | 21.58% |
| Compact Response + `.headers` | 603.05 (582.59--625.49) | 558.07 (539.13--580.74) | 13.54 / 4.68 | 7.46% | 8.06% |
| Compact Response + header iteration | 1214.27 (1147.58--1254.65) | 1145.64 (1121.40--1164.45) | 18.13 / 13.06 | 5.65% | 5.99% |
| Elysia 1 `app.fetch` | 964.78 (951.96--981.61) | 878.66 (865.37--898.09) | 8.52 / 6.41 | 8.93% | 9.80% |
| Elysia 2 `app.fetch` | 1213.60 (1169.56--1244.99) | 1124.67 (1096.23--1137.41) | 16.90 / 9.07 | 7.33% | 7.91% |
| Cached Response clone | 189.34 (184.45--192.96) | 143.53 (140.40--150.99) | 2.63 / 3.06 | 24.20% | 31.92% |
| `new Response` without explicit headers | 203.32 (198.22--213.30) | 163.09 (155.54--184.60) | 4.22 / 3.69 | 19.78% | 24.66% |
| `new Response` with text header | 422.12 (409.19--430.17) | 338.96 (327.63--346.70) | 7.80 / 3.84 | 19.70% | 24.53% |

Raw base/candidate samples, in round order:

```text
compact:       B 436.93 452.57 444.41 450.14 463.16 479.28
               C 374.33 354.04 369.36 369.20 373.11 384.82
headers:       B 582.59 591.14 588.84 615.92 614.96 625.49
               C 554.52 539.13 559.58 556.56 563.88 580.74
iteration:     B 1147.58 1185.96 1214.76 1222.23 1213.78 1254.65
               C 1131.15 1147.12 1164.45 1144.16 1121.40 1157.27
elysia1 fetch: B 956.87 951.96 965.19 973.90 964.36 981.61
               C 878.84 898.09 880.59 878.48 865.37 867.77
elysia2 fetch: B 1192.43 1169.56 1206.27 1244.99 1226.22 1220.92
               C 1096.23 1110.43 1130.06 1123.24 1137.41 1126.09
clone:         B 184.45 189.27 187.24 189.41 192.96 192.50
               C 141.36 140.40 145.69 140.53 148.46 150.99
no headers:    B 199.97 198.22 202.67 203.96 208.64 213.30
               C 155.54 160.75 164.15 162.03 184.60 168.12
text header:   B 409.19 412.28 425.42 429.67 430.17 418.82
               C 327.63 338.12 346.70 333.78 341.45 339.80
```

Five serial order-rotated real-HTTP pairs then used 100,000 warmup requests and
a five-second `oha` interval at concurrency 50. Every run returned only HTTP
200 with success rate 1.0, the expected body and `content-type`, zero oha
errors, and no surviving listener on port 3000.

| Workload | Base RPS median (min--max) | Candidate RPS median (min--max) | MAD base/candidate | Median change | Warm RSS median base/candidate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Elysia 1 | 108,209 (105,094--111,908) | 109,353 (107,259--110,893) | 2,054 / 1,517 | +1.06% | 39,920 / 41,456 KiB |
| Elysia 2 | 105,866 (104,772--107,080) | 106,299 (104,351--106,811) | 1,094 / 512 | +0.41% | 43,760 / 44,816 KiB |
| Hono | 87,517 (86,594--88,748) | 88,441 (87,743--90,184) | 800 / 698 | +1.06% | 40,864 / 39,840 KiB |

```text
elysia1: B 105094 108209 111908 108030 110262
          C 109353 107259 108283 110893 110870
elysia2: B 105866 104772 107080 105785 107065
          C 106299 104351 106564 106811 105738
hono:     B 86717 86594 88748 87517 87755
          C 89272 87796 88441 87743 90184
```

The paired Elysia directions flip and the medians are inside short-run noise,
so this is server-neutral evidence, not a throughput claim. Hono is also
non-regressed. The warm RSS delta is small but consistently positive for the
two Elysia fixtures; the required long plateau run remains open. Phase 1 is
therefore provisional pending the self-profiled PGO matrix rather than retained
as a standalone server win.

## Phase 2: Current String-Call Intrinsic and ASCII Character Cache

The Elysia router repeatedly executes:

```js
const start = url.indexOf("/", pathStart);
const query = url.indexOf("?", start);
return url.substring(start, query === -1 ? url.length : query);
```

PR #44 avoids generic call dispatch when the evaluated method is the original
builtin and caches a one-character ASCII substring such as `"/"`.

### Design

- [x] Use one operation-driven call form, conceptually
      `CALL_STRING_INTRINSIC(kind, argc)`, or an equivalent current metadata
      design. Do not add one fixed opcode/helper family per method.
- [x] Initially support only `indexOf` and `substring`.
- [x] Evaluate the receiver and actual method property normally, then take the
      fast path only when the resolved function is the expected builtin.
- [x] Fall back through the normal call path for patched prototypes, own
      methods, subclasses, or any failed guard.
- [x] Snapshot arguments before receiver or argument coercion can execute JS,
      grow the VM stack, and invalidate pointers into it.
- [x] Preserve optional-base short-circuiting, exceptions, cleanup regions,
      argument order, `ToString`, `ToIntegerOrInfinity`, and UTF-16 indexing.
- [x] Share the semantic implementation with the ordinary builtin rather than
      maintaining a second approximate `indexOf`/`substring` algorithm.
- [x] Add a lazy `ant_value_t ascii_chars[128]` per-isolate cache, or a measured
      equivalent, and expose a narrow known-one-byte helper. The table costs
      1KiB per isolate; character strings are allocated permanently only on
      first use.
- [x] Do not add an ASCII-cache branch to every `js_mkstr()` call unless a
      separate benchmark proves that global policy beneficial.

Correctness gates include primitive/object receivers, side-effecting coercion,
Symbols, negative/NaN/infinite/fractional indices, lone surrogates, astral text,
empty strings, overridden methods, optional calls, stack reallocation, and
interpreter/JIT parity.

Performance gate:

- URL path parsing approaches or beats the PR #44 reference without regressing
  generic string calls.
- Both intrinsic hit rate and fallback rate are recorded on Elysia.
- The Elysia server result improves after fresh PGO; otherwise remove the
  bytecode specialization even if the microbenchmark wins.

### Phase 2 no-PGO LTO result

The candidate is pinned at `/tmp/ant-elysia-string-nopgo` with SHA-256
`0924f08066b08cfb440e7fb403bd3714fd1a2a408974d8177b1a55076c9b2d9a`.
Its comparison base is the Phase 1 binary
`/tmp/ant-elysia-response-nopgo`. Both are release `-O3`, full-LTO, no-PGO
builds from the same toolchain. Six serial order-rotated pairs used 2,000,000
URL iterations or 1,000,000 Elysia fetch iterations per process.

| Workload | Base median, ms | Candidate median, ms | Time reduction | Throughput gain |
| --- | ---: | ---: | ---: | ---: |
| URL `indexOf`/`substring` loop | 263.76 | 239.71 | 9.12% | 10.03% |
| Elysia 1 `app.fetch` | 872.51 | 854.04 | 2.12% | 2.16% |
| Elysia 2 `app.fetch` | 1124.41 | 1113.30 | 0.99% | 1.00% |

```text
URL:     B 263.99 263.52 262.42 267.75 261.80 264.87
         C 239.82 239.09 239.59 240.61 239.13 242.25
Elysia1: B 865.12 888.90 907.98 862.76 879.89 859.91
         C 838.35 882.97 877.17 856.94 851.14 843.83
Elysia2: B 1146.81 1129.57 1112.75 1125.11 1123.71 1119.38
         C 1124.03 1072.98 1128.56 1089.52 1118.28 1108.31
```

The URL result has no direction flips. Elysia 1 also improves in every paired
round; Elysia 2 has one direction flip and is noisier. A 50,000-iteration URL
run scales to about 5.99 ms, so this recovers a material part of the gap but
does not yet approach PR #44's 2.48 ms orientation result.

Longer generic-fallback diagnostics found no material loss: a custom
`indexOf` method was 0.29% slower by median and Array `indexOf` was 1.3%
faster. Three short real-HTTP diagnostics were also non-regressed: Elysia 1
improved 1.12% and Elysia 2 improved 0.35%, with comparable RSS and CPU per
request. These are directional diagnostics, not retained throughput claims.
The candidate remains provisional until intrinsic hit/fallback counts and the
revision-matched PGO server matrix complete the performance gate.

Temporary diagnostic-only counters were then added, used for one process, and
removed before rebuilding. After 2,000 warmup fetches and one checked fetch,
Elysia 1 recorded 4,008 `indexOf` hits and 2,001 `substring` hits; Elysia 2
recorded 4,005 and 2,001. Both recorded zero fallbacks. The representative
route therefore has a 100% intrinsic hit rate; the small `indexOf` excess comes
from framework startup. The generic fallback remains covered separately by
the override/custom-method tests and benchmark.

## Phase 3: Compact the Hot Object Header

PR #44 moves `exotic_ops` and `exotic_keys` from every `ant_object_t` into the
optional sidecar. On the current Darwin ARM64 ABI the observed sizes are:

| Layout | `ant_object_t` | `ant_object_sidecar_t` |
| --- | ---: | ---: |
| Current | 152 bytes | 56 bytes |
| PR-style field placement | 136 bytes | 72 bytes |

This saves 16 bytes for every ordinary object and improves fixed-arena/cache
density, but an exotic object that did not otherwise need a sidecar gains an
allocation and an extra indirection.

- [x] Count total, sidecar, exotic-with-sidecar, and exotic-without-sidecar
      populations in Elysia 1, Elysia 2, Hono, bench-v8, and the spec suite.
- [x] Model net live/allocated bytes including allocator overhead.
- [ ] Port only the two fields and current accessors if the population supports
      it; do not copy the old object layout wholesale.
- [ ] Update initialization, GC visitation, finalization, OOM paths, and exotic
      reads/writes atomically.
- [ ] Validate Proxy, typed array, module namespace, host object, and other
      exotic behavior under GC stress.

Keep the change only if real workloads improve or RSS falls materially without
throughput loss. The struct-size reduction alone is not proof of a win.

### Phase 3 result: rejected

Temporary `Ant.stats()` fields counted live populations, then were removed
before restoring the retained runtime. The raw payload model subtracts 16
bytes per object, adds 16 bytes per existing sidecar, and adds a new 72-byte
sidecar for each exotic object that did not already have one. Allocator
rounding can only make the small new-sidecar term worse.

| Workload | Objects | Existing sidecars | Exotic without sidecar | Modeled live saving |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 | 10,632 | 3 | 2 | 169,920 bytes |
| Elysia 2 | 14,696 | 7 | 2 | 234,880 bytes |
| Hono | 20,538 | 2,472 | 1 | 288,984 bytes |
| Full spec suite | 13,040 | 34 | 206 | 193,264 bytes |
| bench-v8 Splay sample | 1,144,826 | 3 | 1 | 18,317,096 bytes |

The candidate `/tmp/ant-elysia-object-nopgo` has SHA-256
`3e527f2fbc896fa86ca67f58abffae55f24fcaaf61dbc40f349e525c7e4ddc40`.
It built at 136 bytes per object and 72 bytes per sidecar and passed all 4,086
spec checks. Focused Proxy and typed-array tests passed, and a long GC churn
diagnostic ran cleanly for 170 seconds before the intentionally 50,000-frame
test was stopped. A failing `proxy_has_own.cjs` repro fails identically on the
pinned pre-candidate binary and is unrelated.

Six serial order-rotated no-PGO pairs nevertheless rejected the candidate:

| Workload | Base median | Candidate median | Candidate change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 `app.fetch`, ms | 837.99 | 833.72 | +0.51% throughput | 4/6 faster |
| Elysia 2 `app.fetch`, ms | 1066.15 | 1082.20 | -1.48% throughput | 1/6 faster |
| Splay score | 4369.0 | 4324.5 | -1.02% | 1/6 faster |

Even a same-size rebuild that retained only diagnostic/accessor residue lost
all six Elysia 2 pairs by 2.33%, confirming a sensitive full-LTO layout cliff.
All runtime C residue was therefore removed. The rebuilt executable is
byte-for-byte identical to `/tmp/ant-elysia-string-nopgo`, including SHA-256
`0924f08066b08cfb440e7fb403bd3714fd1a2a408974d8177b1a55076c9b2d9a`.
The benchmark controls remain, but the object layout and hot accessors do not
change.

## Phase 4: Response/Headers Native Allocation

PR #44 also routes small Response and Headers native payloads through a fixed
native-data arena. This is independent from lazy materialization and must be
measured separately.

- [x] Count libc allocation/free calls and bytes for `response_data_t`, native
      header data, header entries, and JS wrappers after Phases 1--3.
- [x] Prefer a module-specific typed pool or a documented general small-native
      allocator with alignment, maximum-size, containment, teardown, and OOM
      contracts.
- [x] Do not reserve a large arena or alter global GC thresholds based only on
      the PR's old constants.
- [x] Compare allocator time, fragmentation, RSS plateau, teardown, and
      multi-isolate ownership.
- [x] Run long response churn and repeated isolate create/destroy tests.

Keep the allocator only if the no-server, real server, and memory results agree.

### Phase 4 allocation census and candidate

Temporary single-character counters were added around every relevant create,
copy, inline-entry, heap-entry, wrapper, and destroy operation, sampled, and
removed before the candidate build. After 2,000 warmups and one checked fetch,
the ordinary string route recorded:

| Workload | Response data | Response wrappers | Header data | Inline entries | Heap entries | Header wrappers |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Elysia 1 | 2,002 | 2,002 | 2,004 | 2,003 | 0 | 0 |
| Elysia 2 | 2,002 | 2,002 | 2,003 | 2,002 | 0 | 0 |

The one clone used for the post-loop body check accounts for one native
Response copy. The steady request path therefore performs exactly two libc
payload allocations: one 144-byte `response_data_t` and one 128-byte native
Headers list. Its first content-type entry is stored inside that list, and
lazy Headers avoids a JS wrapper entirely.

The candidate uses one isolate-owned, lazily initialized fixed arena for these
bounded native payloads. Its measured 160-byte class preserves `max_align_t`;
compile-time assertions reject an oversized payload. The native reservation is
capped at 64 MiB rather than PR #44's unexplained 512 MiB, WASM is capped at 2
MiB, and initialization or capacity exhaustion falls back to zeroed libc
allocation. Free classifies arena versus fallback pointers by exact slot
containment. The arena is destroyed only after all object finalizers run.

The first 256-byte candidate, `/tmp/ant-elysia-native-nopgo` at SHA-256
`3b62b3c9aa6007c723dcb5baa1a91fd6a493847006e8d7443dacc2b37223478d`,
was not retainable. Six serial order-rotated pairs measured:

| Workload | Base median | 256-byte candidate median | Paired change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 `app.fetch`, ms | 819.33 | 808.07 | -1.49% | 6/6 faster |
| Elysia 2 `app.fetch`, ms | 1,042.05 | 1,034.32 | -1.26% | 6/6 faster |
| Hono `app.fetch`, ms | 984.22 | 999.56 | +1.71% | 1/6 faster |
| Raw cached Response clone, ms | 662.57 | 768.98 | +16.21% | 0/6 faster |
| Raw new Response, ms | 730.13 | 683.22 | -6.19% | 6/6 faster |
| Raw new Response with header, ms | 1,562.39 | 1,505.79 | -3.87% | 6/6 faster |

The two actual payloads total 272 bytes, but the shared 256-byte class zeroed
512 bytes per pair and made cloning substantially worse. The refined 160-byte
candidate reduces that work and live-slot footprint by 37.5%. It also places
the cold lazy arena at the end of `ant_isolate_t`; putting it beside the three
existing GC arenas shifted every later hot isolate field by 64 bytes. The
tail placement preserves every pre-existing field offset.

The refined pinned binary `/tmp/ant-elysia-native160-tail-nopgo` has SHA-256
`f2e13e067aa018dc52f4a85545d6fbd1226483ef8d10dc5e3a3e8c50752e23c8`.
It uses the same build timestamp as the baseline and passes the focused 66
Response, 60 Headers, 15 Fetch, and 148 string checks, all 4,086 spec checks,
millions of Response/Headers churn operations, and a standalone test with 64
sequential isolate create/bootstrap/churn/destroy cycles.

Six serial, order-rotated no-PGO pairs against the pinned string-intrinsic
binary measured:

| Workload | Base median | 160-byte candidate median | Paired change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 `app.fetch`, ms | 817.94 | 798.60 | -1.85% | 6/6 faster |
| Elysia 2 `app.fetch`, ms | 1,038.10 | 1,024.59 | -1.39% | 6/6 faster |
| Hono `app.fetch`, ms | 986.92 | 976.04 | -0.37% | 5/6 faster |
| Raw cached Response clone, ms | 660.53 | 569.38 | -13.91% | 6/6 faster |
| Raw new Response, ms | 725.33 | 656.59 | -9.76% | 6/6 faster |
| Raw new Response with header, ms | 1,564.22 | 1,461.66 | -6.25% | 6/6 faster |

The paired changes use the median of each candidate/base sample ratio. The
refinement therefore cleared the focused no-server gate, including the Hono
and clone regressions that rejected the first candidate.

It did not clear the real-server or memory gates. Three additional serial,
order-rotated five-second server pairs, each after 100,000 warmup requests,
measured 100% successful responses with the expected body:

| Workload | Base RPS median | Candidate RPS median | Paired change | Warm RSS median base/candidate |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 | 111,086.57 | 108,992.64 | -1.09% | 40,912 / 42,224 KiB |
| Elysia 2 | 107,250.82 | 106,245.71 | -0.94% | 44,448 / 45,392 KiB |
| Hono | 91,224.30 | 88,739.77 | -2.98% | 40,016 / 40,400 KiB |

The 160-byte arena was therefore rejected and removed. It is useful evidence
that libc allocation dominates isolated Response construction, but eliminating
those calls does not improve the complete HTTP path on this host and increases
resident memory. Phase 5 must profile real HTTP before selecting another
allocation or construction optimization.

## Phase 5: Profile the Current Construction and Call Paths

After the known Response and string costs are removed, re-profile instead of
reviving old bytecode encodings.

- [x] Attribute `new Response`, Elysia context construction, handler calls,
      object literals, Promise/await completion, and property operations.
- [ ] Record generic/native/direct/intrinsic call counts and construction
      prototype lookup costs.
- [ ] If constructor prototype lookup remains material, add a current
      shape/epoch-guarded cache inside the centralized construction metadata.
- [ ] Consider native-constructor receiver elision only with an explicit
      constructor contract and subclass/new-target tests.
- [ ] Retain current precomputed object-literal `obj_sites`; do not restore
      per-`DEFINE_FIELD` IC operands or JIT vstack site propagation.
- [ ] Do not add a named-import IC to an RPS branch unless profiles unexpectedly
      show module lookup in the timed request path.

### Phase 5 real-server profile and inspector metadata gate

A five-second, 1 ms `sample` capture of Elysia 2 after 100,000 HTTP warmup
requests produced 3,604 main-thread samples. The complete request path, rather
than `new Response` alone, was the next actionable boundary: 1,320 samples were
under `server_process_client_request`, 1,118 under
`server_finish_with_response`, and 1,102 reached the socket write. The profile
also exposed eager inspector-only work on every uninspected request:

- parsed request headers were cloned into a second native list;
- the request URL was reconstructed for `Network.requestWillBeSent`;
- response headers, content length, and connection metadata were allocated and
  formatted into another native list;
- the response URL was reconstructed again;
- all metadata was then freed after the inspector API returned immediately.

The retained change adds `ant_inspector_network_active()` using the same
started, attached, Network-enabled, writable-client predicate as the event
producer. Server request and response metadata are prepared only when that
predicate is true. A request that begins without Network capture keeps request
ID zero for its lifetime; attached sessions still receive request and response
events with their original headers. The dedicated
`tests/test_inspector_network_server.cjs` regression test connects over CDP,
enables Network, performs a live server request, and verifies both event header
sets.

The exact current-tree no-PGO base
`/tmp/ant-elysia-inspector-base-nopgo` has SHA-256
`a717c88fc3e7c274c216999e8be3eb9f3ad62eabc679ee0c31f3d7ebd50caf73`;
the candidate `/tmp/ant-elysia-inspector-gate-nopgo` has SHA-256
`885ecb8257d08db8a0a25f1f1bf3ca9dcee162be3c72f559e8e3266abb65224b`.
Three serial, order-rotated five-second server pairs measured:

| Workload | Base RPS median | Candidate RPS median | Paired change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 | 111,407.96 | 117,482.59 | +5.54% | 3/3 faster |
| Elysia 2 | 108,101.91 | 113,965.63 | +5.68% | 3/3 faster |
| Hono | 91,109.22 | 95,223.13 | +4.93% | 3/3 faster |

All HTTP runs had 100% success and the expected body. Warm and peak RSS were
within 0.5 MiB and 0.7 MiB respectively across the medians. Six exact-source
no-server pairs were neutral-to-positive: median time changed -0.44% for
Elysia 1, approximately 0.00% for Elysia 2, and +0.21% for Hono. The gate is
retained pending the broader bench-v8 and matched-PGO gates.

### Origin-form URL, shared Request defaults, and HTTP formatting

The inspector-gated profile still attributed 171 samples to building and
parsing each incoming `Request.url`. The server already has the request target
and `Host`, but the generic constructor parsed a synthetic base URL and then
resolved the target against it. The retained origin-form path recognizes only
a non-empty host plus a target beginning with one slash, excluding `//`, and
parses `http://` + host + target once. Absolute-form, authority-like,
asterisk-form, and missing-host inputs keep the generic resolver. The focused
server regression compares normalized path, percent escape, `//` target, and
IPv6-host results with `new URL`.

The candidate `/tmp/ant-elysia-origin-url-nopgo` has SHA-256
`6d695020f5738442e612d15a5e1768321aab54b0494b83fdbe3cbba0590fb59`.
Three incremental server pairs against the inspector gate measured:

| Workload | Parent RPS median | Candidate RPS median | Paired change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 | 120,711.96 | 122,652.24 | +1.61% | 3/3 faster |
| Elysia 2 | 114,626.81 | 119,097.54 | +3.93% | 3/3 faster |
| Hono | 99,221.31 | 98,717.98 | -0.51% | 1/3 faster |

Hono's three paired changes were -0.51%, -4.54%, and +2.60%, so the short
sample is noisy rather than evidence of a stable Hono regression. This path is
provisionally retained for the matched longer matrix because both Elysia
versions improved in every pair.

The same constructor allocated and copied six constant Request defaults per
incoming request. The retained server-only representation points those fields
at shared immutable literals while keeping `method` owned. Generic Requests,
clones, and any initialized override retain fully owned copies; the destructor
uses an explicit representation flag rather than pointer identity. The
combined binary `/tmp/ant-elysia-request-defaults-nopgo` has SHA-256
`89a2907e58026abda63fe364830796d20350d4962244f7ae34dca169b239ec746`.
Three incremental pairs against the URL candidate measured:

| Workload | Parent RPS median | Candidate RPS median | Paired change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 | 122,414.69 | 124,128.05 | +1.33% | 3/3 faster |
| Elysia 2 | 118,020.60 | 120,397.49 | +1.66% | 3/3 faster |
| Hono | 99,079.00 | 99,363.53 | +0.62% | 2/3 faster |

The focused test also verifies every shared default and a cloned Request with
overrides. The request URL/default test, invalid-target test, and the 210-check
Request URL spec selection pass.

The post-gate profile also showed repeated `vsnprintf` stacks for fixed HTTP
syntax. The retained writer appends status, content length, and header
`name: value` bytes directly, with bounded decimal conversion, while leaving
the generic formatted writer available for non-hot paths. The combined binary
`/tmp/ant-elysia-http-format-nopgo` has SHA-256
`e7fcebbb24b60fbafcbdd91a332278be315ef88400de21bb004cf28fafa53807`.
Three incremental pairs against the Request-default candidate measured:

| Workload | Parent RPS median | Candidate RPS median | Paired change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 | 123,053.86 | 125,138.05 | +1.61% | 3/3 faster |
| Elysia 2 | 118,859.17 | 121,161.75 | +1.87% | 3/3 faster |
| Hono | 99,576.10 | 101,798.65 | +1.58% | 3/3 faster |

All runs had success rate 1.0 and the expected body and content type.

The profile still attributed 124 samples to Ada after the one-parse change.
For the exact incoming root target, the retained parser now classifies whether
`Host` is already a canonical lowercase DNS name or canonical dotted quad with
an optional canonical non-default port. `request_create_server` constructs the
known `http://authority/` URL state directly only when that parser bit is set
and the target is exactly `/`. IPv6, uppercase/IDN hosts, numeric-host variants,
port 80, queries, path normalization, absolute-form, and every other target
still use Ada. Computing the classification in the HTTP parser keeps the larger
validator out of the Request constructor translation unit; an earlier version
placed it there and caused a 1.20% no-server layout regression.

The retained binary `/tmp/ant-elysia-root-url-parser-nopgo` has SHA-256
`92986ed6e22edd2b5132319ed74585350a2d8c41ae6195b20dcd97e3ed21983a`.
Three incremental pairs against the HTTP-format parent measured:

| Workload | Parent RPS median | Candidate RPS median | Paired change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 | 126,830.00 | 127,876.55 | +0.44% | 3/3 faster |
| Elysia 2 | 122,645.46 | 123,577.48 | +0.70% | 3/3 faster |
| Hono | 100,387.27 | 101,974.48 | +0.98% | 3/3 faster |

All runs had success rate 1.0. The two-million-iteration Elysia 1 no-server
control had a -0.01% paired median time change across six order-rotated pairs.
The raw-request regression includes canonical DNS and IPv4 root URLs plus the
normalization-sensitive fallback cases.

A fresh Elysia 2 server sample after the root-URL change contained 3,597 stack
samples. The root URL no longer reached Ada. Of the 1,345 samples under
`server_process_client_request`, 1,278 continued through
`server_finish_with_response`, 1,272 reached `server_queue_write`, 1,259
reached `ant_conn_write`, and 1,231 ended in the kernel `write` syscall. This
confirms that one socket write per response is now the native ceiling, while
the remaining removable work is request/header allocation and dispatch around
that syscall. The earlier `uv_try_write` experiment below already establishes
that merely moving the same write inline is not a win.

The parser separately duplicated `Host` and `Content-Type` strings even though
their owned header entries outlived every use. A first ownership-only slice
made these fields borrowed aliases into the parsed header list. The binary
`/tmp/ant-elysia-parser-borrow-nopgo` has SHA-256
`61b7b72d5e9b949762bafc4e851b88ee1b4ff5a9c4dc82a0eb6dccc382d6c5ca`.
Three incremental five-second pairs against the root-URL parent measured:

| Workload | Parent RPS median | Candidate RPS median | Paired change | Pair direction |
| --- | ---: | ---: | ---: | ---: |
| Elysia 1 | 127,145.95 | 127,888.47 | +0.58% | 2/3 faster |
| Elysia 2 | 122,585.17 | 124,188.68 | +1.18% | 3/3 faster |
| Hono | 99,755.73 | 99,628.33 | +0.23% | 2/3 faster |

Every run had success rate 1.0. The raw paired changes were +0.584%, -0.092%,
and +0.827% for Elysia 1; +0.169%, +1.971%, and +1.184% for Elysia 2; and
-2.074%, +1.795%, and +0.228% for Hono. The slice is retained as the lifetime
prerequisite for direct header transfer rather than as a large standalone
claim.

The parser then exposed a larger allocation issue: each header field and value
detached separate 256-byte scratch buffers, after which the server copied both
again into native `Headers`. The current candidate stores each parsed header as
one node with trailing name/value storage, reuses the two token scratch buffers
for the life of a keep-alive connection, and transfers the combined nodes into
the Request's native header list after validation, ASCII case folding, and OWS
trimming. `ant_http_header_t` remains 24 bytes because the shared storage is a
flexible array, while non-parser producers retain their old separately owned
string representation. The candidate is pinned at
`/tmp/ant-elysia-header-transfer-nopgo` with SHA-256
`21db5783813fd2de0ad5654109bf4a031ce14969391cafb01312b6a013224b76`.

The new raw-request regression covers mixed-case names, leading/trailing OWS,
duplicates, and an empty value. That empty case also exposed and fixed an old
boundary bug: the parser had inferred a completed header from `value.len > 0`,
so an empty header merged with the following field. Header emission now uses
llhttp's explicit `on_header_value_complete` callback. WebSocket validation
now reads the Request's native header view instead of the inspector-only raw
copy, preserving the inspector allocation gate without breaking upgrades.
Focused URL, malformed-target, Latin-1 wire, Node HTTP body, inspector Network,
and WebSocket upgrade/deflate tests pass. Server A/B remains pending after the
required build cooldown.

While an unrelated `mediaanalysisd` process kept a host core saturated and
made server timing invalid, the next request-path slices were built and pinned
without making performance claims:

- `/tmp/ant-elysia-shared-method-nopgo`, SHA-256
  `3c0073913e4cc71365140be9d0a2262ed767cd44d2084af7e96bc97a5d2d8e06`,
  removes the parser's duplicate method string and lets a server Request borrow
  llhttp's static method name under its existing shared-default lifetime.
- `/tmp/ant-elysia-root-target-nopgo`, SHA-256
  `e95eac3492bdb3c062a6315225340f01d579772eb217accda2976c3342533120`,
  represents an exact `/` target with a static literal. The parser materializes
  the prefix if a fragmented later callback proves that the target is longer.
- `/tmp/ant-elysia-request-url-getter-nopgo`, SHA-256
  `a65e281fb269dc7da2e658c53b8c475a219d8af98ffe2c3bdfa907d9da41b45a`,
  lets the Request URL getter return the already-built href without a temporary
  serializer allocation.
- `/tmp/ant-elysia-root-url-state-nopgo`, SHA-256
  `705e66e26047f0695fef88ce95cc78099f158553a39a1749d1de7e8e70542ff4`,
  avoids separately allocating `http:` when the canonical-root state already
  owns its complete href.
- `/tmp/ant-elysia-header-lookup-nopgo`, SHA-256
  `e0a37280bbd2d7e8b949e33a7a40a51540df299a533301d5a9edd7522e80a117`,
  avoids allocating a lowercase copy for already-lowercase Headers lookup
  names and returns a single matching value directly. Mixed-case and duplicate
  paths retain normalization and joining.

The raw-request regression splits both `/` and `/split` across socket writes
and covers GET and POST method lifetimes. The 60-check Headers suite, Request
URL, Latin-1 wire, WebSocket, and inspector tests pass on the final request
candidate. Each slice still requires an incremental parent/candidate server
comparison before it can be retained.

The same cooldown window exposed a separate Response construction issue. The
VM already allocates and prototypes a construction receiver, but the native
Response constructor discarded it and allocated a second JS object. Reusing
the prepared receiver removes that allocation and also fixes explicit
Proxy-new-target semantics: Node reads the Proxy's `prototype` once and uses
it, whereas the old Ant path read it for the discarded receiver and returned a
Response with the fallback prototype. The candidate is pinned at
`/tmp/ant-elysia-response-receiver-nopgo` with SHA-256
`818c1b34aa524e2a3a573d62bbf038429324d70b534ddbec204bd54208cc39cd`.
The focused Response constructor test passes in Ant and Node, all 66 Response
spec checks pass, all 434 bind/construct checks pass, and the server Request
regression passes. This candidate also remains unmeasured while the host is
externally loaded.

A second Response candidate targets the two absent-property walks visible in
the fixed-work sample. For an ordinary one-own-data-property `{headers}` init
with the real `Object.prototype`, it loads the data property directly, performs
the complete Headers conversion, and only then revalidates the init shape,
prototype, and absence of `status`/`statusText` before skipping their generic
lookups. The post-conversion proof is required because HeadersInit enumeration
and value conversion can execute JS. A Node-matched regression mutates
`Object.prototype.status` from a nested header getter and verifies that the
later field is still observed. The pinned binary
`/tmp/ant-elysia-response-init-proof-nopgo` has SHA-256
`ae43e569daf6399ea2d784eb89bf7526c38970b76936d6537742d90467a31199`.
All 67 Response, 60 Headers, and 434 bind/construct checks pass. A diagnostic
sample confirms that the two prototype walks disappeared and header conversion
became the dominant native constructor work, but its elapsed time is discarded
because `mediaanalysisd` remained active. Incremental server and no-server A/B
remain mandatory.

Two Headers-record slices remove work below that constructor proof while
preserving the generic observable paths. The first reuses the property
iterator's already-loaded value only for an ordinary non-accessor property
whose value is already a string; accessors, Proxies, coercions, symbols, and
non-string values still perform `Get`. It is pinned at
`/tmp/ant-elysia-header-record-direct-nopgo`, SHA-256
`46b550b24d13ed660e1e0c760d0df0f5b08383c58527ecad7bb155a378f6b8df`.
The second recognizes an ordinary record with exactly one such string
property and appends it without constructing the generic property iterator. It
is pinned at `/tmp/ant-elysia-single-header-record-nopgo`, SHA-256
`42eb9ed960a75cf4d4c4e74661ac3e74bea80b66a89800367d4f0f736b56a1fe`.
The 60 Headers and 67 Response checks plus the raw ByteString wire test pass on
both stages. A diagnostic sample confirms removal of the redundant property
lookup, but timing remains deferred until the external CPU load and cooldown
clear.

The remaining constructor sample showed repeated generic lookup of the
ordinary new target's own `prototype` data property. A general construct-meta
candidate now reads that property by its existing intern identity when the
source is an ordinary object and the own descriptor is data-only. Accessors,
Proxies, and absence use the unchanged semantic lookup; non-object prototype
values retain the `Object.prototype` fallback. The candidate is pinned at
`/tmp/ant-elysia-construct-prototype-own-nopgo`, SHA-256
`3dab4ce3f72aa8540ae41b3f726c29c2eb29cea2500e412b9048a9245a740779`.
All 434 bind/construct checks, the explicit-new-target wrapper regression, 25
Reflect checks, and 67 Response checks pass. It remains a separate unmeasured
slice pending the quiet-host ladder.

The final attribution-only compact-Response sample confirms that the generic
constructor-prototype lookup is gone. Remaining constructor work is now
concentrated in required header-name/value validation and copying, two
`intern_find` calls used by the post-HeadersInit absence proof, the subsequent
`content-type` presence scan, native payload allocation, and GC of the created
Response. Possible follow-ups are an in-layout `has_content_type` bit and a
single fused ASCII/ByteString validation pass, but neither should be layered
until the existing pinned ladder establishes which source slices survive real
HTTP.

### Pending incremental no-PGO server ladder

The following immutable binaries form the next source-attribution ladder. Each
row must be compared only with its direct parent, serially and in rotated
AB/BA order. Diagnostic samples captured while `mediaanalysisd` was consuming
CPU are discarded.

| Slice | Binary | SHA-256 |
| --- | --- | --- |
| A: borrowed Host/Content-Type aliases | `/tmp/ant-elysia-parser-borrow-nopgo` | `61b7b72d5e9b949762bafc4e851b88ee1b4ff5a9c4dc82a0eb6dccc382d6c5ca` |
| B: transfer parsed header ownership | `/tmp/ant-elysia-header-transfer-nopgo` | `21db5783813fd2de0ad5654109bf4a031ce14969391cafb01312b6a013224b76` |
| C: shared static llhttp method | `/tmp/ant-elysia-shared-method-nopgo` | `3c0073913e4cc71365140be9d0a2262ed767cd44d2084af7e96bc97a5d2d8e06` |
| D: static exact-root request target | `/tmp/ant-elysia-root-target-nopgo` | `e95eac3492bdb3c062a6315225340f01d579772eb217accda2976c3342533120` |
| E: reuse the parsed Request URL href | `/tmp/ant-elysia-request-url-getter-nopgo` | `a65e281fb269dc7da2e658c53b8c475a219d8af98ffe2c3bdfa907d9da41b45a` |
| F: avoid root URL protocol allocation | `/tmp/ant-elysia-root-url-state-nopgo` | `705e66e26047f0695fef88ce95cc78099f158553a39a1749d1de7e8e70542ff4` |
| G: lowercase header lookup and one-value path | `/tmp/ant-elysia-header-lookup-nopgo` | `e0a37280bbd2d7e8b949e33a7a40a51540df299a533301d5a9edd7522e80a117` |
| H: reuse the VM construction receiver | `/tmp/ant-elysia-response-receiver-nopgo` | `818c1b34aa524e2a3a573d62bbf038429324d70b534ddbec204bd54208cc39cd` |
| I: post-conversion ResponseInit proof | `/tmp/ant-elysia-response-init-proof-nopgo` | `ae43e569daf6399ea2d784eb89bf7526c38970b76936d6537742d90467a31199` |
| J: reuse already-loaded Headers record values | `/tmp/ant-elysia-header-record-direct-nopgo` | `46b550b24d13ed660e1e0c760d0df0f5b08383c58527ecad7bb155a378f6b8df` |
| K: direct ordinary one-property header record | `/tmp/ant-elysia-single-header-record-nopgo` | `42eb9ed960a75cf4d4c4e74661ac3e74bea80b66a89800367d4f0f736b56a1fe` |
| L: ordinary own-data constructor prototype | `/tmp/ant-elysia-construct-prototype-own-nopgo` | `3dab4ce3f72aa8540ae41b3f726c29c2eb29cea2500e412b9048a9245a740779` |

The serial runner was functionally dry-run with identical binaries and only
ten warmup requests. Readiness/body checks, `oha` JSON validation, RSS
sampling, teardown, and port-release checks all passed. Its one-second numbers
are explicitly not performance evidence. Real iteration begins with three
five-second Elysia 2 pairs per adjacent slice after the host becomes quiet and
stays cool for five minutes. Surviving slices then advance to Elysia 1 and
Hono, followed by the five-pair 15-second retention gate.

An attempted follow-up removed the heap-allocated `server_write_req_t` by
storing its pending action in the retained `server_request_t`. The refcount and
stream-order contracts remained correct and focused tests passed, but the
binary `/tmp/ant-elysia-write-action-nopgo` at SHA-256
`632e61116d8fcf70737b661d41f5bb80f54922e0532b072d0344174a09c0b690`
lost the median pair by 1.06% on Elysia 1, 0.93% on Elysia 2, and 0.30% on
Hono; it won only one of nine pairs. The slice was removed. Fewer allocations
is not sufficient when the full-LTO layout and callback path get worse.

A second write experiment used `uv_try_write` and completed a fully accepted
small server response inline, avoiding the connection write-request allocation
and deferred libuv callback while preserving the queued partial/EAGAIN path.
The candidate `/tmp/ant-elysia-inline-write-nopgo` has SHA-256
`47180c1adf4bc4bd2298c80a20df38fa1e7ee06af0a3d43951c2e8d6b81365dc`.
It measured +0.21% on Elysia 1, -0.79% on Elysia 2, and -0.33% on Hono across
three pairs. Moving lifecycle completion onto the request call stack did not
improve the full server and the path was removed.

The remaining PR #44 Response-init guard was also tested with a stricter
current-tree design. The old PR implementation is not semantically reusable:
it consumes a nested headers record after reading `status` and `statusText`,
changing observable getter order. The candidate recognized only an ordinary
one-property `{headers: value}` object with the real `Object.prototype`, proved
that inherited `status` and `statusText` were absent, consumed the headers in
the existing order, and otherwise used the unchanged generic path. The three
intern identities were placed at the tail of the isolate so existing VM/GC
field offsets did not move.

The focused 200,000-construction benchmark improved a header-record Response
from 61.81 ms to 43.32 ms and a `Headers`-initialized Response from 51.08 ms to
33.95 ms. The corrected regression test passes in both Ant and Node; its old
assertions incorrectly expected explicit `content-type` values to be rewritten.
Real HTTP rejected the optimization. The final tail-layout binary
`/tmp/ant-elysia-response-init-tail-nopgo` has SHA-256
`5165a0dbff612eb40b3211ed98a64cd7a2440fd4fc6e8c5e72b388062822635f`.
Five 15-second Elysia 2 pairs were server-neutral at a -0.03% paired median,
but three fresh pairs lost 0.43% on Elysia 1 and 1.44% on Hono. The fast path
and isolate fields were removed; the test correction remains.

- [ ] If computed string property access remains hot, design a GC-safe stable
      intern-identity IC from scratch. Never store an untraced GC string value
      in code metadata.

## Phase 6: Close the Native Server Gap

Reaching historical PR #44 parity is not the 200k finish line. Once no-server
execution is near the reference, profile real HTTP to find work absent from the
fixed-call harness.

- [ ] Split request parsing, framework dispatch, Response construction, header
      serialization, body write, libuv scheduling, allocation, GC, and teardown.
- [ ] Compare no-server calls per second with actual RPS to quantify the native
      server ceiling.
- [ ] Serialize pending/native Headers without constructing JS wrappers when
      semantics permit.
- [ ] Preserve borrowed immutable response-body storage through the writer;
      measure copies and write-buffer lifetime explicitly.
- [ ] Measure HTTP writer batching, header normalization/copying, keep-alive
      state, libuv writes, and callback traffic.
- [ ] Remove only counter-proven allocations and helper transitions.
- [ ] Reassess GC only after allocation sources are known. Coordinate any array
      backing-store policy with `array-backing-store-gc-pacing.md`; do not port
      PR #44's old floor/timer changes.

Every server optimization must preserve malformed-request handling,
backpressure, disconnects, keep-alive, streaming bodies, header bytes, and
write-buffer lifetime.

## Phase 7: PGO and 200k Confirmation

- [ ] Freeze source and regenerate the full PGO training profile.
- [ ] Build self-profiled base and final candidate with the same compiler,
      linker, LTO, and training corpus.
- [ ] Check for discarded/out-of-date counters and regenerate if any hot
      runtime/JIT/server function is structurally mismatched.
- [ ] Run the full no-PGO and self-profiled PGO AB/BA matrices.
- [ ] Run seven paired 15-second server rounds for Elysia 1, Elysia 2, and Hono.
- [ ] Run one 60-second confirmation per Elysia version with RSS sampling.
- [ ] Run bench-v8, focused runtime tests, the full spec suite, `maid preflight`,
      and all additional validation selected by the repository router.
- [ ] Record raw results, hashes, medians, dispersion, RSS, correctness, and the
      final decision in this plan.

## Retention and Rollback Rules

- Reject a candidate that wins only one short microbenchmark but is neutral or
  negative in both Elysia servers.
- Reject a candidate whose direction does not survive no-PGO and self-profiled
  PGO comparison unless the cause is understood and documented.
- Reject any semantic shortcut that depends on unobservable framework behavior;
  fallbacks must remain correct when user code makes the behavior observable.
- Reject ignored allocation failures, ambiguous ownership, untraced GC values,
  or server use of an error value as Headers data regardless of benchmark gain.
- Investigate any repeatable Hono, bench-v8, startup, or RSS regression before
  retaining the change. Do not average a material per-workload loss away.
- If a phase cannot clear its gate, revert that candidate and proceed from the
  last pinned retained binary.

## Task List

- [ ] Phase 0: freeze fixtures, counters, and paired baselines.
- [ ] Phase 1: land lazy, fallible Response Headers with safe server handling.
- [ ] Phase 2: land a measured current string-call intrinsic and ASCII cache.
- [x] Phase 3: reject hot-object compaction from population and A/B evidence.
- [ ] Phase 4: decide native-data pooling from allocation evidence.
- [ ] Phase 5: optimize only the remaining profiled construction/call costs.
- [ ] Phase 6: remove the measured native HTTP gap to 200k.
- [ ] Phase 7: regenerate PGO and complete final correctness/performance gates.

## Decision Log

- 2026-08-30: Set 200k RPS as the target for both Elysia 1 and Elysia 2, with
  Hono, bench-v8, RSS, and correctness as non-regression gates.
- 2026-08-30: Treat `/private/tmp/ant` as a read-only PR #44 reference and port
  ideas individually rather than merging or replaying the stale branch.
- 2026-08-30: Promote lazy/fallible Response Headers to the first candidate
  because the new stage harness shows the largest isolated gap. The old server
  hunk remains rejected because it fails to check the now-fallible Headers
  result.
- 2026-08-30: Preserve the `indexOf`/`substring` concept but reject its fixed
  duplicated opcode family. Pair the redesigned intrinsic with a narrow lazy
  ASCII-character cache.
- 2026-08-30: Keep current dense-element specialization and object-literal site
  analysis; reject the old `GET_ELEM` and `DEFINE_FIELD` encodings.
- 2026-08-30: Defer constructor caching, native pooling, object compaction, and
  server/GC work until the earlier isolated phases expose their remaining cost.
- 2026-08-30: Reject PR #44's simple Response-init shape shortcut for now. A
  regression test showed that inherited `Object.prototype.status` can become
  observable without the proposed cached absence proof being invalidated.
  Keep the normal semantic property path until Ant has a general, tested
  prototype-absence guard contract.
- 2026-08-30: Do not add the three interned ResponseInit keys independently.
  Their only proposed consumer was the rejected simple-init shortcut, so the
  extra isolate state would have no current performance value.
- 2026-08-30: Retain the operation-driven string intrinsic provisionally. Its
  no-PGO URL loop improves 10.03% in throughput and both Elysia no-server
  fixtures improve, while generic fallbacks and short server diagnostics do
  not materially regress. Fresh PGO server evidence remains mandatory because
  the recovered server gain is still small and the historical URL gap remains.
- 2026-08-30: Reject object-header compaction despite the modeled live-memory
  saving. Elysia 2 and Splay each lose five of six no-PGO pairs, and exact
  runtime-source restoration reproduces the retained binary byte-for-byte.
- 2026-08-31: Replace the earlier rejected ResponseInit shortcut with a
  post-conversion proof. Rechecking the exact init shape and prototype absence
  after HeadersInit conversion preserves mutations performed by user code
  without adding a global prototype-epoch mechanism. Keep it provisional until
  its direct-parent server comparison clears the performance gate.

## Validation Status

- The latest accumulated no-PGO candidate passes all 4,087 checks in the full
  102-file spec suite. This run is correctness evidence only; the externally
  loaded host makes its timing output unusable for performance comparison.
- The PR #44 archive and current binary identities were recorded.
- Both configured builds were confirmed as release `-O3`, full-LTO builds using
  their own explicit PGO profiles.
- The user-provided no-server and stage results were recorded as orientation
  evidence.
- Source inspection connects the URL gap to string-call/cached-character work
  and the compact-response gap to eager Headers/native Response construction.
- A pinned no-PGO base for commit `aebd47e4` is stored at
  `/tmp/ant-elysia-base-aebd47e4-nopgo` with SHA-256
  `7c03bae9966fbf10184f50c09abedd0a2c4f3b6c34361ff4c2bc34b32ea7d555`.
- The lazy-Response no-PGO candidate is stored at
  `/tmp/ant-elysia-response-nopgo` with SHA-256
  `fc5d55e5c5e72556b1877e13252911aa179ac059c2170a82fc57a766ced0be34`.
- The candidate source/include diff has SHA-256
  `e795f2cae1fcbf2061eaf9daad3a4588e20aa67847b4ca930390272f9cce5720`.
  Base and candidate use Clang 21.1.8, release optimization level 3, full LTO,
  the system allocator, and no PGO.
- Focused validation passes 66 Response, 60 Headers, and 15 Fetch checks. The
  Response suite also passes under Node, including inherited init properties,
  Proxy reads, subclass construction, clone independence, explicit
  `content-type`, stable header identity, incompatible receiver handling, and
  single evaluation of `Response.json`'s HeadersInit. Headers record accessors
  are now observed at the same point as Node before later ResponseInit fields.
- The durable Elysia fixtures are pinned to 1.4.28 and 2.0.0-exp.38. Their
  lockfile SHA-256 values are `45b8a657d3257c19cbbd388398181964a6320119e0361b3eed2ba5f2314d2945`
  and `39e223718b0282cdf4cb7e6e88cb1906c8124469c7d7fdcfd976255193b2fae2`.
- The string intrinsic correctness suite passes 148 checks under Ant. Shared
  semantics were also checked under Node through the Ant-specific tail of the
  file. Bytecode inspection shows exact string calls use only the single
  `CALL_STRING_INTRINSIC(kind, argc)` form, and a warmed JIT test exercises
  coercion that recursively grows the VM stack before returning.

## Follow-ups

- Move this plan to `completed/` only after the final 200k confirmation and all
  non-regression gates pass.
- Keep reusable benchmark protocol improvements in repository testing guidance
  after this performance campaign finishes.

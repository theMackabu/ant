# JS Path Node Performance

Status: active
Last reviewed: 2026-09-05
Owner: theMackabu

## Goal and constraints

Priority update (2026-09-05): the user deferred the remaining performance gap
to [technical debt](../tech-debt.md). Node-compatible correctness takes priority;
do not continue performance experiments merely to close these microbenchmarks.
The original target and measured history below remain as context, not a claim
of completion.

First match or beat the installed Ant (`$(which ant)`) on all eight existing
path microbenchmarks, retaining the JS path implementation, primordial
protection, and Node semantics. The saved pre-optimization JS binary is not
the baseline for this gate. Node performance is the subsequent target. Optimize
measured runtime bottlenecks, add regressions, build and test, and compare
pinned binaries with serial ABBA runs. No commits or pushes. Completion requires
all eight installed-Ant targets and compatibility validation, not an average
speedup. This priority was explicitly updated by the user after the three-way
comparison.

## Updated installed-Ant checkpoint

`/tmp/ant-path-goal.KC4jj3/three-way-before-slice-results.json` records all binary hashes,
fixture hash, and twelve samples per binary from serial pairwise ABBA runs.
Measured milliseconds per 100k calls (installed/current/Node):

| Workload | Installed Ant | Current Ant | Node |
| --- | ---: | ---: | ---: |
| POSIX normalize | 9.38 | 46.35 | 8.38 |
| Windows normalize | 10.51 | 86.28 | 15.23 |
| POSIX join | 15.01 | 84.94 | 18.40 |
| POSIX cwd resolve | 962.36 | 208.42 | 32.59 |
| POSIX relative | 26.08 | 160.90 | 32.39 |
| Windows resolve | 965.86 | 175.06 | 21.45 |
| Windows relative | 28.09 | 214.43 | 37.57 |
| POSIX parse | 23.77 | 21.43 | 2.54 |

All fixture checksums match. Current wins three rows; the other five remain
open. Preserve the resolve and parse wins while improving normalization,
joining, and relative paths. The allocation-error checks have now been built;
constant-template tests and 6090 Node differential cases pass on that build.

Fresh normalization profile: `normalize-template-sample.txt` in the same
artifact directory. Largest named self-sample counts include snapshot append
395, do_string_op 304, memmove 258, js_type_alloc 251, substring creation 187,
generic JIT call 169, UTF-16 length 150, and String.slice 128. Unknown JIT
frames remain substantial. Next investigate temporary string construction
and slice dispatch, not a new charCodeAt hypothesis without evidence.

### ASCII slice experiment

The existing slice builtin now uses an ASCII path for primitive flat or
cached-flat strings with nonnegative numeric/undefined bounds. It skips
receiver coercion and UTF-16 translation, preserves known ASCII metadata,
and returns the immutable source value for a full-range slice. All other
cases retain the prior fallback. `test_string_slice_ascii.cjs` passes on
Ant and Node, including fractional/huge/infinite positive bounds, embedded
NUL, allocation churn, Unicode fallback, and captured-target protection.
All 6090 path differential cases pass.

`ascii-slice-results.json` pins the checked object-template build and the
slice candidate, using six samples each in serial ABBA. Median changes:
normalize 46.98 -> 45.39 ms (-3%), Windows normalize 86.66 -> 83.92 (-3%),
join 84.38 -> 82.79 (-2%), cwd resolve 208.30 -> 200.84 (-4%), relative
167.73 -> 156.36 (-7%), Windows resolve 180.13 -> 165.98 (-8%), Windows
relative 216.25 -> 206.71 (-4%), parse 22.24 -> 20.13 (-9%). Some ranges
overlap, especially for the smaller gains. These are per-change results,
not a new installed-baseline parity claim.
Preflight passes; the rebuilt candidate passes all 4136 spec tests across
102 files (`preflight-ascii-slice.log`, `spec-ascii-slice.log`).

### Empty template segments

Inspection of normalization's generated MIR and template compilation showed
that empty cooked literal segments still emitted additions. The compiler now
omits only those literal segments, keeping every substitution's immediate
ToString operation and left-to-right evaluation. Empty templates still emit
the empty string, and tagged templates retain their separate unchanged path.
The new focused test passes on Node and Ant, covering coercion/exception
order, Unicode, append snapshots, reentrancy, and tagged literal arrays.
The 6090 path differential cases also pass.

`template-empty-results.json` compares pinned slice and empty-segment builds
in serial ABBA. Normalize 43.25 -> 42.23 ms (-2%), Windows normalize
80.70 -> 79.21 (-2%), join 81.38 -> 77.56 (-5%), cwd resolve 193.06 ->
187.75 (-3%), relative 148.19 -> 144.89 (-2%), Windows resolve 158.41 ->
153.48 (-3%), Windows relative essentially unchanged. Parse's 2% median
shift is within overlapping ranges and is not attributed to this change.
Most small improvements have overlapping ranges; baseline parity remains
unmet. MIR also confirms that short nonempty concatenations fall back from
the JIT's rope-only allocation path to generic addition, matching the named
do_string_op samples. This is the next runtime path to investigate.
Preflight and all 4136 spec tests pass (`preflight-template-empty.log`,
`spec-template-empty.log`).

### JIT short flat concatenation

The existing string-add JIT fast path now handles nonempty ASCII flat-string
concatenations below the existing short-cons threshold. It uses the pool's
shared size-class lookup at compile time, reuses freed slots or initialized
bump allocation space, and accounts exact requested bytes in gc_pool_alloc.
The GC pressure floor is a conservative gate: at or above it, normal
allocation checks the adaptive threshold. No collection or call occurs on
the inline path. Uninitialized/exhausted buckets, Unicode/unknown metadata,
ropes, builders, and coercions use existing fallback paths. Focused size-class
boundary, GC-survivor, Unicode, coercion, snapshot and root tests pass; the
6090 path differential cases also pass. Compare artifact:
`short-flat-jit-results.json` (against `ant-template-empty`).

Serial ABBA medians: normalize 42.67 -> 39.82 ms (-7%), Windows normalize
79.54 -> 75.61 (-5%), join 77.91 -> 74.54 (-4%), cwd resolve 186.95 ->
171.48 (-8%), relative 145.94 -> 138.14 (-5%), Windows resolve 153.80 ->
133.72 (-13%), Windows relative 195.37 -> 184.88 (-5%). These seven rows
have non-overlapping ranges in this run. Parse is unchanged. Long-append
controls (`short-flat-jit-control-results.json`) are unchanged, ratios
0.988/1.009/1.002 for 32/10000/10000-with-reads. Preflight and all 4136 spec
tests pass (`preflight-short-flat-jit.log`, `spec-short-flat-jit.log`).

### Cached-rope length

Fresh 8-second profiles after short flat allocation:
`normalize-short-flat-sample.txt` and `relative-short-flat-sample.txt`.
Relative has 947 self samples in str_utf16_len out of 6592 total, 451 in
rope_flatten, and 199 in jit_helper_get_length. Normalization now has slice
and snapshot append as its hottest named functions. The JIT length emitter
did not use a rope's existing flat cache. It now validates that cache and
branches to the existing flat metadata path without allocating. Uncached
ropes and unknown UTF-16 metadata keep the helper fallback.

New regression testing also exposed null.length returning without an
exception on the pre-change binary (`cached-length-baseline-test.log`).
The VM and JIT length helper now share a nullish-checking value reader, and
the main JIT emitter checks its error result. Tests retain null/undefined
and throwing-getter coverage rather than ignoring that pre-existing defect.
The rebuilt candidate passes the focused Node/Ant test, snapshot tests and
all 6090 path differential cases. `cached-rope-length-results.json` pins the
short-flat and cached-length candidates in serial ABBA. Cwd resolve improves
170.09 -> 147.96 ms (-13%), POSIX relative 135.96 -> 122.95 (-10%), Windows
resolve 133.28 -> 125.76 (-6%), and Windows relative 182.18 -> 171.60 (-6%).
Normalize and parse are unchanged. Join's +2.5% median shift has overlapping
ranges; monitor it in the next comparisons. The nullish fix is included in
the candidate, not bypassed for measurement.
Preflight and all 4136 spec tests pass (`preflight-cached-rope-length.log`,
`spec-cached-rope-length.log`).

### Flat concatenation threshold experiments

The remaining rope_flatten samples motivated comparing the original 13-byte
cutoff with 64 bytes (`flat64-results.json`). The 64-byte candidate improves
POSIX relative 122.80 -> 97.61 ms (-21%), Windows relative 172.95 -> 147.30
(-15%), cwd resolve 148.24 -> 129.03 (-13%), Windows resolve 127.83 ->
114.92 (-10%). Normalize improves slightly; join and parse are unchanged.
Focused tests, 6090 differentials, preflight and all 4136 spec tests pass.
Long append controls are unchanged (`flat64-append-control-results.json`).
Additional 16/48/128/1024-byte retained/read controls are recorded in
`flat64-concat-control-results.json`; short read workloads improve 38-43%.

A serial ABBA retained-string memory control revealed the tradeoff:
200000 unread 48-byte results increase peak RSS from 24772608 to 30941184
bytes (+25%), `flat64-memory-results.json`. This deliberately keeps input
halves shared, exposing the cost of storing full flat results versus ropes.
Selected 32 bytes after the follow-up comparison; the 64-byte candidate
remains pinned as `ant-flat64` but is not retained in source.

`flat32-results.json`: POSIX relative 121.29 -> 97.11 ms (-20%), Windows
relative 171.20 -> 146.64 (-14%), Windows resolve 124.41 -> 114.95 (-8%).
Other rows are essentially unchanged, including cwd resolve, which gives
up the extra gain of the 64-byte experiment. The existing cwd/parse wins
over installed Ant remain. In `flat32-memory-results.json`, the 48-byte
retention control has unchanged peak RSS (24854528 -> 24813568 bytes).
This does not establish memory neutrality for every string size; it avoids
the measured 48-byte penalty while keeping the relative-path gains.
Focused tests and 6090 path differential cases pass on the 32-byte build.
Preflight and all 4136 spec tests pass. Long-append controls show no
regression (`flat32-append-control-results.json`, ratios 0.983/0.998/0.985).

### Native normalization trace

Temporary debug-gated native code dumping was used because release MIR
compiles out its detailed code-generation logs. The instrumentation has
been removed from source. Diagnostic binary: `ant-native-diagnostic`; no
performance claim uses it. `native-normalize.log` contains MIR plus mapped
machine bytes; `native-normalize-sample.txt` captures the same process.
`analyze-native.mjs` uses llvm-mc to decode sampled instruction windows into
`native-hotspots.json`. The 16 KiB per-function dump is bounded and does not
cover every sample; missing addresses are left unclassified.

The hot normalizeString body is `jit_M_0x7cda169010`. Samples include repeated
closure/native target loads and guards (offsets 3176, 2168, 2128), string
metadata loads (2352, 1572), and stack loads/boxing (824, 2020). The emitted
upvalue reader follows closure -> upvalue array -> cell -> location -> value
at every read. Captured constants and the separator callback warrant
entry-time load/guard investigation, preserving TDZ and mutable bindings.
This is diagnostic evidence for the next optimization, not a speed result.

### Captured-constant and target-guard experiment (removed)

The experimental source hoisted multiply-read const upvalues at JIT entry,
bailing before JS execution if a selected binding is still in TDZ. Mutable
bindings are not hoisted. A suspended-generator test exercises initialization
during a call, untaken TDZ reads, and different closures sharing compiled code.

Initial hoisting had no clear path impact (`constant-upvalues-results.json`).
MIR showed it applied mostly to the benchmark wrapper: esbuild bundling had
lowered the builtin's top-level const declarations to var. `gen_builtins.js`
now experimentally minifies standalone CJS modules without bundling while
retaining bundles for dependency-bearing modules. A missing Meson dependency
on that generator was also added. Generated output confirms preserved const
bindings. Combined measurements still show no clear gain
(`lexical-builtins-results.json`).

The next candidate caches the immutable uncurried charCodeAt target guard at
entry, propagating its guard register with stack value metadata and clearing
it at branch merges. Receiver/index checks stay per-call. Generated MIR
confirms guard reuse (`constant-char-guard-mir.log`), but the ABBA results
(`constant-char-guard-results.json`, against ant-flat32) still show no clear
normalize/join improvement. Some resolve medians improve about 4% with
overlapping ranges. Do not count this as an accepted speedup. The current
binary is pinned as `ant-constant-char-guard`; the last accepted candidate
is `ant-flat32`. Focused char guards, TDZ, primordial protection/GC, and 6090
path differential cases pass. Preflight and all 4136 spec tests also pass
(`preflight-constant-char-guard.log`, `spec-constant-char-guard.log`). The
experiment has now been removed: JIT hoisting, cached target guards, the
generator experiment, and its dedicated upvalue test. Recoverable copies are
in the artifact directory. The Meson generator dependency fix and paired
charCodeAt closure guard regression remain; earlier improvements are retained.

### Short snapshot append JIT fast path

The snapshot append helper remained hot in the normalization profile (373
self samples in `normalize-short-flat-sample.txt`). Non-captured, non-numeric
locals now reuse the existing inline concatenation allocator for short ASCII
strings (below 32 bytes), or return an immutable operand for an empty-string
append. This path cannot call or collect. It updates both the local register
and frame slot; captured locals, parameters, coercions, larger results, and
allocation pressure retain the helper. Snapshot operands are already evaluated,
so RHS replacement and conversion ordering remain unchanged.

The first build failed because the local-only block was placed in the
parameter branch; this was corrected before testing or measuring the candidate.
`snapshot-flat-jit-results.json` pins `ant-flat32` and `ant-snapshot-flat-jit`
with six samples each in serial ABBA. Median milliseconds per 100k calls:

| Workload | Before | Snapshot fast path |
| --- | ---: | ---: |
| POSIX normalize | 40.92 | 38.70 |
| Windows normalize | 77.86 | 74.95 |
| POSIX join | 78.08 | 74.83 |
| POSIX cwd resolve | 150.63 | 145.75 |
| POSIX relative | 99.72 | 93.88 |
| Windows resolve | 117.49 | 114.60 |
| Windows relative | 148.68 | 144.35 |
| POSIX parse | 19.23 | 19.04 |

Normalize and relative sample ranges do not overlap. Join and resolve changes
are smaller with overlapping ranges; parse is unchanged. Long append controls
are unchanged (candidate/base 0.994, 0.999, 0.993 for 32/10000/10000-with-read).
Focused snapshot, GC-root, short concat, and charCodeAt tests pass. Expanded
short-append boundaries cover 31/32 bytes. All 6090 Node differential cases
pass. Final rebuild is byte-identical to the measured candidate (SHA256
`42350abc96bb261d6db2e7fab5f860077913a542de572c27563f055fba5aca10`).
All 4136 spec tests in 102 files pass, as do preflight and the remaining focused
regressions (`spec-snapshot-flat-jit.log`, `preflight-snapshot-flat-jit.log`).
Meson regeneration was already exercised for the retained dependency fix;
no new build-graph change was made in this step.
Existing PGO configuration still reports stale control-flow profile warnings.
Installed-Ant parity remains unmet on five of eight workloads.

Fresh normalization profile: `normalize-snapshot-flat-sample.txt`, 5995 samples.
Snapshot append no longer appears among leading native self costs. Slice has
343 self samples, generic JIT call 171, allocation 128, truthiness 104. However,
4632 samples are unsymbolicated JIT frames; do not attribute those to slice
or claim that eliminating native calls alone closes the remaining gap.

### Installed-Ant checkpoint after snapshot fast path

`three-way-results.json` contains the latest serial pairwise ABBA run (12
samples each, unchanged hashes, matching checksums). Milliseconds per 100k:

| Workload | Installed Ant | Current Ant | Node |
| --- | ---: | ---: | ---: |
| POSIX normalize | 9.25 | 36.53 | 7.82 |
| Windows normalize | 10.05 | 72.98 | 14.30 |
| POSIX join | 14.28 | 71.34 | 17.47 |
| POSIX cwd resolve | 914.79 | 140.78 | 31.71 |
| POSIX relative | 24.51 | 90.57 | 30.86 |
| Windows resolve | 914.45 | 109.83 | 20.04 |
| Windows relative | 26.71 | 138.76 | 34.91 |
| POSIX parse | 22.02 | 18.60 | 2.30 |

Three installed-Ant wins remain, five gaps are 3.70-7.26x. Use the paired
before/candidate run above for attribution, not differences between separate
three-way checkpoints. The earlier table is preserved in
`three-way-last-flat32-results.json`.

### Follow-up native attribution and numeric-local lead

Temporary ARM64 diagnostics followed MIR's entry trampoline before dumping
the mapped code region. The first attempt dumped only the trampoline region;
it was corrected and the profile rerun. Artifacts: `native-second.log`,
`native-second-sample.txt`, `native-second-hotspots.json`. These are diagnostic
runs, not performance comparisons. Instrumentation was removed afterward;
source matches `swarm-before-second-native-profile.c`, and the restored build
hash matches `ant-snapshot-flat-jit` exactly.

The hottest mapped normalization offsets include 3176 (separator callee guard,
120 self samples), 3412 (common join after the inlined separator or its fallback,
117), 824 (stack load, 113), and 2352 (string metadata load, 102). The common
join is not evidence that the fallback call ran. Most previous unmapped samples
now resolve to normalization's generated code. The larger dump also identifies
short-string copy instructions beyond the earlier 16 KiB diagnostic limit.

Stronger next lead: MIR declares numeric mirrors for locals l1 through l7 and
guards them at OSR entry, but normal initialization/loop operations keep l1-l4
boxed while l5 (the loop index) uses its numeric mirror. The compiler hoists
lexical declaration markers; JIT `jit_has_immediate_numeric_local_init` only
accepts an immediately adjacent numeric constant and store. `OP_SET_LOCAL_UNDEF`
otherwise clears numeric specialization. This likely discards useful numeric
feedback for grouped declarations. Verify builtin bytecode or a minimal grouped
declaration repro, then consider recognizing a safe straight-line initialization
prefix. Preserve TDZ, captured bindings, reads before initialization, control
flow, and bailout state. The plain bytecode diagnostic did not dump the bundled
path function, so that exact-bytecode verification remains pending. No new
runtime optimization or speed claim was made in this diagnostic step.

### Removed grouped-initializer and register-argument experiments

The grouped-initializer candidate did preserve numeric registers in the repro
and bundled path MIR. Focused regressions, 6090 differential cases, and all
4136 specs passed. However, `grouped-init-results.json` and the reversed-order
`grouped-init-reverse-results.json` showed small mixed changes with overlapping
ranges, not a convincing path win.

The combined register-argument candidate also passed focused charCodeAt tests
and the 6090 differential cases. Its first benchmark was interrupted by SIGTRAP
inside macOS `pthread_jit_write_protect_np` during MIR initialization. The crash
report's UUID matched the candidate. Twenty repeated runs each of the combined,
grouped-only, and accepted snapshot binaries then passed. The completed rerun
(`char-register-args-results.json`) was noisy and inconclusive; no speedup was
accepted and the initialization trap's cause remains unresolved.

At the user's request, both experiments and the dedicated grouped-local test
were removed. Copies are recoverable in `/tmp/ant-path-goal.KC4jj3` as
`swarm-rejected-grouped-and-register-args.c` and
`test_jit_grouped_numeric_init.cjs`. Extra-argument evaluation regression coverage
for the existing charCodeAt opcode remains. `swarm.c` is restored exactly to
`swarm-before-second-native-profile.c`, retaining all previously accepted work.
Earlier hoisting/target-guard experiments and temporary native diagnostics are
also absent. The rebuild matches the accepted snapshot binary SHA256
`42350abc96bb261d6db2e7fab5f860077913a542de572c27563f055fba5aca10`.
Preflight, focused charCodeAt/short-append tests, all 6090 Node differential
cases, and all 4136 spec tests pass (`preflight-clean-experiments.log`,
`spec-clean-experiments.log`). The refreshed accepted-only comparison is
`three-way-results.json`; its prior version is preserved in
`three-way-before-cleanup-table.json`.

## Baseline and evidence

Artifacts: `/tmp/ant-path-goal.KC4jj3`; preserved `ant-baseline` SHA256
`2a424f01d83f343d26e893e331c507083731a7aa041b95679e790b250faccb2d`.
Original Node comparison: `/tmp/ant-path-node.b1fMjy/results.json`, Node v26.8.1,
100k measured operations and 20k warmups, six runs per binary in serial ABBA.
Ant/Node ratios: POSIX normalize 8.48, Windows normalize 7.49, POSIX join 6.72,
POSIX cwd resolve 36.98, POSIX relative 8.59, Windows absolute resolve 11.47,
Windows relative 8.34, POSIX parse 12.29. Checksums agree on the fixture.

Profiles: `/tmp/ant-path-hot.VxZN5A/findings.md`. Instruments aborted; macOS
sample captured 8 seconds per workload. Relative spends 19.6% self samples in
generic call dispatch; rope_flatten 7.7%, str_utf16_len 7.0%. Prebuilt input
control reproduces these costs. Normalize string-construction stacks cover
33.5% inclusive samples; char opcode helper stacks cover 16.3%. Inclusive
categories overlap. Unsymbolicated JIT frames limit attribution.

## Work log

1. Implemented: allow the guarded character reader to use a rope's existing
   cached flat string, without allocation or mutation. Uncached, builder, and
   non-ASCII values retain the original generic fallback. Add repeated ASCII
   and Unicode rope character-read coverage. Build and focused Ant/Node tests,
   primordial mutation tests and primordial GC tests pass. Serial ABBA against
   preserved baseline: POSIX relative 272.69 -> 207.62 ms (-24%), Windows
   absolute resolve 236.71 -> 205.61 (-13%), Windows relative 299.37 -> 263.36
   (-12%). Normalize/join/parse unchanged. `rope-results.json` records hashes
   and all samples.
2. Implemented cwd cache after an 8-second isolated cwd profile showed mostly
   getcwd filesystem syscalls (`cwd-sample.txt`). Node v26.8.1's installed
   bootstrap source caches cwd until successful chdir. Ant now does the same,
   with a rooted per-runtime cached string and process-wide atomic generation
   invalidation across runtimes. External native chdir is not intercepted,
   as with Node's JS cache. Cache and uncached failure tests pass under both
   Ant and Node, including deleted cwd, failed chdir, and allocation churn.
   Serial ABBA: cwd resolution 1134.02 -> 247.81 ms (-78%); other workloads
   approximately unchanged. See `cwd-results.json`. Still above the 2x target.
3. Address remaining measured character guard, string construction, and parse
   bottlenecks individually, with per-change measurements. An 8-second parse
   profile (`parse-sample.txt`, 5914 samples) has js_try_char_code_at 556 self
   samples, js_type_alloc 308, js_mkstr_utf16_range 307, memmove 277,
   sv_call_native 223, jit_helper_call 195, get_slot 178, builtin_string_slice
   163, jit_helper_typeof 157, builtin_function_call 149, and
   jit_helper_define_slot 140. utf16_range_to_byte_range already has an ASCII
   shortcut; do not infer that these samples represent UTF-16 scanning.
   Next investigate guarded/inlined string operations and object literal
   initialization, with a full-result parse control to distinguish unused
   result elimination from general runtime work.

## Subsequent measured changes

- Short immutable snapshot append: only STR_ALC_SNAPSHOT uses a bounded flat
  append for results up to the existing builder tail capacity. An initial
  experiment applying it to every append regressed long append controls by
  43% and was revised, not retained. The snapshot-only version improves POSIX
  normalize 64.62 -> 54.38 ms (-16%), Windows normalize -10%, join -10%, cwd
  resolve -8%, POSIX relative -10%, Windows resolve -6%, Windows relative -7%.
  Parse unchanged. Serial controls for 32/10000 appends, with and without
  reads, are unchanged (0.999x, 1.007x, 0.997x). Artifacts:
  `short-snapshot-results.json`, `snapshot-control-results.json`.
- Inline JIT uncurried charCodeAt: immutable SV_CALL_IS_UNCURRY identifies
  native Function.call.bind wrappers independently of primordials. Dedicated
  opcode MIR guards the native target, ASCII flat/cached-rope receiver,
  in-range integral index and active VM execution, then reads the byte.
  Other cases use the existing helper. No allocation occurs on the fast path.
  Serial ABBA against the snapshot-only binary: normalize 54.91 -> 46.07 ms,
  Windows normalize 93.96 -> 84.03, join 102.58 -> 89.93, cwd resolve 220.85 ->
  200.23, relative 180.47 -> 156.35, Windows resolve 187.20 -> 174.27, Windows
  relative 233.68 -> 207.26, parse 27.81 -> 25.14. See
  `char-inline-results.json`. Focused guards/primordial tests pass; expanded
  differential: 6090 Node cases, zero differences (`differential-results.json`).
  Broad suite: 4121 passed, only the known baseline fetch DNS error.
- Fresh parse profile (`parse-inline-sample.txt`) no longer has charCodeAt
  among the hottest native functions. Slice construction/allocation and
  redundant Function.call/native dispatch remain hot. Implemented: collapse
  native uncurry call plans, retaining current_func, exact explicit receiver,
  bound arguments and cleanup; leave construction and outside-VM checkpoints
  unchanged. A new test exposed pre-existing frozen Array.push behavior on
  the preserved pre-change binary; not changed in this scoped optimization.
  Serial ABBA (`uncurry-results.json`): parse 26.01 -> 24.61 ms (-5%),
  join 94.30 -> 88.25 (-6%), relative 164.41 -> 156.94 (-5%); other rows
  improve 1-3%, with overlapping ranges. Native uncurry tests pass on Node
  and Ant, including explicit receivers, rebinding, and exception identity.
- Primitive typeof comparisons: compile comparisons against primitive type
  names to a tag-test opcode instead of allocating/returning a type string.
  Preserve operand evaluation, TDZ, undeclared globals, with/eval resolution,
  and generic object/function classification. Both main and inline JIT
  emitters support the opcode. `type-test-results.json`: join 89.41 -> 81.30
  ms (-9%), Windows resolve 176.08 -> 166.73 (-5%), parse 24.49 -> 23.02
  (-6%). Other improvements are smaller. Focused Ant/Node regression passes.
  Broad suite at this stage: 4136 passed, zero failures (`spec-type-test.log`).
- Constant object-literal templates: JIT recognizes consecutive constant
  definitions with static keys and no branch entry into the skipped span.
  A private GC-rooted template reuses the existing object-cloning primitive;
  each execution still creates an independent object and property storage.
  Dynamic values, computed keys, spreads, and accessors retain normal code.
  `object-template-results.json`: parse 23.20 -> 20.19 ms (-13%); other seven
  workloads unchanged. Candidate SHA256:
  `18f4f42a934af0198dfe9594b27bb53546fcd78433faaea5c499220a4a18315a`.
  Focused identity, mutation, descriptors, prototype, and fallback tests pass
  on Ant and Node; 6090 path differential cases have zero differences.
  Two subsequent allocation-error checks require a rebuild and validation.

## Validation checkpoint (before subsequent changes)

Not complete. All eight performance gates remain open. Existing tree changes
to charCodeAt, Reflect.set, and assert.throws are retained. Build configuration
uses existing PGO data with stale-profile warnings; final evidence needs its
configuration recorded, with no performance attribution based on unmatched
profile configurations. Both incremental builds succeeded. Preflight passed.
The current broad suite passes 4121 tests and 101 files; its single fetch DNS
failure also reproduces on the preserved baseline (`fetch-baseline.log`).
Focused charCodeAt, cwd cache/error, primordial protection/GC, assert constructor,
and Reflect.set tests pass. Full path differential expansion remains pending.

Latest serial Node comparison: `node-after-cwd.json` pins both binaries and all
samples. Ratios in the original workload order: 8.24, 7.26, 6.56, 7.68, 6.49,
9.92, 7.22, 12.28. Thus zero of eight gates are met yet. Cwd syscall overhead
and cached-rope character dispatch have improved, but substantial general
engine work remains. Do not mark the goal complete from these partial wins.

# Buffer Index Coercion Review Fixes

Status: active
Last reviewed: 2026-09-05
Owner: theMackabu

## Problem and scope

The new ToIndex calls can run user code between validating a buffer offset and
using its length. A transfer during length conversion sets the backing length
to zero; subtracting a previously valid positive offset then wraps size_t.
DataView accessors trust the resulting view length and can access beyond the
allocation. Transfer itself also needs to reject a receiver detached during
newLength conversion.

The iterable-first TypedArray constructor also routes TypedArray sources through
user-overridable iteration and discards an already retrieved iterator method
when calling the boolean js_iter helper. That helper cannot preserve iterator
exceptions or distinguish collection allocation failure from normal completion.

## Decisions

- Validate TypedArray backing state after offset and length conversion, before
  subtracting the offset. Preserve DataView's captured buffer length for its
  length bounds check and reject detachment before creating the view.
- Recheck transfer detachment after newLength conversion.
- Copy TypedArray sources using internal length and elements, with a byte copy
  for matching types and a Number/BigInt content-type check.
- Use a buffer-local iterator collector with the captured method and cached next
  method. Propagate method/property errors, validate iterator results, and root
  collected values until element conversion finishes.
- Leave the shared iterator APIs and TypedArray.from unchanged. Reworking those
  callers is unnecessary to fix constructor dispatch. Ordinary array iteration
  retains O(n) temporary storage; a native-array optimization needs guards for
  observable iteration and conversion ordering and is separate performance work.

## Validation

- Built the reviewed implementation in the repository Nix dev shell. The added
  regression failed at the first coercion-detachment case with `expected
  TypeError`, demonstrating that it reaches the missing rejection.
- The expanded regression passes under Node.
- Final rebuild succeeded in the repository Nix dev shell. The compiler reported
  stale PGO profile control-flow warnings for modified functions.
- Expanded `tests/test_arraybuffer_length.cjs`: passed on the rebuilt Ant binary.
- Seven related regression files passed: buffer, buffer_indexof, buffer_inspect,
  buffer_wtf8_utf8, buffer_species_arraybuffer, arraybuffer_isview, and
  typedarray_apply.
- Focused buffer and dataview specs: 261 tests passed, zero failures.
- Full spec suite (`--all`): 4,136 tests passed across 102 files, zero failures.
- Manifest module, maid preflight, maid knowledge, and git diff --check: passed.

Implementation and validation are complete; changes remain uncommitted. No
ASan build or allocation-failure injection was run.

The normal shell initially failed to locate libm and then llvm-nm. Building with
`nix develop` and the SDKROOT from packages/nix/shell.nix restores the configured
toolchain without changing repository build configuration.


## Follow-up: dense arrays and iterator result templates

The constructor now recognizes a native array-values iterator after calling the
captured iterator method and retrieving next. It checks matching source, initial
iterator state, native next identity, dense storage, and exclusively numeric
elements. It updates the iterator state and copies directly without temporary
iterator results or a collected-value list. Holes, indexed accessors, objects,
custom next methods, and partially consumed iterators retain the general path.

Native js_iter_result uses a lazy per-isolate rooted template, following the
CryptoKey record pattern. Each call clones a fresh object and sets its done and
value slots with a GC write barrier. It preserves existing descriptors, property
order, and the existing native result prototype (null in Ant). Tests cover
result mutation isolation and retained object-valued results across allocation
pressure. An initial test incorrectly expected Object.prototype; both the old
and new binaries returned null, so the test now checks prototype preservation.

Validation: both focused tests pass; all 4,136 spec tests pass; preflight and
diff whitespace checks pass. Seven interleaved fresh-process benchmark samples
per case, using median times and matching checksums, measured:

- Array to Uint8Array (4,096 elements): master 156 us, prior diff 333 us,
  optimized 14.4 us per construction (10.9x faster than master).
- Array to Uint8Array (65,536 elements): master 2,512 us, prior diff 5,395 us,
  optimized 164 us (15.3x faster than master).
- Native iterator next loops: 2.2–2.5x faster than the prior diff across arrays,
  TypedArrays, and strings; this isolates result templates from constructor
  fast paths.
- Same-type TypedArray copies and custom JavaScript iterables are unchanged
  relative to the prior diff.

Benchmark scripts and samples are in /tmp/ant-buffer-optimized-bench.py,
/tmp/ant-iterator-bench.cjs, and /tmp/ant-buffer-optimized-results.json. No ASan
or forced allocator-failure testing was performed for this follow-up.

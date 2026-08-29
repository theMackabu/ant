# PR #44 Source Port Audit

Status: active
Last reviewed: 2026-08-29
Owner: theMackabu

## Goal

Decide which source changes from stale PR #44
(`perf/silver-jit-elysia-parity`) should be recreated on current master, without
recovering or merging the branch wholesale.

This is a source audit, not a performance result. It compares the original PR
delta against the current implementation and records one verdict for every
changed file under `include/` or `src/`.

## Pinned Revisions

- PR merge base: `ca8e720d5e0a468799011401f0f16f3fe49a859b`
- PR head: `af696a85`
- Current master during this audit: `65a98043`
- Source delta: 42 files, 3296 insertions, 715 deletions

Current master and the PR have diverged substantially: master has 69 commits
not in the PR, and the PR has 40 commits not in master. The audit therefore
uses behavior and invariants, not whether an old hunk still applies cleanly.

## Verdict Legend

- **Port**: still absent and useful enough to recreate as a small current-tree
  change.
- **Port separately**: valid compatibility or observability work, but not part
  of the Elysia performance sequence.
- **Superseded**: current master already contains the idea in a newer or safer
  form.
- **Dependency only**: contains declarations or call-site glue; follow the
  implementation verdict instead of porting it independently.
- **Redesign**: the optimization is still interesting, but the PR's design is
  unsafe or too stale to port.
- **Reject**: do not reproduce the PR change.

## Executive Verdict

Do not recover, merge, or cherry-pick the branch. Recreate only isolated ideas.

The best first candidates are:

1. Cache immutable `Request.method` and `Request.url` values, including the two
   reserved slots, as one small measured change.
2. Remove the heap allocation from `js_prop_iter_begin()` by storing the
   isolate, object, and index directly in `ant_iter_t`.
3. Make Buffer registry removal O(1) with `registry_slot`, independently of the
   old Buffer encoding rewrite.
4. Reimplement the Headers allocation/layout work in a focused change with
   fallible native operations and ByteString coverage.
5. Only after Headers is sound, evaluate lazy Response headers and borrowed
   immutable string bodies. These are the most plausible Elysia wins, but the
   PR implementation has unchecked failure paths and cannot be copied.

The old GC policy must not be ported. It removes the only fallback that
eventually collects array-heavy workloads while still failing to account for
array backing-store pressure. The active array-pacing plan must be completed
first.

The old dynamic string-key element IC must also not be ported. It stores a
GC-managed `ant_value_t` key in code-arena IC metadata, but the PR's function
marking does not trace that key. A collection followed by address reuse can
turn the raw equality guard into an ABA-style false hit and return the wrong
property. Any renewed design needs a stable intern identity or an explicitly
traced/invalidated key.

## File-by-File Verdicts

### Public and Runtime Headers

| File                         | Verdict                                           | Evidence and action                                                                                                                                                                                                                                                                                                        |
| ---------------------------- | ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `include/ant.h`              | **Port part**                                     | The allocation-free property iterator is still absent: current `ant_iter_t` owns a heap `ctx`. Recreate that small ABI change. Do not couple it to the PR's 256-byte native-data arena; measure Headers/Response first and choose ownership deliberately.                                                                  |
| `include/arena.h`            | **Superseded**                                    | Current master already has `fixed_arena_alloc_uninit()` and uses it for initialized-on-write allocations. The spelling and surrounding arena implementation changed; nothing should be recovered.                                                                                                                          |
| `include/common.h`           | **Dependency only**                               | The two request cache slots are absent and should land only with the `Request.method`/`url` cache. Reserving them without the getters adds per-object state for no benefit.                                                                                                                                                |
| `include/gc.h`               | **Reject**                                        | The PR lowers the pool floor from 8 MiB to 1 MiB and removes timing constants used by the fallback. It does not make array backing stores visible to pacing. Keep current policy until the array-pacing plan is resolved and benchmark a new policy independently.                                                         |
| `include/gc/stats.h`         | **Port separately, optional**                     | Exit telemetry is useful for diagnosis but is not an Elysia optimization. If revived, keep it opt-in, use the current GC policy fields, and verify the timing API on Windows. Do not make it a prerequisite for runtime changes.                                                                                           |
| `include/internal.h`         | **Mostly superseded; dependency only**            | Find-only interning, UTF validity state, fixed-arena evolution, and object-site shape work already exist in newer forms. Native constructor metadata, ASCII character caching, and intern-cache additions should be reconsidered only with their implementations. Do not copy this broad state-layout change.              |
| `include/modules/buffer.h`   | **Port**                                          | `registry_slot` is still absent and current unregister remains a linear scan. Add the field together with swap-remove registration/unregistration and focused lifetime tests.                                                                                                                                              |
| `include/modules/headers.h`  | **Redesign**                                      | Native pending-header APIs are still absent and are relevant to Response construction, but the PR mixes allocation policy, lazy materialization, and fallible operations. Define an explicit ownership/error contract before adding the APIs.                                                                              |
| `include/modules/regex.h`    | **Superseded**                                    | Regex subject validity, watched-property handling, and execution plumbing were substantially rewritten on current master. Mine old tests only; do not transplant the old interface.                                                                                                                                        |
| `include/modules/response.h` | **Redesign**                                      | Lazy pending headers and borrowed body ownership are still absent, but the old `response_get_headers(js, obj)` becomes fallible without making all callers handle errors. Redesign the API after Headers lands.                                                                                                            |
| `include/object.h`           | **Measure before porting**                        | Current objects still keep `exotic_ops` and `exotic_keys` in the hot object rather than the sidecar, so the size reduction is not present. The object layout has otherwise changed heavily. Record `sizeof(ant_object_t)` and object/exotic population before moving fields; update GC/finalization atomically if it wins. |
| `include/silver/engine.h`    | **Mostly superseded; redesign dynamic-key state** | Current object-site cache and 64-byte IC layout are newer. The PR's `cached_key`/accessor additions are not present and are not GC-safe as implemented. Native-constructor and no-JIT declarations are dependencies, not standalone ports.                                                                                 |
| `include/silver/glue.h`      | **Dependency only**                               | All changes are helper signatures for the old IC, object-site, string-intrinsic, import, or constructor designs. Follow the corresponding implementation verdicts; do not grow the helper surface first.                                                                                                                   |
| `include/silver/opcode.h`    | **Redesign**                                      | The widened GET_ELEM/NEW/IMPORT opcodes and fixed string intrinsic opcodes are absent. Current master already has newer GET_ELEM specialization and descriptor-driven Map templates. Add bytecode only after a measured current-tree design proves it needs a dedicated site; do not revive the PR encodings.              |
| `include/utf8.h`             | **Superseded**                                    | Current UTF-8/WTF-8 and UTF-16 range APIs are broader and safer. The PR's in-place surrogate helper also failed to validate the third continuation byte. Keep the current implementation.                                                                                                                                  |

### Core Runtime and GC

| File               | Verdict                                                    | Evidence and action                                                                                                                                                                                                                                                                                                                         |
| ------------------ | ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/ant.c`        | **Port two isolated ideas; otherwise superseded/redesign** | Port the allocation-free property iterator. Consider one-byte string caching only with allocation data. Find-only lookup interning, argument snapshots, UTF handling, and shared object shapes already landed in newer forms. The native-data arena and exotic sidecar move need independent size/profile evidence and must not be bundled. |
| `src/gc/gc.c`      | **Reject**                                                 | The adaptive rewrite removes the tick/time fallback but still gates pool checks behind nursery conditions and ignores array backing stores. That converts an existing pacing blind spot into potentially unbounded growth. Rebuild policy only after the active array-pacing plan, with plateau and throughput tests.                       |
| `src/gc/objects.c` | **Dependency only**                                        | The PR only marks/frees new cached roots and sidecar state and adds telemetry hooks. Current object-site shape marking is already newer. Revisit this file only with a chosen object-layout or telemetry change.                                                                                                                            |
| `src/gc/stats.c`   | **Port separately, optional**                              | The JSON-at-exit diagnostics can be recreated against the current collector, but the file is observability rather than throughput. Preserve zero-cost disabled paths and validate Windows clocks/output before landing.                                                                                                                     |
| `src/main.c`       | **Dependency only**                                        | Only exposes `ANT_DEBUG=gc:stats`. Land it with a redesigned telemetry module, not alone.                                                                                                                                                                                                                                                   |
| `src/pool.c`       | **Reject as policy; dependency for telemetry**             | The PR adds cause tracking around the existing pool-triggered major collection. Do not mix telemetry with the unsafe 1 MiB/adaptive policy. Cause reporting may be added later without changing collection thresholds.                                                                                                                      |
| `src/utf8.c`       | **Superseded**                                             | Current master has stricter WTF-8/UTF-8 export and UTF-16 range handling. Do not restore the PR helper that recognizes a surrogate prefix without fully validating the sequence.                                                                                                                                                            |

### Built-ins and Host Modules

| File                      | Verdict                                              | Evidence and action                                                                                                                                                                                                                                                                                                                                                                                       |
| ------------------------- | ---------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/modules/buffer.c`    | **Port O(1) unregister; audit semantics separately** | Current unregister scans the entire global registry, so `registry_slot` plus swap-remove is still useful. Much of the encoding support exists, but the PR went through several correctness fixes and current UTF export has since changed. Reuse tests, not the stale encoding diff.                                                                                                                      |
| `src/modules/builtin.c`   | **Dependency only**                                  | The PR only accounts for/follows exotic sidecar storage. It has no independent optimization.                                                                                                                                                                                                                                                                                                              |
| `src/modules/crypto.c`    | **Port separately**                                  | AES-GCM/HMAC `generateKey` and raw `exportKey`, extractability, usages, and cleansing are compatibility work, not the Elysia path. Current master still creates imported keys as non-extractable and lacks those APIs. Recreate with current WebCrypto validation and promise conventions.                                                                                                                |
| `src/modules/fetch.c`     | **Port with Headers**                                | Avoiding transient JS strings while ingesting literal headers is sensible, and current code already has a fallible `headers_append_literal()` path in some places. Complete it only after the new Headers storage contract is fixed; do not make it a separate data representation.                                                                                                                       |
| `src/modules/headers.c`   | **High-priority redesign**                           | The one-allocation entry layout, inline first entry, ByteString-preserving values, and native pending data are plausible HTTP wins. The PR's surface still has unchecked allocation paths such as a void `headers_data_append_if_missing`. Reimplement in stages: storage layout, fallible primitives, ByteString tests, then pending/lazy materialization.                                               |
| `src/modules/path.c`      | **Port separately after Node differential tests**    | The PR fixes `relative(resolve(from), resolve(to))` behavior and Windows drive-relative/rooted cases. Current master again normalizes the raw inputs instead of resolving them against cwd, so the compatibility issue remains. It is not an Elysia optimization.                                                                                                                                         |
| `src/modules/regex.c`     | **Superseded; mine tests only**                      | The file has undergone a much larger cache/statics/execution rewrite. Current master already caches validity and handles current UTF/lastIndex machinery. The old split fast path required multiple follow-up fixes, so porting it would reintroduce obsolete assumptions.                                                                                                                                |
| `src/modules/request.c`   | **Port**                                             | Current `method` allocates a new JS string on every getter call; `url` rebuilds and allocates the href every time. The underlying request data is immutable after construction. Cache each value in a rooted reserved slot and measure the request benchmark in isolation.                                                                                                                                |
| `src/modules/response.c`  | **High-priority redesign**                           | Lazy Headers creation and borrowing immutable JS string bodies can remove real per-response work. The PR implementation ignores failures in clone/header copy, uses fallible header materialization without a consistent caller contract, and has direct shape-slot assumptions requiring revalidation. Port only after Headers, in separate steps with GC ownership and subclass/getter semantics tests. |
| `src/modules/server.c`    | **Do not port old hunk**                             | The PR updates calls to a now-fallible `response_get_headers(js, obj)` but does not check the returned error before enumeration/serialization. A redesigned Response API must make these two paths propagate failure before any use.                                                                                                                                                                      |
| `src/modules/textcodec.c` | **Superseded**                                       | Current TextEncoder uses the newer string validity/export machinery and has separate Latin-1 work. Do not restore the PR's copy-and-patch helper.                                                                                                                                                                                                                                                         |

### Silver Compiler, Interpreter, and JIT

| File                          | Verdict                                             | Evidence and action                                                                                                                                                                                                                                                                                                                                                                                    |
| ----------------------------- | --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/silver/compiler.c`       | **Mostly superseded; redesign string-call idea**    | Object literal site analysis is now precomputed and safer. The old GET_ELEM/DEFINE/NEW/IMPORT IC operands are coupled to stale layouts. The syntax-recognized string `indexOf`/`substring` calls may still be measurable, but should use current call/intrinsic infrastructure and preserve property override, coercion, optional chaining, try/finally, and argument snapshot semantics.              |
| `src/silver/engine.c`         | **Dependency only**                                 | This file only decodes the PR's widened bytecode and dispatches its two string intrinsics. Current opcode widths and engine paths differ. Port nothing until a current compiler/opcode design exists.                                                                                                                                                                                                  |
| `src/silver/glue.c`           | **Mostly superseded; reject dynamic-key helper**    | Current master has newer object-site and PUT_FIELD helpers, and general call/construct plumbing has changed. Reject the old dynamic GET_ELEM helper because its cached key is untraced. Import/constructor/string helpers need isolated evidence before recreation.                                                                                                                                    |
| `src/silver/ops/calls.h`      | **Redesign if measured**                            | Native-constructor receiver elision and constructor-prototype caching are absent, but current construct semantics are centralized in `sv_prepare_construct_meta()` and related feedback. Optimize that current path rather than copying the old flag/opcode scheme.                                                                                                                                    |
| `src/silver/ops/coercion.h`   | **Port only if import profiling justifies it**      | Named-import property lookup still lacks the PR's per-site IC in the interpreter. It is a small valid concept, but module initialization is not obviously an Elysia steady-state bottleneck. Keep it behind startup/profile evidence and current namespace semantics.                                                                                                                                  |
| `src/silver/ops/comparison.h` | **Small port candidate**                            | Current `instanceof` IC declines primitive left operands before caching, so repeated primitive negative results still take the slow path. The PR's negative-cache idea is viable if it reuses the current epoch/protector rules and gets proxy/`Symbol.hasInstance` tests. Low priority for Elysia.                                                                                                    |
| `src/silver/ops/globals.h`    | **Superseded by current policy**                    | The PR merely advances IC warmup state on successful hits/fills. Current global IC layout and activation policy have changed and now fills directly. Reassess via current miss counters rather than transplanting the old state mutation.                                                                                                                                                              |
| `src/silver/ops/property.h`   | **Reject dynamic-key design; otherwise superseded** | Current fixed-name property IC and PUT_FIELD paths are newer. The PR's computed string-key IC keeps an untraced GC value and can produce a stale raw-key hit. Accessor/prototype extensions also need current `prop_count`, deletion, proxy, and epoch guards. Design a stable-key IC from scratch if profiles still demand it.                                                                        |
| `src/silver/swarm.c`          | **Superseded except future measured redesigns**     | Current master has the newer raw object-flag masks, ABI validation, extracted PUT_FIELD add guard, barriers, inline site names, object-site lookup, numeric dense element specialization, and bailout work. Do not port the old 762-line emitter expansion or its hard-coded bit positions. A future generic string-key element IC or string-call intrinsic must be built against current MIR helpers. |

## Recommended Port Sequence

Each step should be a separate commit and should be kept only if correctness
gates pass and a profile-matched serial A/B test shows the intended benefit.

1. **Request getter cache**
   - Add the two slots and reserve them at Request creation.
   - Cache `method` and the built `url` string.
   - Validate clone/constructor/server requests and GC rooting.
2. **Allocation-free property iterator**
   - Put `js`, `obj`, and the index directly in `ant_iter_t`.
   - Verify nested iterators, empty/invalid inputs, symbols, deletion, and GC
     safety across value materialization.
3. **O(1) Buffer registry removal**
   - Add `registry_slot`; update the swapped last entry on removal.
   - Test first/middle/last removal and failed registration growth.
4. **Headers storage layout**
   - First make all storage primitives explicitly fallible.
   - Then introduce the compact entry/inline-first-entry representation.
   - Preserve obs-text/ByteString values across JS and HTTP boundaries.
5. **Response slices, not a rewrite**
   - First borrow immutable JS string bodies with an explicit rooted owner and
     ownership enum.
   - Then add lazy Headers materialization with one fallible API used by every
     server/fetch/clone caller.
   - Finally consider the narrow content-type init fast path after semantic
     differential tests.
6. **JIT work only from current profiles**
   - If computed string property access remains hot, design a GC-safe stable-key
     IC. Do not use an untraced `ant_value_t` in code metadata.
   - If string calls remain hot, prefer current descriptor/intrinsic patterns
     over fixed stale opcodes.
7. **GC policy last**
   - Complete array backing-store pacing first.
   - Require array-churn RSS plateau tests plus throughput tests before changing
     fallback timing or the pool-pressure floor.

Buffer encoding, WebCrypto, path, regex, telemetry, import IC, and primitive
`instanceof` work belong in separate compatibility/diagnostic/performance
changes. They should not be credited to the Elysia optimization sequence
without measurements.

## Validation Status

- Compared the complete 42-file `include/` and `src/` delta from the pinned PR
  merge base to the pinned PR head.
- Compared every changed file with current master and inspected the current
  implementations of the affected runtime/JIT/module paths.
- Cross-checked the GC verdict with the active array backing-store pacing plan.
- Confirmed all 42 changed source files have an explicit verdict in this file.
- No source files were edited.
- No build, benchmark, or runtime test was run; this report makes no measured
  performance claim.

## Decision Log

- 2026-08-29: Chose idea-by-idea recreation over branch recovery because the
  PR and master have both diverged materially.
- 2026-08-29: Rejected the PR GC policy as a port source until array backing
  stores participate in pacing.
- 2026-08-29: Rejected the PR dynamic-key IC representation because its cached
  GC key is not traced.
- 2026-08-29: Prioritized Request caching, property-iterator allocation removal,
  Buffer registry removal, then staged Headers/Response work.

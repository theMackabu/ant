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

## Port Status

- [x] Cache `Request.method` and `Request.url` in rooted reserved slots. The
  current-tree port also invalidates the private fetch Request's caches when a
  redirect rewrites its method or URL. Completed 2026-08-29.
- [x] Remove the heap allocation from property iteration by storing the isolate,
  object, and offset directly in `ant_iter_t`. Completed 2026-08-29.
- [x] Make Buffer registry removal O(1). The current-tree port uses a 32-bit
  tail slot and narrows the two boolean fields, preserving the existing
  40-byte 64-bit and 24-byte 32-bit `ArrayBufferData` layouts. Completed
  2026-08-29.
- [x] Redesign compact/fallible Headers storage. Entries now use one allocation
  with a 96-byte inline first-entry buffer; copy, set, and append-if-missing
  paths report allocation failure; JS values preserve ByteString semantics;
  and raw HTTP ingestion no longer round-trips through a JS UTF-8 string.
  Completed 2026-08-29.
- [ ] Port Response optimizations in isolated ownership-safe slices after
  Headers.
  - [x] Borrow direct immutable JS string bodies through a rooted Response slot
    and an explicit storage state, avoiding the body byte copy and content-type
    allocation. `response_data_t` remains 136 bytes on the current 64-bit ABI.
    Completed 2026-08-29.
  - [ ] Lazy Headers materialization and the narrow init-shape fast path remain
    unported. Keep them separate: server serialization materializes headers
    immediately, so laziness alone needs current-tree profile evidence.

## File-by-File Verdicts

### Public and Runtime Headers

| File                         | Verdict                                           | Evidence and action                                                                                                                                                                                                                                                                                                        |
| ---------------------------- | ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `include/ant.h`              | **Ported in part**                                | The allocation-free property iterator landed on 2026-08-29. The PR's 256-byte native-data arena remains intentionally unported; measure Headers/Response first and choose ownership deliberately.                                                                                                                   |
| `include/arena.h`            | **Superseded**                                    | Current master already has `fixed_arena_alloc_uninit()` and uses it for initialized-on-write allocations. The spelling and surrounding arena implementation changed; nothing should be recovered.                                                                                                                          |
| `include/common.h`           | **Ported dependency**                             | The two request cache slots landed with the `Request.method`/`url` cache on 2026-08-29.                                                                                                                                                                                                                                   |
| `include/gc.h`               | **Reject**                                        | The PR lowers the pool floor from 8 MiB to 1 MiB and removes timing constants used by the fallback. It does not make array backing stores visible to pacing. Keep current policy until the array-pacing plan is resolved and benchmark a new policy independently.                                                         |
| `include/gc/stats.h`         | **Port separately, optional**                     | Exit telemetry is useful for diagnosis but is not an Elysia optimization. If revived, keep it opt-in, use the current GC policy fields, and verify the timing API on Windows. Do not make it a prerequisite for runtime changes.                                                                                           |
| `include/internal.h`         | **Mostly superseded; dependency only**            | Find-only interning, UTF validity state, fixed-arena evolution, and object-site shape work already exist in newer forms. Native constructor metadata, ASCII character caching, and intern-cache additions should be reconsidered only with their implementations. Do not copy this broad state-layout change.              |
| `include/modules/buffer.h`   | **Ported**                                        | `registry_slot` landed with swap-remove registration/unregistration on 2026-08-29. The current port preserves the previous struct size on both 64-bit and 32-bit targets.                                                                                                                                                  |
| `include/modules/headers.h`  | **Ported in part**                                | The existing append-if-missing API is now fallible. Native pending-header ownership APIs remain intentionally absent until lazy Response materialization has current profile evidence.                                                                                                                                    |
| `include/modules/regex.h`    | **Superseded**                                    | Regex subject validity, watched-property handling, and execution plumbing were substantially rewritten on current master. Mine old tests only; do not transplant the old interface.                                                                                                                                        |
| `include/modules/response.h` | **Ported in part**                                | Borrowed string body ownership now has an explicit compact storage state without growing `response_data_t`. Lazy pending headers remain deferred because their fallible materialization contract still affects every caller.                                                                                              |
| `include/object.h`           | **Measure before porting**                        | Current objects still keep `exotic_ops` and `exotic_keys` in the hot object rather than the sidecar, so the size reduction is not present. The object layout has otherwise changed heavily. Record `sizeof(ant_object_t)` and object/exotic population before moving fields; update GC/finalization atomically if it wins. |
| `include/silver/engine.h`    | **Mostly superseded; redesign dynamic-key state** | Current object-site cache and 64-byte IC layout are newer. The PR's `cached_key`/accessor additions are not present and are not GC-safe as implemented. Native-constructor and no-JIT declarations are dependencies, not standalone ports.                                                                                 |
| `include/silver/glue.h`      | **Dependency only**                               | All changes are helper signatures for the old IC, object-site, string-intrinsic, import, or constructor designs. Follow the corresponding implementation verdicts; do not grow the helper surface first.                                                                                                                   |
| `include/silver/opcode.h`    | **Redesign**                                      | The widened GET_ELEM/NEW/IMPORT opcodes and fixed string intrinsic opcodes are absent. Current master already has newer GET_ELEM specialization and descriptor-driven Map templates. Add bytecode only after a measured current-tree design proves it needs a dedicated site; do not revive the PR encodings.              |
| `include/utf8.h`             | **Superseded**                                    | Current UTF-8/WTF-8 and UTF-16 range APIs are broader and safer. The PR's in-place surrogate helper also failed to validate the third continuation byte. Keep the current implementation.                                                                                                                                  |

### Core Runtime and GC

| File               | Verdict                                                    | Evidence and action                                                                                                                                                                                                                                                                                                                         |
| ------------------ | ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/ant.c`        | **One part ported; otherwise superseded/redesign**         | The allocation-free property iterator landed on 2026-08-29. Consider one-byte string caching only with allocation data. Find-only lookup interning, argument snapshots, UTF handling, and shared object shapes already landed in newer forms. The native-data arena and exotic sidecar move need independent size/profile evidence and must not be bundled. |
| `src/gc/gc.c`      | **Reject**                                                 | The adaptive rewrite removes the tick/time fallback but still gates pool checks behind nursery conditions and ignores array backing stores. That converts an existing pacing blind spot into potentially unbounded growth. Rebuild policy only after the active array-pacing plan, with plateau and throughput tests.                       |
| `src/gc/objects.c` | **Dependency only**                                        | The PR only marks/frees new cached roots and sidecar state and adds telemetry hooks. Current object-site shape marking is already newer. Revisit this file only with a chosen object-layout or telemetry change.                                                                                                                            |
| `src/gc/stats.c`   | **Port separately, optional**                              | The JSON-at-exit diagnostics can be recreated against the current collector, but the file is observability rather than throughput. Preserve zero-cost disabled paths and validate Windows clocks/output before landing.                                                                                                                     |
| `src/main.c`       | **Dependency only**                                        | Only exposes `ANT_DEBUG=gc:stats`. Land it with a redesigned telemetry module, not alone.                                                                                                                                                                                                                                                   |
| `src/pool.c`       | **Reject as policy; dependency for telemetry**             | The PR adds cause tracking around the existing pool-triggered major collection. Do not mix telemetry with the unsafe 1 MiB/adaptive policy. Cause reporting may be added later without changing collection thresholds.                                                                                                                      |
| `src/utf8.c`       | **Superseded**                                             | Current master has stricter WTF-8/UTF-8 export and UTF-16 range handling. Do not restore the PR helper that recognizes a surrogate prefix without fully validating the sequence.                                                                                                                                                            |

### Built-ins and Host Modules

| File                      | Verdict                                              | Evidence and action                                                                                                                                                                                                                                                                                                                                                                                       |
| ------------------------- | ---------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/modules/buffer.c`    | **O(1) unregister and single-byte encoding ported** | `registry_slot` plus swap-remove landed on 2026-08-29. Latin-1/ASCII encode, decode, byte-length, write, and search semantics were recreated against the current UTF-16/WTF-8 APIs rather than copying the stale general encoder rewrite; Ant and Node pass the shared Buffer coverage. Audit any remaining encoding gaps separately.                                                                                                                           |
| `src/modules/builtin.c`   | **Dependency only**                                  | The PR only accounts for/follows exotic sidecar storage. It has no independent optimization.                                                                                                                                                                                                                                                                                                              |
| `src/modules/crypto.c`    | **Port separately**                                  | AES-GCM/HMAC `generateKey` and raw `exportKey`, extractability, usages, and cleansing are compatibility work, not the Elysia path. Current master still creates imported keys as non-extractable and lacks those APIs. Recreate with current WebCrypto validation and promise conventions.                                                                                                                |
| `src/modules/fetch.c`     | **Ported with Headers**                              | Raw HTTP response headers now use the literal-byte append path, avoiding transient JS strings and preserving obs-text bytes.                                                                                                                                                                                                                                                                            |
| `src/modules/headers.c`   | **Ported except pending data**                       | One-allocation entries, the inline first entry, fallible primitives, and ByteString-preserving JS/HTTP boundaries landed with focused and wire-level coverage. Native pending data remains deferred with lazy Response materialization.                                                                                                                                                                     |
| `src/modules/path.c`      | **Port separately after Node differential tests**    | The PR fixes `relative(resolve(from), resolve(to))` behavior and Windows drive-relative/rooted cases. Current master again normalizes the raw inputs instead of resolving them against cwd, so the compatibility issue remains. It is not an Elysia optimization.                                                                                                                                         |
| `src/modules/regex.c`     | **Targeted semantics ported; broad rewrite superseded** | Inherited `exec` lookup, `@@replace` index coercion/clamping, and the false-positive-prone `RGI_Emoji` expansion were fixed on 2026-08-29 using the current lookup, UTF-16, and pattern-translation machinery. The old cache/execution rewrite and split fast path remain superseded; the latter required multiple follow-up fixes and would reintroduce obsolete assumptions.                                |
| `src/modules/request.c`   | **Ported**                                           | Rooted lazy caches for `method` and `url`, slot preallocation on every Request creation path, and redirect invalidation landed on 2026-08-29. A profile-matched request benchmark remains required before making a speed claim.                                                                                                                                                             |
| `src/modules/response.c`  | **Ported in part**                                    | Direct immutable JS string bodies now borrow storage through a rooted owner and explicit ownership state; clone and allocation-pressure tests pass. Lazy Headers and direct init-shape assumptions remain deferred pending profile evidence and a complete fallible caller contract.                                                                                                                        |
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

1. [x] **Request getter cache** — completed 2026-08-29
   - Add the two slots and reserve them at Request creation.
   - Cache `method` and the built `url` string.
   - Validate clone/constructor/server requests and GC rooting.
2. [x] **Allocation-free property iterator** — completed 2026-08-29
   - Put `js`, `obj`, and the index directly in `ant_iter_t`.
   - Verify nested iterators, empty/invalid inputs, symbols, deletion, and GC
     safety across value materialization.
3. [x] **O(1) Buffer registry removal** — completed 2026-08-29
   - Add `registry_slot`; update the swapped last entry on removal.
   - Test first/middle/last removal and failed registration growth.
4. [x] **Headers storage layout** — completed 2026-08-29
   - First make all storage primitives explicitly fallible.
   - Then introduce the compact entry/inline-first-entry representation.
   - Preserve obs-text/ByteString values across JS and HTTP boundaries.
5. **Response slices, not a rewrite**
   - [x] First borrow immutable JS string bodies with an explicit rooted owner
     and ownership state. Completed 2026-08-29.
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
- The first three isolated ports were implemented on 2026-08-29.
- Compact/fallible Headers storage and ByteString-preserving JS/HTTP boundaries
  were implemented on 2026-08-29. Focused Headers, Response-construction, and
  raw wire-byte tests passed.
- Direct JS string Response bodies now borrow rooted immutable storage instead
  of copying it. The original and clone survive allocation pressure in the
  focused Response test, and the data-structure size did not grow.
- `meson compile -C build` passed.
- Focused Request, property-iteration consumer, structured-clone, Buffer,
  ArrayBuffer species, and WTF-8 tests passed.
- The complete spec suite passed after the Headers/Response ports: 3991 tests
  in 101 files, with zero failures.
- The raw ByteString wire test, Response borrowed-body/clone pressure test,
  focused Buffer/fetch tests, regression manifest load, `maid preflight`, and
  `maid knowledge` all passed.
- Latin-1/ASCII Buffer semantics and detached-ArrayBuffer `TypeError` assertions
  pass all 215 focused checks under both Ant and Node.
- Inherited RegExp `exec` overrides and custom `@@replace` result-index
  coercion, clamping, errors, UTF-16 slicing, and representative RGI emoji
  string-property behavior pass all 45 focused checks under both Ant and Node.
- LLVM discarded stale PGO counters for changed functions, so the resulting
  binary is not suitable for a comparative performance claim. No benchmark
  result is recorded here.

## Decision Log

- 2026-08-29: Chose idea-by-idea recreation over branch recovery because the
  PR and master have both diverged materially.
- 2026-08-29: Rejected the PR GC policy as a port source until array backing
  stores participate in pacing.
- 2026-08-29: Rejected the PR dynamic-key IC representation because its cached
  GC key is not traced.
- 2026-08-29: Prioritized Request caching, property-iterator allocation removal,
  Buffer registry removal, then staged Headers/Response work.
- 2026-08-29: Ported and validated the first three isolated changes. Added
  redirect cache invalidation absent from the PR and preserved
  `ArrayBufferData` size on both supported pointer widths.
- 2026-08-29: Ported compact Headers entries without adopting the PR's native
  arena or lazy Response coupling. Made storage failure observable and added
  JS and wire-level ByteString coverage before proceeding to Response work.
- 2026-08-29: Ported only the Response string-body borrowing slice. Deferred
  lazy Headers and init-shape shortcuts because they broaden the fallible API
  and need profile evidence beyond construction-only microbenchmarks.
- 2026-08-29: Recreated the PR's Latin-1/ASCII Buffer semantics with a targeted
  current-tree encoder and portable detached-ArrayBuffer `TypeError` checks;
  did not port the stale generalized encoder rewrite.
- 2026-08-29: Ported only the still-valid RegExp semantic slices: prototype-aware
  `exec` lookup and spec-shaped replacement-index handling. Kept the current
  cache/execution architecture and rejected the old split fast path.
- 2026-08-29: Tightened the existing structural `RGI_Emoji` translation so
  keycap bases cannot match alone and keycap, modifier, tag, presentation, and
  common ZWJ sequences retain support without affecting ordinary regexes.

# Large AST Workload Performance

Status: active
Last reviewed: 2026-08-07
Owner: theMackabu

## Goal

Reduce Ant's execution time and peak memory for large JavaScript parser and
AST-transform workloads without weakening property, string, proxy, accessor,
or garbage-collection semantics.

The motivating repro is:

```sh
./build/ant todo/tests/ytdlp.js
```

The file bundles Meriyah and Astring, parses a large YouTube player source,
walks and transforms its AST, and generates output through approximately 1.4
million small writer calls.

Current local measurements after the rope-generation and JIT follow-up:

- Node: about 0.31s
- Ant before the follow-up: 5.04s
- Ant after the follow-up: 3.00--3.14s with byte-identical output
- Ant with Astring mechanically changed to collect chunks and join once: about
  4.49s, 1.17 GB peak RSS

The target is to improve broad AST/object-heavy workloads. Do not add a
`ytdlp.js`-, Meriyah-, or Astring-specific shortcut.

## Current Findings

- The original minute-plus stall came from flattening the accumulated output
  on every append after `ROPE_FLATTEN_THRESHOLD` reached 512 KiB.
- Removing the total-length flatten condition makes the exact repro complete
  correctly in about five seconds. The output matches Node byte-for-byte.
- Removing depth-triggered flattening, reverse iterative flattening, iterative
  rope marking, and a young rope pool reduce the same pinned workload from
  5.04s to 3.13/3.14s. The per-isolate mark-state follow-up holds at
  3.00/3.04s and removes process-global GC state.
- The remaining gap is not primarily string copying. The chunked-writer control
  bounds the remaining accumulation opportunity at roughly 0.9s and 300 MB for
  this workload.
- A three-second sample of the post-fix run shows the largest native buckets in
  static property access, property writes, shape lookup/transitions, GC
  scanning/marking, VM/call overhead, and MIR compilation.
- Frequently sampled property functions include `js_try_get`, `js_setprop`,
  `jit_helper_get_field`, `js_getprop_fallback`,
  `ant_shape_add_interned_tr`, `mkprop_interned_exact`, and
  `ant_shape_lookup_interned`.
- Frequently sampled GC functions include `gc_scan_obj`, `gc_objects_run`,
  `gc_objects_run_minor`, `gc_mark_str`, and `gc_strings_sweep`.
- MIR compilation appears in `process_bb_ranges`, `reg_alloc`,
  `generate_func_code`, `build_ssa`, and liveness helpers. Some bundled parser
  functions have hundreds or thousands of locals, so compilation cost and
  expected payback need explicit measurement.

## Scope

- Static `GET_FIELD` / `PUT_FIELD` inline caches and JIT helpers
- Shape lookup, transition reuse, and atom-identity handling
- Object, string, and rope allocation interaction with GC scheduling
- JIT admission policy for very large property-heavy functions
- Focused correctness tests, counters, and benchmarks under `tests/`

Computed `GET_ELEM` / `PUT_ELEM`, proxy-heavy numeric properties, and indexed
plain-object storage remain owned by
[Dynamic Property Performance](dynamic-property-perf.md). Recursive call ABI
work remains owned by
[Silver Recursive JIT Performance](silver-recursive-jit-perf.md).

## Constraints

- Preserve getters, setters, proxies, prototype-chain lookup, descriptors,
  property deletion, and shape invalidation semantics.
- Preserve immutable JavaScript string snapshots.
- Keep profiling counters behind an existing debug/statistics surface or a
  build-time diagnostic guard; do not add unconditional hot-path output.
- Treat wall-clock results as benchmarks, not normal test assertions.
- Do not implement strict persistent AVL rope balancing without allocation and
  GC measurements. Rebuilding a path on every append may allocate more nodes
  than the current one-node concat path.
- Do not pursue a property builder until its materialization and escape
  barriers are enumerated and tested.

## Task List

1. Land and baseline the immediate rope fix. **Complete.**
   - `ROPE_MAX_DEPTH` and depth-triggered flattening are removed. Flattening
     and GC traversal are iterative, so depth is no longer a C-stack limit.
   - Remove total-length-triggered flattening and its obsolete macro.
   - Keep the focused large `this.output += chunk` correctness regression.
   - Record plain and `ANT_DEBUG='dump/vm:op-warn'` timings for the exact repro.
   - Finish the engine-level validation recommended by `maid preflight`.

2. Add measurement counters before changing property caches.
   - Count `GET_FIELD` and `PUT_FIELD` IC hits, misses, invalidations, and
     fallback reasons by bytecode site.
   - Separate monomorphic, polymorphic, and megamorphic receiver sites.
   - Count own-property hits, prototype hits, accessor paths, and nullish
     failures.
   - Report the hottest miss sites without printing on every access.
   - Re-run `ytdlp.js` and a smaller checked-in AST-shaped benchmark.

3. Add a small polymorphic inline cache for static property reads.
   - Start with two to four `(receiver shape, holder/slot, guard)` entries for
     `GET_FIELD`.
   - Preserve prototype epochs and existing invalidation rules.
   - Fall back cheaply after a site becomes megamorphic.
   - Cover alternating AST node shapes, inherited properties, accessors,
     deletion, prototype mutation, and receiver shape changes.
   - Measure hit rate and end-to-end time before extending the design to
     `PUT_FIELD`.

4. Measure and improve shape-transition reuse.
   - Count transition lookups, hits, misses, allocations, byte comparisons,
     and entries lost or invalidated across GC.
   - Verify repeated AST object layouts reuse the same transition chains.
   - Prefer atom identity or stable interned-key identity over repeated byte
     comparison where semantics permit it.
   - Check whether property flags or construction paths create avoidable shape
     variants.
   - Benchmark object-literal and incremental AST-node construction separately.

5. Reduce GC work caused by AST and rope allocation pressure. **Complete for
   ropes.**
   - Measure collections by trigger source: object arena, string pool, rope
     pool, external allocation, and explicit pressure.
   - Record live/reclaimed bytes and scan counts for minor and major cycles.
   - Verify rope-pool growth does not cause unnecessary full object scans.
   - Evaluate nursery sizing only after recording survival rates; avoid trading
     an unmeasured RSS increase for a small timing improvement.
   - Rope nodes now allocate into a per-isolate 8 MiB young pool. Minor GC
     traces young ropes from roots, remembered objects, and mutable builders;
     live blocks promote and dead blocks recycle without a full object major.
   - Rope marking is iterative and all mark-table state is per isolate. If
     reserving the complete mark table fails, collection returns before any
     marking or sweeping begins, conservatively retaining every rope block.

6. Reduce accumulation allocations only if it remains material after property
   and GC work. **Complete for the measured append path.**
   - Track rope nodes allocated, maximum depth, flatten count, and bytes copied.
   - Prototype right-tail chunk aggregation or another allocation-conscious
     immutable representation.
   - Do not rely on `is_flat(left) && is_flat(right)` alone; repeated appends
     quickly make the left operand a rope and bypass that condition.
   - Compare against a guarded property-builder prototype only after auditing
     reads through fields, elements, descriptors, spreads, serialization, and
     native helpers.
   - Require wins beyond the measured chunked-writer ceiling before accepting
     substantial complexity.
   - Typed string `ADD` now bump-allocates a rope directly in JIT code, and a
     monomorphic guarded `PUT_FIELD` path writes the cached own slot directly.
     The primitives compose; a separate fused sequence showed no measurable
     gain and was not added.
   - Concatenations shorter than 13 bytes copy flat strings directly; 13 bytes
     and above use ropes, matching V8's short-cons policy.

7. Add a JIT admission experiment for oversized functions.
   - Record compilation time, bytecode size, local count, basic-block count,
     helper density, call count, and time spent executing compiled code.
   - Test size/complexity budgets that delay or skip functions unlikely to
     repay MIR compilation cost.
   - Compare total execution with current behavior; compilation time saved is
     not a win if interpreter execution loses more.
   - Preserve existing JIT correctness and bailout behavior.

8. Revisit call overhead only with fresh evidence.
   - Attribute time to frame staging, closure entry, generic call planning, and
     helper calls after property and GC improvements land.
   - Reuse compatible work from the recursive-call plan rather than building a
     parallel call ABI.
   - Consider monomorphic direct-call paths or small-function inlining only if
     they remain top sampled costs.

9. Validate each optimization independently.
   - Build with `meson compile -C build`.
   - Run the focused regression and the checked-in AST benchmark.
   - Compare exact `ytdlp.js` output with Node.
   - Record median timing, peak RSS, relevant counters, and a post-change
     sample.
   - Run `maid preflight` and all recommended focused validation.
   - Run `./build/ant examples/spec/run.js --all` for changes to general
     property, string, GC, or JIT semantics, or record the concrete reason a
     narrower run is sufficient.

## Measurement Table

Update this table after each accepted change. Use multiple runs for timing
decisions and keep raw one-off samples out of performance claims.

| Variant                    |     Time |                Peak RSS | Output                        |
| -------------------------- | -------: | ----------------------: | ----------------------------- |
| Node baseline              |   ~0.31s |            not recorded | reference                     |
| Ant before rope fix        | over 60s | over 2 GB while stalled | incomplete during observation |
| Ant after rope fix         |   ~5.37s |                ~1.47 GB | matches Node                  |
| Ant chunked-writer control |   ~4.49s |                ~1.17 GB | matches Node                  |
| Pinned pre-follow-up Ant   |    5.04s |            not recorded | matches Node                  |
| Final rope/JIT follow-up   | 3.13/3.14s |          not recorded | matches Node                  |

## Decision Log

- 2026-07-10: Treat removal of size-triggered rope flattening as the emergency
  fix. It changes the exact repro from a minute-plus stall to a correct result
  in about five seconds.
- 2026-07-10: Do not attribute the remaining gap primarily to ropes. The
  chunked-writer control leaves about 4.49s of execution and more than 1 GB of
  peak RSS.
- 2026-07-10: Prioritize static polymorphic property IC measurement, shape
  transition reuse, and GC trigger attribution before further rope redesign.
- 2026-07-10: Keep strict immutable AVL balancing out of the immediate speed
  path because persistent rotations may multiply node allocations per append.
- 2026-07-10: Keep this plan separate from computed-property/proxy performance;
  the motivating AST workload is dominated by static named fields.
- 2026-08-07: Accept reverse iterative flattening, iterative marking, the young
  rope pool, direct typed-string JIT allocation, and guarded monomorphic
  `PUT_FIELD`. Together they improve pinned `ytdlp.js` by about 37.8%.
- 2026-08-07: Do not add a fused `GET_FIELD + ADD + PUT_FIELD` opcode. The
  primitives already compose and inline-emitter variants were flat.

## Validation Status

- `meson compile -C build` and `maid preflight`: passed.
- `./build/ant tests/test_rope_large_property_concat.cjs`: passed.
- Pinned AB/BA `todo/tests/ytdlp.js`: 5.04/5.04s before versus 3.14/3.13s
  after; output checksum identical in all four runs.
- Per-isolate mark-state AB/BA: global state 3.04/3.07s versus per-isolate
  3.00/3.04s; output checksum identical.
- Spec 3713/0, JIT 9/0, harness 175/0, devirt fuzz all seeds agree.
- Concurrent four-worker rope-nursery regression passed.
- Newt Main 41.516s, wall 41.87s, maximum RSS 779,829,248 bytes.

## Stop Conditions

- Do not extend a monomorphic cache to a PIC unless counters show meaningful
  polymorphic misses at stable sites.
- Do not keep a shape optimization that reduces one helper's samples without
  improving end-to-end median time.
- Do not tune GC thresholds without recording survival, reclaimed bytes, pause
  time, and peak RSS.
- Stop rope optimization for this repro when results approach the measured
  chunked-writer control unless a new profile still shows rope work as a top
  cost.
- Re-profile after every major track; do not add estimated wins together.

## Follow-Ups

- Add a checked-in, license-safe AST-shaped benchmark that does not depend on
  the scratch `todo/tests/ytdlp.js` input.
- Decide whether IC/shape counters belong under `ANT_DEBUG`, `process.stats()`,
  or a benchmark-only build option.
- Record Node and Ant peak RSS using the same measurement method.
- Revisit builder compaction after materialization separately if native builder
  chunk retention appears in a future heap profile.

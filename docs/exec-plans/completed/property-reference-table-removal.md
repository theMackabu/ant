# Property Reference Table Removal

Status: completed
Last reviewed: 2026-07-30
Owner: theMackabu

## Goal

Stop repeated property stores to a long-lived object from growing RSS without bound.

Started as "find the leak in `events.on()`"; the leak turned out to be a symptom of the
engine's property-reference table, and the fix was to delete the table rather than to size
it better.

## Reproduction

Six lines, no allocation, two live objects:

```js
const state = { head: 0 };
for (let i = 0; i < 8_000_000; i++) state.head = i;
```

159 MB before, 26 MB after. It scaled linearly with the number of stores and never
plateaued.

## Root cause

`lkp()` and friends did not return where a property lived. They appended a row to a
per-isolate `js->prop_refs` array — `{obj, slot, valid, invalidates_instanceof}` — and
returned a 1-based index into it. Callers then handed that index straight back to
`propref_load` / `js_saveval`, which looked the row up again.

Three separate costs fell out of that:

1. **A row per store.** `js_setprop` resolves then immediately writes, so a hot store loop
   minted millions of rows all describing the same `(obj, slot)`. 8M stores produced
   7,999,575 rows.
2. **Realloc churn.** `gc_run` zeroed `prop_refs_len` and _then_ called `gc_refs_shrink`,
   which decided based on that just-zeroed length. Its `used >= cap / 4` guard was never
   true, so every collection shrank the table to its 256-entry floor and the workload
   regrew it through ~11 doubling reallocs. The freed multi-megabyte blocks accumulated in
   macOS's allocator cache — `MALLOC_LARGE (empty): 122.2 MB, 20 regions`.
3. **Interpreter mitigation.** `sv_execute_frame` reset `js->prop_refs_len = 0` on six
   different back-edge opcodes to keep the table from exploding inside loops.

The table's design intent was "resolve once, read or write many". By this point no caller
did that. Every consumer was one-shot, and the single caller that _did_ hold a handle
across time — `json.c`'s duplicate-key detection — was broken by it: a mid-parse collection
resets `prop_refs_len`, `propref_get` only bounds-checks and never re-verifies identity, so
`JSON.parse('{"a":1,<120k keys>,"a":2}')` returned `a = 1` instead of `2`.

## Resolution

`lkp`, `lkp_interned`, `lkp_proto`, `lkp_sym` and `lkp_sym_proto` now return the location
itself:

```c
typedef struct {
  ant_object_t *obj;
  uint32_t slot;
} ant_prop_loc_t;
```

Sixteen bytes, returned in two registers under both AAPCS and SysV, so it is cheaper than
the index it replaced while carrying strictly more information. "Not found" is
`loc.obj == NULL`.

Deleted along with the table: `ant_prop_ref_t`, `obj->propref_count`,
`propref_adjust_after_swap_delete` and its O(`prop_refs_len`) scan per `delete`,
`js->prop_refs`/`_len`/`_cap`, `src/gc/refs.c`, `include/gc/refs.h`, the `gc_run`
reset, the six interpreter back-edge resets, and a 512-entry dedup cache that had been
added in front of the table before the table itself was reconsidered.

`invalidates_instanceof` is now recomputed in `prop_loc_store` rather than cached at
lookup time — same frequency as before, and it reads the shape as of the write rather than
as of the lookup.

Public API: `js_propref_load` -> `js_prop_load(ant_prop_loc_t)`, `js_saveval` ->
`js_prop_store(ant_t *, ant_prop_loc_t, ant_value_t)`, and `js_mkprop_fast_off` deleted
(it was already marked `// TODO: deprecate`; its only caller used the handle as a
success flag and now calls `js_mkprop_fast`). This is an ABI break for embedders.

Taken deliberately, with no compatibility shim. A shim is not merely undesirable here, it
is not implementable: the old entry points took a `uint32_t` handle into `js->prop_refs`,
and the whole point of this work was deleting that table. Keeping the signatures alive
would mean keeping the table alive, which is the memory the change exists to reclaim. The
replacements carry an `ant_object_t *` plus a slot, which no handle can be reconstructed
into after the table is gone.

Ant is pre-1.0 and has no published ABI contract, so there is no SONAME to bump and no
supported embedder release to migrate from. Embedders on these two symbols update the call
sites; the mapping above is the whole migration. Revisit this position when a stable
release exists — at that point removals like this one need the versioning and deprecation
window that would be ceremony today.

## Results

Peak RSS, PGO release build, versus `af22111a`:

| workload                      | before   | after   |
| ----------------------------- | -------- | ------- |
| `state.head = i` x8M          | 159.1 MB | 26.1 MB |
| two stores x5M                | 190.4 MB | 28.9 MB |
| `events.on()` x400k           | 98.1 MB  | 24.5 MB |
| `this.count++` via method x5M | 10.6 MB  | 10.4 MB |

Throughput, best of five:

| workload                       | PGO   | no-PGO |
| ------------------------------ | ----- | ------ |
| store/load through `lkp` x20M  | -6.5% | -8.2%  |
| dynamic string keys            | -0.7% | -0.7%  |
| delete churn                   | ~0%   | -0.2%  |
| V8 suite geomean (median of 3) | -0.3% | -0.3%  |

The V8 suite is JIT-dominated and never reaches `lkp` — `sample` on the prototype-lookup
microbenchmark shows the time entirely inside `sv_execute_frame` with no `lkp*` frame at
all. That benchmark swings +5% without PGO and -9% with it, which is code-layout
sensitivity in `sv_execute_frame` from removing the six back-edge stores, not a property
cost. Measurements were taken against a _stale_ PGO profile for every touched function, so
the throughput figures are a floor.

## Safety

The `valid` flag existed so a handle held across a `delete` would be invalidated rather
than silently renumbered by the swap-delete. Removing it is safe because:

- The GC never marked `prop_refs` as roots — it only zeroed the length. A handle's
  `ref->obj` was already an unrooted raw pointer, so a location is no less safe.
- A handle held across a collection was _already_ silently wrong: `propref_get` would find
  it out of bounds, `propref_load` returned `undefined` and `js_saveval` tripped an assert
  that is compiled out under `NDEBUG`. That is the json.c bug.
- Only user JS can `delete`, and only a reentrant call can run user JS. An audit of every
  converted site found zero locations that survive one: each candidate is either in a
  mutually exclusive branch or every reentrant path `return`s before the location is used.

Changing the type from an integer to a struct meant the compiler had to visit every call
site — `off != 0` does not compile on a struct — so no site could be missed silently.

## Validation

- Harness 152 passed / 0 failed; conformance 1511 / 1511.
- A 45,005-check stress script covering delete-driven slot renumbering, a setter that
  deletes a sibling mid-assignment, prototype-store instanceof invalidation, proxies over
  mutated targets, frozen enforcement, and JSON duplicate keys across a forced collection:
  identical results on node v26.5.1, `af22111a`, and this build.

## Property order after delete

Removing the table left `ant_shape_remove_slot`'s `swapped_from` out-param with no
consumers, which made a second, pre-existing bug cheap to fix.

Deleting a property swapped the last slot into the hole to keep `shape->props` dense. Slot
order is property order, so any delete other than of the final property observably
reordered the object: `{a,b,c,d}` minus `b` enumerated as `a,d,c` where node gives
`a,c,d`. That violates OrdinaryOwnPropertyKeys, which requires string keys in insertion
order, and it was visible through `Object.keys`, `Object.entries`, `for...in`,
`JSON.stringify` and spread.

Slots now close up instead: `memmove` the tail down one, then decrement every index entry
above the hole. `obj_remove_prop_slot` shifts the values to match. Delete becomes O(n) in
the object's property count rather than O(1), which is the standard trade for ordering and
is unmeasurable on the delete-churn benchmark (-0.2%).

Verified with a 4000-case differential fuzz of random insert / delete / re-add /
`defineProperty` sequences — key order and `JSON.stringify` output hash identically to
node v26.5.1.

## Follow-ups

- Regenerate `meson/pgo/profiles/ant-darwin-aarch64.profdata`; it is stale for every
  function touched here.

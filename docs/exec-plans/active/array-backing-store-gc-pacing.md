# Array Backing Store GC Pacing

Status: active
Last reviewed: 2026-07-31
Owner: theMackabu
Handoff to: PR #44 (`perf/silver-jit-elysia-parity`)
Depends on: PR #66 must land first

## Dependency

PR #66 lands the byte counter this plan builds on: `js->alloc_bytes.arrays`, a maintained
live-byte total for array backing stores. **Rebase on PR #66 before starting.** If
`js->alloc_bytes.arrays` does not exist in `include/internal.h`, you have the wrong base;
see "What PR #66 already landed" below for the exact 4-file change to recreate.

The split is deliberate. The counter alone changes no behaviour - it reports the same
numbers the old arena walk did - so it went in with the PR #66 review fixes. Everything
that changes collector behaviour is in this document and belongs on PR #44.

## Goal

Make the collector see array backing stores. Today a JS array's elements live in a
`malloc`'d block that no GC pacing signal accounts for, so a program that allocates a
small number of objects, each owning a large array, grows without bound.

The byte accounting for this landed separately on PR #66. This plan covers only the
pacing half, which belongs with the rest of the GC work on PR #44.

## Background

An array's elements are stored in `obj->u.array.data`, allocated with plain `malloc` in
two places:

- `src/ant.c` `alloc_array_with_proto()` - initial `MAX_DENSE_INITIAL_CAP` block
- `src/ant.c` `dense_grow()` - `realloc` on growth, doubling `cap`

and freed in one place:

- `src/gc/objects.c` `obj_free()` - `free(obj->u.array.data)`

`gc_maybe()` paces on two signals, and neither one sees these bytes:

1. `js->obj_arena.live_count` - arena object count. An array is one object no matter how
   large its backing store is.
2. `js->gc_pool_alloc` - bytes handed out by `js_type_alloc()`. Arrays never call it.

So 2000 arrays of 20000 elements are 2000 objects and 0 pool bytes to the collector,
while being 320 MB of resident memory.

## What PR #44 already does, and what it does not

PR #44 rewrites much of `src/gc/gc.c`: it adds `gc/stats.{h,c}` telemetry, adds
`gc_remember_ewma` / `gc_promoted_ewma`, reworks `gc_adapt_nursery()` around remembered
set pressure, and adds saturating `gc_grow_toward()` / `gc_shrink_toward()` helpers.

It does **not** touch array backing store accounting. `gc_maybe()` keeps its existing
shape: pool pressure is consulted only *inside* the `young_count >= gc_nursery_threshold`
branch, and only *behind* the `js->minor_gc_count >= gc_major_every_n` gate. An array-heavy
workload never enters that branch, so it never reaches the pool check.

PR #44 also **deletes** the tick-based fallback at the end of `gc_maybe()`:

```c
-  if (gc_tick < 8192) return;
-  if (young_count == 0 && js->gc_pool_alloc == 0) { gc_tick = 0; return; }
-  if (gc_now_ms() - gc_last_run_ms < GC_FORCE_INTERVAL_MS) { gc_tick = 0; return; }
-  gc_tick = 0;
-  gc_run(js);
```

That path was the only thing that eventually caught this workload on master. Removing it
makes the gap unbounded rather than merely slow: if `young_count` never reaches the
nursery threshold and `live` never reaches the live threshold, `gc_maybe()` now returns
without doing anything, regardless of how many bytes of array data exist.

This is not a regression introduced by PR #44 - the underlying blindness predates it - but
PR #44 removes the safety net, so the two should be resolved together.

## Measurements

All numbers from `/tmp/arrplateau.mjs`: ten rounds, each allocating 2000 arrays of 20000
elements and retaining none. The live set is O(1), so RSS must plateau.

`arrays` is `Ant.stats().alloc.arrays`, the live byte count.

| | round 1 | round 4 | round 8 | max RSS |
|---|---|---|---|---|
| master | 524 MB | 2097 MB | 4194 MB | 1890 MB |
| accounting only | 0.5 MB | 0.8 MB | 0.8 MB | 890 MB |
| accounting + pressure entry | 0.5 MB | 0.8 MB | 0.8 MB | 636 MB |

Cost, from `/tmp/arrbench.mjs` (churn / retain / grow):

| | churn | retain | grow |
|---|---|---|---|
| master | 465 ms | 49 ms | 25 ms |
| accounting + pressure entry | 454 ms | 59 ms | 23 ms |

The retain delta is within run-to-run noise on this machine; re-measure before trusting it.

## The trap: an earlier attempt measured as a 44% regression

An earlier version of this work charged array growth to `js->gc_pool_alloc` **without**
adding it to `gc_pool_live_bytes()`. That mismatches the units on the two sides of the
comparison: `gc_pool_alloc` carried array bytes while `gc_pool_last_live` (its threshold
basis, via `gc_pool_major_threshold()`) did not. The threshold stayed at the 8 MB
`GC_POOL_PRESSURE_FLOOR` while the counter climbed at array speed, so majors fired
continuously and the retain case regressed 44%.

If you charge arrays to `gc_pool_alloc`, you **must** also add them to
`gc_pool_live_bytes()`. Both sides or neither.

## Task list

1. Charge array bytes to `js->gc_pool_alloc` in `dense_grow()` and
   `alloc_array_with_proto()`, using the same growth delta already computed for
   `js->alloc_bytes.arrays`.
2. Add `js->alloc_bytes.arrays` to `gc_pool_live_bytes()` so the threshold basis matches
   the counter. See the trap above.
3. Let pool pressure start a collection on its own. Do **not** add a second decision site
   with an early `return`: that skips `gc_run_minor()`, so `gc_adapt_nursery()` never runs
   and `gc_minor_surv_ewma` freezes at a stale value while pressure keeps firing majors.
   PR #44 makes this worse, since `gc_remember_ewma` and `gc_promoted_ewma` are also
   updated only from `gc_run_minor()`. Fold it into the existing branch instead:

   ```c
   bool pool_pressure = js->gc_pool_alloc >= gc_pool_major_threshold(js);

   if (young_count >= gc_nursery_threshold || pool_pressure) {
     ...
     gc_run_minor(js);

     // gc_run_minor leaves gc_pool_alloc alone, so pool_pressure still holds here
     bool major_due = pool_pressure ||
       (js->minor_gc_count >= gc_major_every_n && live_before_minor >= major_threshold);
     ...
   }
   ```

   This collapses the old nested form to one decision site and is behaviour-preserving
   when `pool_pressure` is false. Note the old inner `js->gc_pool_alloc >= pool_threshold`
   term is exactly `pool_pressure`, because `gc_run_minor()` does not reset
   `gc_pool_alloc` - only `gc_run()` does.
4. Decide what replaces the deleted tick fallback, if anything. With steps 1-3 the
   array case is covered, but the fallback also caught slow leaks in other subsystems.
5. Re-validate `Ant.stats().alloc.arrays` against a direct arena walk after any change to
   the free path. See "Validation" below.

## Do not do this

Do not put the pressure check at the allocation site the way `src/pool.c`
`js_type_alloc()` does it:

```c
js->gc_pool_alloc += size;
size_t pool_threshold = gc_pool_major_threshold(js);
if (js->gc_pool_alloc >= pool_threshold) gc_run(js);   // safe HERE only
```

That is safe in `js_type_alloc()` because the collection happens *before* any memory is
handed out, with nothing in flight. `dense_grow()` has no equivalent window:

```c
ant_value_t *next = realloc(obj->u.array.data, sizeof(*next) * (size_t)new_cap);
if (!next) return 0;
// a gc_run() HERE is a use-after-free: realloc already released the old block, so
// obj->u.array.data dangles while obj->u.array.cap still reads old_cap, and the mark
// path at src/gc/objects.c:383 walks data[0 .. min(len, cap)]
for (ant_offset_t i = old_cap; i < new_cap; i++) next[i] = T_EMPTY;
obj->u.array.data = next;
obj->u.array.cap = (uint32_t)new_cap;
```

Moving the check *above* the `realloc` trades that for a different bug: `obj` is derived
from `arr` at function entry, so a `gc_run()` there can sweep it. You would have to
re-derive `obj` after the collection and prove `arr` is rooted at all nine `dense_grow()`
call sites (`src/ant.c` lines 2778, 6216, 10002, 10033, 11386, 11641, 15091, 15151,
15351). `alloc_array_with_proto()` has the same hazard in reverse: it mallocs after
`obj_alloc()` but before the new object is reachable, so a collection there frees the
object being built.

`gc_maybe()` is called at safepoints. That is the reason to put the decision there and
not at the allocation site, even though the allocation site reacts sooner.

## Already landed on PR #66

The byte accounting is done and does not need repeating. `js->alloc_bytes.arrays` is a
maintained live-byte counter:

- `include/internal.h` - `size_t arrays;` added to `js->alloc_bytes`
- `src/ant.c` `dense_grow()` - `+= (new_cap - old_cap) * sizeof(ant_value_t)`
- `src/ant.c` `alloc_array_with_proto()` - `+= cap * sizeof(ant_value_t)`
- `src/gc/objects.c` `obj_free()` - saturating `-= cap * sizeof(ant_value_t)`
- `src/modules/builtin.c` - `Ant.stats().alloc.arrays` now reads the counter instead of
  walking the arena, so it is O(1)

The subtraction saturates on purpose. If the count ever drifts, a `size_t` underflow turns
`alloc_bytes.arrays` into a nonsense multi-exabyte figure, which is far worse for anything
reading it than a count that reads low. Once step 2 lands, that figure would also feed the
pool threshold and stop the collector outright.

Note that `alloc_bytes.closures` and `alloc_bytes.upvalues` are read by
`src/modules/builtin.c` and `src/modules/v8.c` but are **never incremented anywhere**.
They always report 0. Unrelated to this plan, but worth fixing while in the area.

## Validation

`Ant.stats().alloc.arrays` was verified exact against a direct arena walk across
allocation, `realloc` growth, sparse conversion, nested arrays, and post-sweep states -
zero drift at every observation point. If you change the free path, re-run that check by
temporarily re-adding the walk alongside the counter:

```c
// in src/modules/builtin.c, inside the existing 3-pass object loop
if (obj->type_tag == T_ARR && obj->u.array.data)
  array_bytes += obj->u.array.cap * sizeof(ant_value_t);
...
js_set(js, alloc, "arraysCounter", js_mknum((double)js->alloc_bytes.arrays));
```

then assert `arraysCounter === arrays` at each step. There is no exposed `Ant.gc()` or
`--expose-gc`, so drive collections by churning small objects rather than forcing one.

## Decision log

- 2026-07-31: Kept the byte accounting on PR #66, moved the pacing change to PR #44.
  The accounting is a self-contained stats fix with no policy effect; the pacing change
  overlaps PR #44's `gc_maybe()` and `gc_adapt_nursery()` rewrite and would conflict.
- 2026-07-31: Rejected a top-level `if (pool_pressure) { gc_run(js); return; }` in
  `gc_maybe()`. It duplicates the existing pool decision with different gating and
  early-returns past `gc_run_minor()`, starving the EWMA feedback loops.
- 2026-07-31: Rejected the `js_type_alloc()`-style eager check inside `dense_grow()`.
  See "Do not do this".

## Follow-ups

- `alloc_bytes.closures` and `alloc_bytes.upvalues` are never incremented.
- Typed arrays and `ArrayBuffer` backing stores may have the same blindness. PR #44's
  notes mention typed-array churn RSS separately (36.7 MB to 19.9 MB), so check whether
  `create_array_buffer_data()` is already accounted for before assuming it is not.
- Even with pacing fixed, RSS plateaus near 636 MB on the churn test while the live set is
  under 1 MB. That is allocator retention of freed 160 KB blocks, not a collector problem.
  Worth a separate look at whether large backing stores should bypass malloc.

# Card Marking for Old Arrays

Status: active (phases 1-5 done and measured against master on
`gc/nursery-policy-card-marking`; see the checkpoint)
Last reviewed: 2026-09-30
Owner: theMackabu

## Current checkpoint (2026-09-28)

Implemented on top of `55b04f85`:

- `gc_card_table_t` in `include/gc/objects.h`: 128-slot cards, tables only for
  arrays of at least 1024 slots, kept in the object sidecar
  (`ant_object_sidecar_t.gc_cards`), freed with the sidecar.
- `dense_set` calls `gc_write_barrier_elem`, which sets the slot's card and
  remembers the array once. It returns early for arrays too small for cards
  that are already remembered; a first version without that early return cost
  2-5% on unrelated array-heavy cases.
- `gc_remember_add` marks an array's table `all_dirty`, so every store that
  doesn't use the element barrier falls back to a full element scan. A table
  created for an array already remembered this cycle starts `all_dirty`.
- `gc_elements_moved` marks a remembered array `all_dirty` after the
  `memmove`s in `shift`, `unshift` and `splice` and after the stream queue
  shifts. Moves don't get precise card ranges yet.
- `gc_scan_obj` scans only dirty cards of a card-marked array during a minor.
  Both collections clear the cards of every remembered object.
- `gc_verify_cards` (`src/gc/verify.c`, verify builds only) runs before each
  minor and aborts if a young reference sits in a clean card. It judges
  closures by their own `generation`, because `gc_value_ref_is_young` treats
  every function value as young.

Collection policy, changed in the same step: the byte-based nursery and major
triggers of `55b04f85` and its nursery growth (up to 2.5x) are reverted to
master's object-count triggers with a fixed nursery. Measured against master,
growth cost edge mutations +17 ms and captured closures +36 MB, and the byte
triggers moved major collections enough to raise peak memory on medium
survival and edge mutations. Card marking alone keeps most of the
high-survival gain. Kept from `55b04f85`: no wall-clock collections, the idle
hook before the event loop blocks, and a precomputed allocation limit
(`gc_refresh_alloc_limit`), which makes the per-allocation check a compare.

Update (2026-09-29): major growth now needs two signals to agree
(`gc_adapt_major_interval`). The old generation grows only when majors take
more than about 10% of wall time and a major marks more than 800 bytes per
1024 the program allocated since the previous one
(`GC_MAJOR_WORK_SHARE_HIGH`; allocation is summed at collections, so
allocation pays nothing). Shrinking still follows the time share alone.

The time share alone grew heaps whose majors were costly for reasons growth
doesn't fix: on medium-survival it grew to 3.25x live to save one major of
13, peaking at 63 MB. The work share alone ignores what marking costs. With
both, medium-survival peaks at 48 MB and batch-retention at 94 MB (from 120)
for one and three more majors, and every other fixture keeps its schedule.

Measured and rejected on the way:

- Byte-based triggers (arena slots plus array storage): +17 MB on
  old-graph-mutations, +2 MB on json-roundtrip and +6% time on
  old-graph-large, under every growth rule tried. The object-count triggers
  stay.
- The work share alone (deterministic, no clock): lowest memory on several
  fixtures, but it grows heaps like Splay's where marking is cheap relative
  to the program, and its single threshold suits no workload mix.
- Thread CPU time instead of wall time: more growth (the program thread's
  CPU time runs behind wall time while it waits on memory and on JIT
  threads), and wall time was already stable under load in a test with 18
  competing processes.

bench-v8's Splay is scored over a fixed time, so its score moves by +-10%
between runs of the same binary; compare its GC counts instead. With the
combined rule it runs about one more major per run (under 1% of its time)
and peaks lower.

Validation: `tests/test_gc_array_cards.c` (native) and
`tests/test_gc_array_cards.cjs` (harness `tests` group); 13/13 native GC/JIT
tests; release harness; `ANT_GC_STRESS` 97 and 211 with the verifier;
`--jitless` runs of the new test. A deliberately dropped card bit makes the
verifier abort.

Measured 2026-09-28 against master (`9d61d90d`), both with PGO trained on
their own code; binaries and runners in `.cache/bench-20260928/`:

- gc-patterns fixtures: `high-survival` 173.7 -> 116.4 ms, `medium-survival`
  107.0 -> 95.3 ms, `captured-closures` 108.9 -> 99.0 ms, `batch-retention`
  168.5 -> 160.1 ms; peak memory level or lower on every case; the other cases
  within noise. Fixture total 1,348.6 -> 1,256.3 ms.
- Fixed-work Splay, Newt Main and Prelude, ytdlp, the SSR bench and the repo
  GC benches: within noise or faster, identical output.
- Against the gc-fixed report's A/B (scaled onto this machine),
  `high-survival` goes from about +73% to about +16%. The rest is promotion:
  the fixed 32,768-object nursery still promotes everything in that workload.

Update (2026-09-30), from review:

- Array storage lives outside the object arena, so the object-count triggers
  never saw it: 1,000 iterations of `new Array(131072).fill(i)` peaked at
  1,013 MB, on master (`9d61d90d`) as well. Growing array storage now makes
  the next allocation check decide on a collection once storage allocated
  since the last collection reaches the heap's size, and at least
  `GC_ARRAY_GROWTH_FLOOR` (16 MiB); a major is due when array storage has
  grown by the heap's size since the last major, by the same floor
  (`gc_array_grew`, `gc_decide`). That loop now peaks at 42-51 MB; growth by
  `push`, promoted arrays and `slice` copies are bounded the same way (43-51
  MB, from 413-1,017 MB on master). A first version made a major due when
  array storage doubled since the last major: Splay's live tree grows its
  payload arrays that fast, so it ran extra majors (bench-v8 Splay -20%,
  splay-long 1,000 -> 1,179 ms at half the memory).
- Named stores (`arr.tag = young`) remember the array without dirtying its
  cards (`gc_write_barrier_prop`); only stores that may write dense storage
  without the element barrier still use `gc_remember_add`, which marks the
  table `all_dirty`.
- An idle major runs with a finite budget only once a major's cost is known;
  before the first major it waits for an idle period with nothing scheduled.
  Idle majors fire halfway to the threshold: with Elysia under one
  connection, all 14 majors in 10 s were idle ones (about twice the majors
  per request of 50 connections, where 1 of 38 was idle). Against idle majors
  at 3/4 of the way and no idle majors (idle minors kept), 5 rounds each:
  throughput, p50, p99, p99.9 and p99.99 within noise at both concurrencies;
  the slowest request 1.26 ms (halfway), 1.36 ms (3/4), 1.18 ms (none) and
  1.26 ms on master; server RSS 31/31/33 MB at one connection. The halfway
  rule stays.

Remaining: retest nursery growth now that rescans are
proportional to changes; canonical bench-v8 and elysia on the final builds;
precise move ranges; card size and threshold.

## Problem

The minor collector's remembered set is per object. When an old object stores
a young reference, `gc_write_barrier` (`include/gc.h`) adds the whole object to
`js->remember_set`, and `gc_objects_run_minor` (`src/gc/objects.c`) runs
`gc_scan_obj` on it, which walks every element of an array's dense storage.

For a large old array written a little at a time, every minor rescans the
entire array to find the few slots that changed. Measured on the
`high-survival` case of `.cache/gc-causal-20260921/fixtures/gc-patterns.cjs`
(a 131,072-slot ring array of payloads, about 11k slots rewritten per minor):

- about 75% of minor time is `gc_scan_obj` over the remembered set
  (`.cache/gc-nursery-20260923/hs.sample`);
- each minor costs about 1.24 ms against 0.43 ms for medium survival, about
  6 ns per slot, mostly cache misses on each target's mark field.

The same shape appears wherever an old array acts as a cache, queue or ring
buffer. Growing the nursery hides part of the cost by making minors rarer, but
not the per-minor rescan.

## Goal

A minor's work for a remembered old array should scale with what changed since
the last collection, not with the array's length.

## Scope

In scope: dense element storage (`u.array.data`) of old arrays above a size
threshold.

Out of scope, possibly follow-ups:

- Map and Set hash tables, which have the same whole-object rescan (the
  `growth` bench's remembered Map).
- Objects with many named or overflow properties.
- Regressions that are not remembered-array rescans. The fixture matrix of
  2026-09-23 showed `old-graph-mutations` +8-9% and `old-graph-large` +1-8%
  against master whenever the nursery may grow. Neither is this problem:
  `old-graph-large` does no writes, and `old-graph-mutations` remembers
  small node objects (`graph[j].edge = x`), not a large array. Their cause is
  still unknown and needs its own investigation.

## Current invariants the design depends on

- **Element stores of heap references go through C.** The JIT's inline element
  store (`emit_properties.c`, `mir_emit_dense_element_guard` with
  `JIT_ELEMENT_WRITE`) only writes when both the new value and the old slot
  value are numbers, so it needs no barrier. Every other element store calls
  `jit_helper_put_elem`, which reaches `dense_set` (`src/ant.c`) and then
  `gc_write_barrier`.
- **The JIT's inline barrier is for named properties only.**
  `mir_emit_put_field_barrier` (`src/jit/properties.c`) calls `gc_remember_add`
  directly. It never writes array elements.
- **Direct writers of array storage outside `dense_set`** (as of this review):
  fill loops writing `T_EMPTY` and literal initialization of new, still young
  arrays in `src/ant.c`, which need no card; the element-shifting loops of the
  stream queues (`src/streams/readable.c`, `src/streams/writable.c`), which
  move existing references between slots and do need cards. Phase 1 audits
  every `u.array.data[...] =` and bulk copy again.
- **After a minor, no old-to-young reference remains:** every young survivor is
  promoted. So all cards can be cleared at the end of each minor and each
  major.

## Design

- **Cards.** One bit per 128 slots (1 KiB of storage). Only arrays whose
  capacity is at least a threshold (initially 1024 slots) get a card table.
  Smaller arrays keep whole-object remembering, where a card table saves
  nothing.
- **Where the card table lives.** `u.array` is a full 16-byte union
  (`data`, `len`, `cap`), so there is no spare field. Candidates: the existing
  per-object sidecar (`ant_object_sidecar`), a header in front of the storage
  allocation, or a side table keyed by object. The sidecar is the preferred
  starting point, because it already exists for rarely needed per-object data
  and doesn't change how storage is allocated or freed. Phase 1 decides.
- **Barrier.** Add an element-aware barrier, for example
  `gc_write_barrier_elem(js, obj, idx, val)`, used by `dense_set` and the other
  element writers. For an old array with a card table that stores a young
  reference: set card `idx / 128` and add the object to the remembered set
  once, flagged as card-marked. Otherwise, behave like `gc_write_barrier`
  today. The allocation of the card table happens on the first such store, so
  arrays that are never written with young values never pay for one.
- **Moves.** Any operation that moves references inside an old card-marked
  array (shift, unshift, splice, sort, reverse, copyWithin, the stream queue
  shifts) dirties the cards of every destination slot, or falls back to
  whole-object remembering for that array until the next collection. Missing
  one of these frees a live object, so the fallback is the default for any
  bulk path not yet converted.
- **Growth.** `dense_grow` (`realloc`) resizes the card table and keeps
  existing bits. Array capacity never shrinks, so neither does the table.
- **Minor scan.** For a card-marked remembered array, `gc_scan_obj` scans only
  the slots of dirty cards. Other remembered objects are scanned as today.
  Cards are cleared at the end of the minor.
- **Major.** Marks the whole array as today and clears all cards.
- **JIT.** No code-generation change, as long as the inline element store
  stays numbers-only. Add a native test that fails if an inline element store
  ever writes a heap reference without going through the barrier.

## Audit result (phase 1)

| writer | kind | handling |
| --- | --- | --- |
| `dense_set` (`push`, `reverse`, `sort`, `copyWithin`, `fill`, `splice`/`unshift` inserts, `arr_set`, `js_arr_push`, `jit_helper_put_elem`) | element store | card for `idx` |
| `shift`, `unshift`, `splice` `memmove` through `dense_data` | moves | `all_dirty` if remembered |
| stream queue shifts (`readable.c`, `writable.c`) | moves | `all_dirty` if remembered |
| `T_EMPTY` fills, literal init of new arrays, `pop` | no references or young array | none |
| `dense_grow` `realloc` | resize | table grows on the next store past its end |
| JIT inline element store | numbers only | none |

`sort` copies into a private buffer and writes back through `dense_set`.
No code keeps a local copy of `u.array.data` for writing.

## Phases

1. **Audit and layout.** Enumerate every writer of `u.array.data` and every
   bulk copy into it; classify each as young-only, needs-card, or moves.
   Choose the card-table location and threshold. Update this plan.
2. **Barrier and scan.** Element-aware barrier, card allocation, card-limited
   minor scan, clearing at minor and major, `dense_grow` resize. Unconverted
   bulk paths fall back to whole-object remembering.
3. **Verification mode.** Under `ANT_GC_VERIFY`, before each minor, scan every
   card-marked remembered array in full and assert that every young reference
   lies in a dirty card. Run the stress sweeps with it.
4. **Convert bulk paths** one at a time, each with a test that moves young
   references inside an old array and then forces a minor.
5. **Measure and tune** card size and threshold.

## Validation

- **Correctness:** new native tests (scattered young stores into a large old
  array followed by a minor; each bulk move; growth across card boundaries);
  the full harness; `ANT_GC_STRESS` sweeps at 97 and 211 with the phase-3
  verifier enabled; `--jitless` runs of the same.
- **Performance:** the `gc-patterns` fixture matrix against the pre-change
  binary. `high-survival` minor time should drop sharply; the other cases
  should stay level. Also the GC bench set (`growth`, `bigheap`,
  `array_churn`), bench-v8 and elysia.
- **Nursery cap:** once rescans are proportional to changes, re-test the
  nursery cap. 4x was faster than 2.5x on the fixtures but doubled some peaks
  and added a loss on `old-graph-large`.

## Risks

- **A missed card frees a live object.** The phase-3 verifier and the
  fall-back-by-default rule for bulk paths are the mitigations.
- **The barrier gets slower for every array store.** The card path only runs
  for old arrays storing young references; measure `dense_set`-heavy
  workloads (`array-callbacks`, `json-roundtrip`).
- **The sidecar may not suit a hot path.** If looking up the card table there
  costs too much, a storage header is the alternative.

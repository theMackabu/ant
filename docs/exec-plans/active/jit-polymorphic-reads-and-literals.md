# Polymorphic Property Reads and Object Literal Allocation

Status: active (committed on `gc/nursery-policy-card-marking` through
`0874dbb2`; review fixes of 2026-09-30 uncommitted)
Last reviewed: 2026-09-30
Owner: theMackabu

## Problem

Measured with `xctrace` (`sample`'s top-of-stack summary drops JIT code and
overstated helper shares by 5-10x), the runtime costs outside JIT code on
bench-v8 were:

- The named property read slow path (`jit_get_field_fallback`): 4-8% of
  Richards, DeltaBlue, RayTrace and Splay. Each read site caches one shape.
  Sites that see several classes (Richards' `this.task.run(...)` over four
  task classes, Splay's `left`/`right` read from nodes with and without own
  properties, DeltaBlue's constraint subclasses) missed on almost every call
  and paid a full chain lookup plus a cache rewrite.
- Allocation: an object literal cost about 700 instructions and `new P(a, b)`
  about 1000, against a handful in node. Every literal property was a C call
  (`jit_helper_define_slot`) that re-checked the key against the runtime's
  watched names; every constructor property add called
  `jit_helper_shape_transition`; every `new` hashed `prototype` on the
  constructor.

## Changes

- **Polymorphic read cache** (`sv_gf_poly_t`, `include/silver/engine.h`).
  Named read sites (`OP_GET_FIELD`, `OP_GET_FIELD2`, `OP_GET_FIELD_OPT`) keep
  up to 8 more own or prototype cases in a code-arena block pointed to from
  the unused half of the read cache's guard union. Entries are validated
  exactly like the site's own cache (epoch, shape, receiver prototype and its
  identity, holder, slot key). A lookup that is about to overwrite a current
  case keeps the old case (`sv_gf_poly_keep`); a hit swaps the entry with the
  site's own case (`sv_gf_poly_promote`), so the inline check sees the latest
  case. A site that adds more than 16 cases within one epoch is marked
  `SV_IC_POLY_MEGA`, drops its entries and stops using the cache.
- **Inline check of the other cases** (`jit_emit_get_field_poly`,
  `src/jit/emit_properties.c`): only at sites already polymorphic when
  compiled, only in functions of at most 512 bytecode bytes, and not at sites
  already churning. It loops over the entries at run time, so cases added
  after compiling are covered.
- **Inline object literal stores** (`jit_emit_define_slot_inline`): an
  `OP_DEFINE_SLOT` whose key is not one `ant_property_mutation_invalidate`
  watches, on a young non-exotic object whose shape already has the slot, is a
  store. Functions over 1024 bytecode bytes keep the helper.
- **Inline shape transition** (`mir_emit_shape_transition`,
  `src/jit/properties.c`) for property adds: reference counts adjusted
  inline; the helper still handles a last reference and objects that guard
  absence. `ANT_SHAPE_REF_COUNT_OFFSET` is asserted in `src/shapes.c`.
- **Prototype slot cache for `new`** (`sv_construct_prototype_cached`,
  `include/silver/call.h`): the slot of `prototype` in the constructor's
  function object shape, per function, checked against the current shape's
  property on every use.

- **Direct constructor calls from compiled `new`** (`jit_emit_new_direct`,
  `src/jit/emit_calls.c`). `jit_helper_new_this` allocates `this` when the
  constructor is a plain sync, non-derived closure with compiled code and a
  plain `prototype` data property, called as its own new.target; otherwise it
  returns `T_EMPTY` before anything observable ran and `jit_helper_new` takes
  the call. The constructor's compiled code is then called the way compiled
  calls call compiled code. When it returns undefined and its slot feedback is
  frozen, the value is `this` and the feedback counters are bumped inline;
  otherwise `jit_helper_new_result` picks the value and records. The
  `prototype` slot cache now holds a reference to the function object's shape
  (registered with `sv_ic_shape_ref_register`), which keeps that shape shared,
  so an equal shape is enough to use the cached slot.

Two register-aliasing traps hit while writing these: the value register of a
compiled read or `new` can be the stack register its operand was popped from,
so anything written before the operand's last use must go to a scratch
register (a removed helper-call variant of the read cache clobbered the
receiver this way; `jit_emit_new_direct` copies the constructor first).

## Stage 3 (538e01ed)

- **Megamorphic reads.** A read site that keeps needing new cases shares an
  isolate-wide cache keyed by receiver shape, receiver prototype and key
  (`sv_gf_mega_cache_t`): 4096 primary and 1024 secondary entries, a primary
  insert moving the evicted entry to the secondary table, as V8's stub cache
  does. Entries reference their shapes. Compiled code probes the primary table
  inline at megamorphic sites (`jit_emit_get_field_mega_probe`), without a
  loop. A function whose site went megamorphic after compiling is recompiled
  once, after 256 slow-path reads there, at most twice per function.
- **Same shape, different prototype** now counts as a new case. RayTrace's
  `Class.create` wrapper reads `this.initialize` on fresh objects of 14
  classes that share one shape; that one site was 13.9 of 17.4 million
  slow-path reads.
- **Not done, measured worse:** shared-cache probes in functions over 512
  bytecode bytes (+20-30 MB compiler memory for `rayTrace`), and recompiling
  for merely polymorphic sites (more compiles; the inline loop can't reorder
  cases, so a site whose own case is the rare one pays on every read, where
  the slow path's promotion makes the common case hit inline).
- **Stores.** Compiled stores of a reference into an old object used to leave
  compiled code for the write barrier. The value check now sets the barrier
  (`need_barrier`) when the value is young, as `gc_write_barrier` decides, and
  the existing inline barrier remembers the object. Splay's tree links are
  exactly this. Store sites also keep other cases in C (`sv_pf_poly_t`,
  existing-property stores and adds, promoted into the site's own cache on a
  hit).

Stage 3 results (PGO, against master): RayTrace +14-16% on bench-v8, the rest
level with the stage-2 build; tests `test_poly_field_store.cjs` and
`test_jit_store_barrier.cjs` (the latter fails without the inline barrier).

## Review fixes (2026-09-30)

- **Adds depend on the prototype.** Whether `o.y = v` adds an own property
  depends on the receiver's prototype chain (an inherited setter runs
  instead, an inherited read-only property rejects it), and objects of one
  shape can have different prototypes (`Object.create`). Add cases now record
  a key for the prototype (`sv_add_proto_key`, in `sv_ic_entry_t.add_proto`
  and `sv_pf_poly_entry_t.proto`). For an old prototype the key is its value:
  old objects are freed only by majors, whose epoch bump retires every add
  case, so compiled code checks it with one load and compare
  (`mir_emit_put_field_add_guard`). A young prototype could be freed by a
  minor and its address reused, so its key is its identity tagged odd, which
  compiled code never matches; C checks it and rekeys the case by value once
  the prototype is old. No case is kept under a null prototype. A first
  version compared identities inline (decode the prototype, load its
  identity): RayTrace +2.7% and EarleyBoyer +2.0% instructions. Recording
  only old prototypes sent every add to the slow path until the first minor
  (32K slow adds in a constructor loop).
- **Strictness in compiled code.** Compiled functions push no VM frame, so a
  failed store or delete took its strictness from the interpreted frame below
  it: strict compiled code ignored failed stores, and sloppy compiled code
  called from strict code threw. This predates the branch (the installed
  binary does the same). The named store helper takes the compiled (or
  inlined) function's mode as an argument and records it against the current
  frame only on a miss, since a cached case can't fail on strictness; the
  element store and delete helpers have strict and sloppy entries picked when
  compiling (`sv_vm_t.jit_mode_fp`, `sv_vm_is_strict`).
  `tests/test_jit_strict_mode.cjs`.
- **Megamorphic cache entries** are cleared after each major
  (`sv_gf_mega_clear`): the major's epoch bump makes them miss anyway, and they
  kept their shapes alive until overwritten.
- **Shared definitions.** The keys whose writes need more than the store
  (`ant_property_key_is_watched`, `ant_property_key_is_protector` in
  `include/internal.h`) and the megamorphic hash shifts
  (`SV_GF_MEGA_*_SHIFT`) are defined once for C and the JIT; the inline
  constructor feedback's field sizes are asserted in `src/jit/emit_calls.c`.
- **Named stores into old arrays** remember the array without dirtying its
  cards (`gc_write_barrier_prop`, `gc_remember_props`), since a minor rescans
  every named slot of a remembered object.

## Rejected along the way

- An unrolled 8-way inline probe at every polymorphic site: doubled JIT
  compile time in large functions (`rayTrace` 102 -> 378 ms) and raised peak
  memory by up to 120 MB, because MIR's per-block data grows with the added
  branches.
- A runtime helper call at every read site for sites that become polymorphic
  after compiling: added compile time everywhere, and calls out of JIT code
  cost about as much as the lookup they save.
- Checking only the entries present at compile time: lost the gains, since
  those sites add their cases after compiling.
- Hits that did not promote: a site whose own case stayed on a rare shape went
  to the slow path on every read (DeltaBlue +26% instructions).
- Inline literal stores in all functions: Newt's 17 KB tokenizer function
  compiled 33 ms slower and peak memory rose 24 MB.
- Skipping constructor feedback recording once frozen: changes the counts
  `tests/test_ctor_prop_feedback.cjs` checks.

## Measurements (2026-09-28)

PGO builds trained on their own code; master is `9d61d90d`, committed is
`40a0b852`. Binaries and runners in `.cache/bench-20260928/`.

bench-v8, 5 rounds: Richards +4.0%, RayTrace +6.7%, EarleyBoyer +4.7%,
Splay +6.8%, the others within +-0.3% of master; geometric mean +2.8%
(committed alone +1.1%). A second PGO build of the same source gave the same
picture; PGO rebuilds of identical source move single tests by about +-2-3%.

gc-patterns fixtures: faster than master on 10 of 12, level on the ropes
cases. Apps: Splay long run 854 -> 748 ms and 1707 -> 1285 MB, Newt Main
24.3 -> 23.5 s, ytdlp, SSR and array_churn faster; identical output.

Open items:

- Newt Main peak 534 MB against 523-528 MB, with the same collection schedule
  (48 majors, 6910 minors) and the same peak at any size cutoff for the
  inline literal stores; not compile memory. Possibly different stale words
  under the conservative stack scan. Newt Main runs 7% fewer instructions.
- `medium-survival` peaks at 64 MB in 6 of 9 runs against 60 MB: the
  time-share growth rule grows the heap one step more when the mutator is
  faster (the committed build does it in 2 of 9 runs). Not a retention.
- The direct `new` path is emitted at every `new` site with no size gate;
  its compile cost in very large functions is not measured separately.
- Next candidate: per-object shape reference counting on allocation and free.

## Validation

`tests/test_poly_field_ic.cjs`, `tests/test_jit_define_slot.cjs` and
`tests/test_new_prototype_cache.cjs`, with the JIT and `--jitless`, match
node. Removing the epoch guard from the inline check, or the generation guard
from the inline store, makes the respective test fail. Full harness (only
`test_hono_adapter` fails, as on master and the installed binary), spec suite
4240/4240, JIT suite 10/10 files.

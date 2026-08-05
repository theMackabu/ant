# fable-perf-fixes Landing

Status: all phases implemented + interleaved A/B validated (2026-08-03);
GC-stress pass for 1b/2 still pending
Last reviewed: 2026-08-03

## A/B results (2026-08-03, interleaved, order alternated across rounds,
## vs pinned base /tmp/ant_fable_base = master 66499b0b)

newt (`todo/tests/newt`): Prelude 6270/6153 → 2759/2821 ms (**2.23×**),
Main 73678/74216 → 41475/41923 ms (**1.77×**). Main 41.7s vs June's ~36s
is explained by the excluded utf8 chunk index (June's comparable
pre-index point was ~39.5s).

bench-v8 (per-test children run directly under each binary — see caveat):
Richards 1336→2629 (1.97×), DeltaBlue 2831→4332 (1.53×), Crypto 704→1420
(2.02×), RayTrace 2371→3152 (1.33×), EarleyBoyer 3470→5064 (1.46×),
RegExp 435→409 (**0.94×, consistent both rounds/orders**), Splay
2676→3307 (1.24×), NavierStokes 2133→2892 (1.36×). **Geomean ≈ 1.44×.**

Caveats found while measuring:
- **`examples/bench-v8/index.js` always spawns `./build/ant`** for the
  benchmark children, regardless of which binary runs the runner — any
  "A/B" through the runner is self-vs-self. Measure by concatenating
  `tests/base.js + tests/<t>.js + harness.js` and running the file
  directly under each pinned binary. (Same trap family as `ant x`
  execing the PATH binary.) Fix the runner separately.
- **RegExp −6%**: small, consistent. Suspects: the `func_obj == 0`
  branch now in every `js_as_obj` on functions, or the dense-array check
  at the top of `arr_get`. Deferred; targeted look later.

## Implementation status (2026-08-03)

All phases (1a, 1b, 2, 3, 4, 5) are implemented in one uncommitted tree on
`perf/vm-dispatch`. Per-phase smokes + gates run at time of writing:
spec 3712/0 (3706 + 6 new lastIndexOf tests), harness 166/0 incl. oha
floors, jit suite 125/125, devirt fuzzer all seeds agree,
`test_jit_string_builder_snapshot` OK. **Not yet run**: interleaved newt /
bench-v8 A/B against `/tmp/ant_fable_base` for phases 1b–5 (Phase 1a was
measured: object-literal micro 2.4×, newt ~2%, no regression), and the
GC-stress pass for phases 1b/2.

Deviations from the June diffs (deliberate, current-tree adaptations):
- **1a**: shaped-site arrays additionally freed in `sv_compile_ctx_cleanup`
  (covers error paths; June's cleanup-label frees also kept, pointers NULLed).
- **1b**: two post-June JIT inline fastpaths (OP_INSTANCEOF,
  OP_CALL_IS_PROTO in swarm.c) got the obj-epoch guard the June diff could
  not know about; `with`-unscopables negative cache and
  `cached_function_proto_obj` moved to the obj epoch to preserve their
  pre-split invalidation cadence. Full audit table in the session log.
- **2**: no `rt` global anymore — `sv_closure_t` carries an `ant_t *js`
  owner pointer for lazy materialization. eval-env closures (direct eval)
  materialize eagerly. `jit_helper_export`/`sv_export_target_ns` read
  `closure->module_ctx` instead of forcing materialization.
  **New-drift fix**: `mir_emit_value_to_objptr_or_jmp` (shared by all JIT
  inline fastpaths) now bails to slow on `func_obj == 0` — without this,
  warm functions crash on property access of lazy closures (was caught by
  the jit suite, SIGSEGV at 0x10).
  **Generational GC bug found post-hoc (DeltaBlue SIGSEGV at 0x0)**: lazy
  materialization writes a *young* func_obj into a closure that may be
  *old*; closures reachable only through old-object properties are never
  traversed during minor GC (by design — see the comment at the bottom of
  `gc_objects_run_minor`), so the func_obj was swept while still referenced.
  Eager creation never hit this because closure and func_obj were born in
  the same generation. Fix: a closure remember set mirroring the upvalue
  one — `gc_remember_closure` (gc/objects.c, list on the isolate,
  `in_remember_set` flag on `sv_closure_t`), marked via `gc_mark_closure`
  during minors and cleared after minors / at major start.
  `sv_init_closure_function_object` remembers the closure *before* `mkobj`
  so the whole materialization window is covered; one minor promotes the
  func_obj subtree, after which old-generation pre-marking covers it. This
  generalizes to any future lazily-written closure field. Repro was
  `examples/bench-v8` DeltaBlue (ctors stored on old globals, materialized
  at first `new` after warm-up, minor GC before next use). The June branch
  has the same latent bug — it never ran DeltaBlue.
- **3**: MCO coroutine cstk swap sites are gone (stackless port) — only
  `js_setstackbase/limit` refresh `cstk.floor`. The devirt CALL_METHOD
  inline site passes NULL typed-args (it flushes up front). Typed-slot
  re-box lines were also applied to today's new inline-body cases
  (NIP, IS_UNDEF_OR_NULL, JMP_NOT_NULLISH, GET_FIELD_OPT).
- **4**: INC/DEC/POST_DEC eligibility + emitter cases already on master —
  only SWAP_UNDER/ROT4_UNDER/PUT_ARG were missing. `sv_put_field_cached`
  extracted from today's (richer) `sv_op_put_field`; the IC-hit path now
  bumps the epoch on `.prototype` writes (pre-existing hole vs the
  primitive-IC obligation). The 16 `l_is_num/r_is_num` re-box lines were
  applied to all 8 arith/compare sites (master had none).
- **5**: reimplemented on top of the guarded-store invariant:
  `mir_emit_numeric_local_store_mirror` used for all dnum stores (resume at
  the op, value in snapshot); snapshot re-box centralized in
  `mir_emit_bailout_jump_typed` + the two inline snapshots (NEG, BNOT);
  ADD_LOCAL generic path refreshes the boxed reg from the mirror before its
  guards. **Landmine hit and fixed**: `known_bool` must be cleared in the
  main emitter's label-merge memset block alongside slot_type/known_func —
  missing this made express 404 after JIT warm-up (single-BEQ branch on a
  non-boolean).
Owner: theMackabu

Land the June 2026 perf stack from branch `fable-perf-fixes` (5 commits,
~1282 insertions) onto current master. **Port per feature, do not merge**:
swarm.c has since gained devirt, the inline GET_FIELD IC fastpath, builder
guards, table-based opcode flags, derived-ctor handling and the primitive
property IC, so June hunks that "apply cleanly" are the dangerous ones — they
silently reintroduce deleted shapes or bypass guards added since. Treat each
commit as a **spec**, re-derive against today's code.

This document is written to be picked up cold. Read it top to bottom before
touching anything.

## Session-start checklist (nothing here survives a restart)

1. **Starting point**: master at `66499b0b` ("fix spawnSync in bench") —
   includes `a1a7605d` "primitive fast paths" (the primitive property IC port
   this plan builds on) and the jit-runner fix. Uncommitted at hand-off: the
   harness manifest `jit` group and this doc. The user commits; agents never
   do (`git commit`/`push` only on explicit request) — check `git status`
   before assuming anything about the tree.
2. **Re-extract the commit specs** (referenced throughout as
   `/tmp/fable_<hash>.diff`):
   ```
   for c in 0acc4c9b 21aec5e6 705a46fb b3894cbe 9cfb48e3; do
     git show $c > /tmp/fable_$c.diff
   done
   ```
3. **Re-pin the baseline binary** before any A/B measurement:
   ```
   ninja -C build && cp build/ant /tmp/ant_fable_base
   ```
   (Do this at the *current phase's* starting point, not from stale copies.)
4. Build: `ninja -C build`. Note `maid build` serves stale binaries from cache —
   use `maid -C build` or ninja directly, and sanity-check the binary changed.

Review follow-ups (2026-08-03, post code review):
- **Tier promotion made active** (landed after the A/B was banked):
  `jit_loop_hot` is now set unconditionally in `sv_jit_try_osr`, so an
  opt1-compiled function that later becomes loop-hot lands at opt3 on its
  next recompile. Measured interleaved passive-vs-active (one-line delta,
  pinned binaries): newt Main 40.1/41.7s → 37.9/38.9s (~-6%), Prelude flat
  to slightly better, compile count/time unchanged (1363 compiles, ~2.0s),
  octane spot checks neutral-to-positive. Verified against the
  passive-vs-active pinned pair on all three exposure surfaces: full
  bench-v8 (8 tests, both orders — all within noise), oha RPS numbers
  (hono/express/h3/elysia, both orders — no systematic direction; note
  the harness honors `ANT_TEST_BIN`/execPath so harness A/B is valid,
  unlike the bench-v8 runner), and an adversarial recompile-churn micro
  (300 funcs x compile→OSR→type-churn→recompile: dead even, recompiles
  are tfb-bounded). All debug-stats scaffolding (`ANT_JIT_STATS`,
  `ANT_GC_STATS`, `ANT_LAZY_STATS`) was removed after verification;
  `ANT_JIT_OPT` remains (it selects behavior, not stats).
- **Numeric literal keys via `%g`** (pre-existing, both paths): keys ≥ 7
  significant digits become exponent-form strings, e.g.
  `({123456789: 1})` gets key `"1.23457e+08"`. The port is faithful and
  internally consistent; fix separately with proper number-to-string.

## Commit inventory

| commit | contents | plan |
| --- | --- | --- |
| `9cfb48e3` shape caching | DEFINE_SLOT literal boilerplates (compiler+literals+glue+swarm), IC obj-epoch split (shapes.h/shapes.c/gc.c/comparison.h/property.h), utf8 chunk index, **plus an unrelated `lastIndexOf` surrogate fix in ant.c** | Phase 1a + 1b; utf8.c EXCLUDED (see `utf16-random-access-index.md`) |
| `b3894cbe` inline changes | lazy function objects (`sv_closure_t` module_ctx/pending_name, `func_obj==0` sentinel + materialize), GC closure cadence (gc.c alloc-count triggers) | Phase 2 |
| `21aec5e6` improve jit | tiered JIT (ctx_hot/opt3, jit_loop_hot), cstk-floor inline probe, param_cache, **and the entire typed-inline-boundary round** (inl_num/inl_d, arg_num, I2DB/D2IB usage) — far bigger than its label; heaviest phase | Phase 3 |
| `0acc4c9b` opcode eligibility | eligibility for INC/DEC/POST_DEC/PUT_ARG/SWAP_UNDER/ROT4_UNDER, put-field IC (`jit_helper_put_field_ic` → `sv_put_field_cached`), int32 itoa fast path, arr_get dense fast path, regexp first-char memcmp gates | Phase 4; eligibility re-expressed as OP_FLAG table entries; check which small wins master already has |
| `705a46fb` numerical awareness | dnum locals + known_bool (swarm.c only) | Phase 5: **REIMPLEMENTATION, not a pick** — predates the guarded-store invariant |

MIR side: I2DB/D2IB bitcast insns are already in the vendored fork
(`vendor/mir/mir.h:101`) — nothing to merge there.

## Baselines (2026-08-02, master + primitive-IC port)

Recorded with the tree that includes the `claude-string-perf-fix` port
(primitive property IC). Re-measure if the starting point differs.

- **newt** — `./build/ant todo/tests/newt/bootstrap/newt.js todo/tests/newt/src/Main.newt`
  Prelude **6130ms**, Main **76472 / 77322ms** (two runs, ~1% variance).
  June post-stack targets: Prelude ~2.5s, Main ~36s.
- **bench-v8** — `./build/ant examples/bench-v8/index.js`: Richards ~1298,
  DeltaBlue ~2695, Crypto ~683, RayTrace ~2228–2356, EarleyBoyer ~3300,
  RegExp ~415, Splay ~2760–2820, NavierStokes ~2089.
  June targets: Richards 2637, DeltaBlue 4280, Crypto 1433, Navier 2647.
- **bench_call_fallback**: charCodeAt 16 ns/op.
- Suites green: spec 3706/0, harness 166/0, jit suite 125/125.

## Validation discipline (every phase)

- **Gates**: `./build/ant examples/spec/run.js --all` (expect 3706 tests, 0
  files failed) · `./build/ant tests/harness/run.js` (expect 166+ passed, 0
  failed — includes oha RPS floors and the jit suite) ·
  `./build/ant examples/jit/run.js` (125/125) · newt interleaved vs the pinned
  base · the phase's own micro · bench-v8 where relevant.
- **devirt differential fuzzer**: `./build/ant todo/tests/devirt_fuzz.cjs`
  (checked in; 4 seeds vs node, exit 1 on divergence) — **mandatory** after
  anything touching call dispatch (Phases 3–5).
- **Only interleaved same-moment runs with pinned binaries count.** Never
  compare two variant binaries to each other — always against the pinned base.
  (Lesson: a phantom Buffer.from "regression" was chased for an hour because
  two variants were compared without a true baseline.)
- **GC-touching phases (1b, 2)**: temporarily re-add an `ANT_GC_STRESS` tick to
  `gc_maybe` (shape documented in `completed/module-import-gc-flake.md`) to make
  rooting hazards deterministic; remove before finishing.
- **One phase per checkpoint.** The user commits between phases. A phase that
  goes sideways is reverted to its checkpoint, not patched forward.

## Phase 0 — prep (DONE 2026-08-02)

- Fixed `examples/jit/run.js` (`spawnSync` needed `encoding: 'utf8'` after
  child_process adopted node's Buffer default; the runner called
  `.endsWith()` on a Buffer). Wired it into the harness manifest as group
  `jit` so it can't silently rot again.
- Recreated the devirt differential fuzzer at `todo/tests/devirt_fuzz.cjs`
  (the original lived in /tmp and was lost).
- Pinned baselines (above); inventoried all 5 commits — confirmed **nothing
  from the June rounds is uncommitted**; the typed-inline work is inside
  `21aec5e6`.

## Phase 1a — DEFINE_SLOT literal shape boilerplates (NEXT)

Fully mapped from `9cfb48e3`; all anchors verified present on master. Goal:
object literals with all-static keys build their final shape **once per
site** instead of walking `ant_shape_add_interned_tr` per property per
object (~14% of newt runtime today).

Pieces, in dependency order:

1. **`include/silver/opcode.h`** — add `OP_DEF(DEFINE_SLOT, 7, 2, 1, atom)`
   (atom u32 + slot u16) and `OP_FLAG(DEFINE_SLOT, SV_OPF_JIT_ELIGIBLE)`.
2. **`include/silver/engine.h`** — extend `sv_obj_site_cache_t` with
   `const uint32_t *key_atoms; uint16_t key_count;`. Non-NULL `key_atoms`
   means `shared_shape` (once built) is the FINAL shape and the following
   DEFINE_SLOTs store positionally.
3. **`include/silver/compiler.h`** — add `shaped_sites` / `shaped_keys`
   arrays + counts/caps to `sv_compiler_t` (malloc'd, consumed and freed by
   `sv_func_init_obj_sites`).
4. **`src/silver/compiler.c`** —
   - `object_literal_static_keys()`: qualifies a literal (≤32 props, every
     prop `N_PROPERTY`, no `FN_GETTER|FN_SETTER|FN_COMPUTED`, no
     `__proto__:` colon form, no duplicate keys; handles ident / quoted-ident
     / string / number keys).
   - `record_shaped_site()`: appends `{bc_off, first_key, key_count}` + atom
     indices.
   - `compile_object()`: on qualifying literals emit `OP_OBJECT`, record the
     site, then `OP_DEFINE_SLOT` + atom + positional slot per property
     (preserving `compile_expr_with_inferred_name` for ident keys).
   - `sv_func_init_obj_sites(sv_compiler_t *c, sv_func_t *func)` — takes the
     compiler now; copies each recorded site's atom list into the code arena
     and frees the compiler-side arrays via a `cleanup:` label. **Three call
     sites** to update (compiler.c ~5868, ~6096, ~6560).
5. **`src/silver/ops/literals.h`** — extract `sv_obj_site_apply(js, func,
   site, ptr)` from `sv_op_object`: when `key_atoms` is set and
   `shared_shape` is empty, walk `ant_shape_add_interned_tr` once to build
   the final shape (retain it, release on failure); then adopt the shape and
   `js_obj_ensure_prop_capacity`. Keep the old non-static behavior for sites
   without `key_atoms`.
6. **`src/silver/ops/property.h`** — `sv_op_define_slot()`: bounds-checked
   `ant_object_prop_set_unchecked` + `gc_write_barrier`; falls back to
   `sv_try_define_field_fast` / `js_define_own_prop` when pre-shaping failed.
7. **`src/silver/engine.c`** — dispatch label `L_DEFINE_SLOT ... NEXT(7)`.
8. **`include/silver/glue.h` + `src/silver/glue.c`** —
   `jit_helper_object` gains `(sv_func_t *func, int32_t bc_off)` and applies
   the site; new `jit_helper_define_slot(vm, js, obj, val, str, len, slot)`.
9. **`src/silver/swarm.c`** — `LOAD_EXT` + `MIR_new_import` for
   `jit_helper_define_slot`, a 7-arg `define_slot_proto`, the `OP_DEFINE_SLOT`
   emitter case, and update the `OP_OBJECT` call site to the new 4-arg
   `object_proto` (passing `func` and `bc_off`).

**Bonus correctness fix in the same commit** (unrelated to shape caching,
take it): `builtin_string_lastIndexOf` in `src/ant.c` conflates UTF-8 byte
offsets with UTF-16 indices. Live bug on master —
`"abc🎈def🎈xyz".lastIndexOf("🎈")` returns **10**, node returns **8**. The
commit's version converts the position argument and result through
`utf16_strlen` / `utf16_index_to_byte_offset`. Add a regression test.

Gates: spec, harness, jit suite, newt (shape-transition cost should largely
vanish), plus object-literal-heavy micros.

## Phase 1b — IC obj-epoch split (TOP RISK)

Split `ant_ic_epoch_counter` into two: keep the main epoch for property ICs
(which revalidate against the live receiver's shape and *survive* minor GCs),
and add `ant_ic_obj_epoch_counter`, bumped by both minor and major GC, for
caches that hold raw object pointers without shape revalidation
(instanceof / isPrototypeOf). Minors then stop wiping every property IC —
worth a lot on newt (428 minors/run today).

`9cfb48e3` contains the June version (shapes.h, shapes.c, gc.c,
comparison.h, property.h — including reordering `sv_ic_try_get_hit` to
value-compare the proto *before* dereferencing the cached holder, which is
what makes surviving minors sound).

**The audit is the work, not the diff.** The June change predates several
cache families. Enumerate every one on today's tree and prove its
invalidation survives minors not bumping:

- get-field IC (+ the warmup/miss/active `cached_aux` machinery)
- the `add`-transition guard in the put path
- **the primitive property IC, positive AND negative entries** (added
  2026-08-02; the negative entry's guards are `.prototype` writes and proto
  rewiring, not GC — expected to be *helped* by the split, but prove it)
- instanceof / isPrototypeOf pointer caches (these are the ones that need the
  new obj epoch)
- `obj_sites` shared shapes (Phase 1a adds more of these)
- JIT inline IC fastpaths in swarm.c
- any cached import bindings

## Phase 2 — lazy function objects + GC closure cadence (`b3894cbe`)

`sv_closure_t` gains module_ctx/pending_name; `func_obj == 0` sentinel with
`sv_closure_materialize_func_obj`; OP_CLOSURE / `jit_helper_closure` /
SET_NAME stash instead of building; GC marks module_ctx and skips
`func_obj == 0`. **Critical pairing**: minors never sweep closure arenas, so
lazy closures starve the nursery trigger — the commit's gc.c changes add
closure-allocation counters that trigger minors/majors
(`GC_CLOSURE_NURSERY_THRESHOLD` 16384, `GC_CLOSURE_MAJOR_THRESHOLD` 131072).
Landing the closures without the cadence made things *slower* in June.

Port onto today's `gc/objects.c` (mark hooks were deleted in the isolate-
values migration; remember-set machinery is current). Gates: async harness
group with its maxRss bounds (`test_upvalue_gc` etc.), GC stress, newt
(second-biggest win: `jit_helper_closure` ~11% + GC ~30% subtree).

## Phase 3 — tiering + cstk floor + param_cache + typed inline (`21aec5e6`)

Heaviest phase. Contents: two MIR contexts (opt1 fast tier, opt3 for
loop-hot functions via `jit_loop_hot`), inline C-stack probe against a
cached `js->cstk.floor`, read-only param hoisting (`param_cache`), and the
typed-inline boundary (`inl_num[]`/`inl_d[]` slots, typed arg passing,
guard-free typed arithmetic paths).

Landmines recorded in the perf memory:
- **`param_cache` must be emitted at `self_tail_entry` BEFORE the OSR
  dispatch** — OSR entries must populate them, or cold calls return garbage.
- **MIR crashes** ("wrong get for bitmap_t") if `MIR_gen_set_optimize_level`
  changes between `MIR_gen` calls in one context — hence two contexts.
- opt3 costs ~3.4× opt1 compile time; newt Prelude was once 62% MIR-compile
  time at opt3. **newt must not regress** — this is the shape that regressed
  it before.
- The inline emitter (`inl_vs`) is a *distinct region* from the main emitter
  (`vs`); scripted edits matching case bodies can land in the wrong one.
- Typed-slot flags must be flushed at labels/branches/non-typed-aware cases
  and cleared on pop; stale flags are the hazard.

Gates: newt, compile counts, fib/call micros, full harness
incl. oha floors, devirt fuzzer.

## Phase 4 — eligibility flags + put-field IC (`0acc4c9b`)

Eligibility is now a table (`OP_FLAG` in opcode.h) — re-express, don't take
the old swarm.c hunks literally. For the put-field IC, first check what
master already has (`sv_try_put_field_fast` + the add-transition guard exist)
— possibly only the JIT helper wiring is missing.

**Interaction**: whatever cached put path results **must** carry the
`.prototype` write epoch bump — that is a load-bearing obligation of the
primitive IC landed 2026-08-02 (see `sv_try_put_field_fast` and `mkprop`).
Also check whether master already has the int32 itoa / dense arr_get / regexp
memcmp wins before porting them.

## Phase 5 — dnum locals + known_bool reimplementation (`705a46fb`)

Use the commit as a spec only. It predates the invariant that **every store
to a specialized (known-NUM) local must be runtime-guarded**
(`mir_emit_numeric_local_store_mirror` and the guarded OSR entry) — merging
it verbatim would reintroduce the stale-mirror bug class killed this month
(see `ant-string-builder-snapshot` memory). Eligibility rules, bailout
snapshot reboxing (`mir_emit_dnum_rebox`), and the `known_bool` slot tracking
are the parts worth reusing.

Gates: `tests/test_jit_string_builder_snapshot.cjs`, jit suite, newt,
devirt fuzzer.

## Phase 6 — generational closures (2026-08-05, implemented, tuning pending)

Young-closure/young-upvalue rosters (`js_closure_alloc` appends; slow-path
trackers in gc/objects.c) + young sweeps in `gc_objects_run_minor` that
mirror the major arena sweep; trigger in `js_closure_alloc` at
`GC_CLOSURE_NURSERY_THRESHOLD` via `gc_pressure`. `ANT_GC_LOG=1` prints
freed/promoted counters at teardown.

Liveness bugs found and fixed (each was a hard crash):
1. JIT open-upvalue chains are raw cell pointers on the C stack —
   `gc_scan_range` now walks the chain with per-hop arena containment
   checks.
2. `gc_pin_existing_objects` promotes without marking — young rosters are
   drained at pin time (boot closures held only by pinned objects were
   swept; the "bouncer TypeError").
3. **Sidecar containers skip the barrier**: emitter listeners
   (`src/modules/events.c`) and abort-signal listeners/followers
   (`src/modules/abort.c`) live in malloc'd arrays only reached by scanning
   the owner object, which minors skip for old owners. Pre-existing hole,
   invisible while minors never swept funcs; fatal once they did
   (`examples/demo/event_loop.js` beforeExit SIGSEGV after 5s of churn).
   Fixed with `gc_write_barrier` at the four store sites. Audit: the only
   sidecars walked from `gc_scan_obj` are eval-env (creation-time-only
   writes, safe), abort, emitter — all covered.

Results: churn micro 10M closures → 17MB max RSS (node: 58MB). All gates
green (spec 3712/0, harness 167/0 incl. oha + new
`tests/test_gc_closure_churn.cjs` [96MB mem cap], jit suite 0 fail).
newt completes end-to-end; node baseline for newt: Main 3.34s, 309MB RSS.

Tuning measurements (2026-08-05, per-minor log via ANT_GC_LOG):
- At threshold 16384: 13,732 minors totaling **17.35s** of Main (~47s!),
  p50 854µs, avg roster exactly 16,384, ~988 promoted/minor (13.6M total),
  RSS 1.52GB. (The teardown `minors=` count resets at majors — use the
  per-minor lines for totals.)
- Threshold raised to **131072** (current): 1,689 minors / 12.4s, promoted
  13.6M → **3.5M** (longer nursery residence = free aging), RSS 1.22GB,
  Main ~-2s. Gates re-run green; churn harness test now 51MB.
- Per-minor cost is LINEAR in roster (~56ns/dead closure): the sweep is
  dominated by `free(c->upvalues)` per dead closure — total sweep work is
  proportional to allocations (225M × 56ns ≈ 12s) and cadence cannot
  reduce it. The fixed root-scan cost is what the 128k change amortized.

Tuning LANDED (2026-08-05, later):
1. **Inline upvalue slots** (`inline_upvals[4]` in sv_closure_t, struct
   104→136B): `closure->upvalues` points at inline storage when
   upvalue_count <= 4 — no calloc in jit_helper_closure / OP_CLOSURE /
   bound-closure copy, no free() in either sweep (both sweeps + major
   sweep guard `upvalues != inline_upvals`). Arena elems never move, so
   the self-pointer is stable; arena alloc memsets, so slots start NULL.
2. **Promoted-count major trigger**: `gc_closure_promoted_since_major`
   (reset at major) forces major_due at GC_CLOSURE_PROMOTED_MAJOR (262144
   ≈ 36MB). This fixes the real leak: the watermark-growth check measures
   per major *window* and steady promotion stayed under it forever.
3. **Age-mark aging REJECTED after implementation**: our remember-set
   entries are one-shot, so a young closure held by an old object survives
   minor 1 (remembered scan) but is unmarked at minor 2 → swept while
   referenced (caught immediately by the emitter-once micro). Sound aging
   requires rebuilding remembered slots during minor scans like V8's
   scavenger. Nursery residence at 128k already provides most of the
   benefit (13.6M → 3.5M promotions).

newt A/B (same binary, back-to-back): Main **42-43s** (was 46.7s; pinned
pre-feature base 41.7s), minors 12.4s → 9.6s, RSS **~960MB** (was 1.52GB;
node 309MB), closure arena watermark at exit 53MB (was ~1GB-class),
upvalue arena 21MB, obj arena 274MB. Threshold sweep: 16k gives Prelude
2.86s but Main 54s (13.6M promotions → ~50 forced majors); 131072 gives
Prelude 3.6s (+0.75s, real) and Main −11s — 131072 ships. All gates
re-run green (spec 3712/0, jit 0 fail, harness churn test, DeltaBlue 4212,
Splay 3373 / Richards 2557 = branch-expected). Closure GC is now
~time-neutral vs base with bounded memory.

## Phase 7 — closure-volume attack (2026-08-05, measurement + first wins)

**Measurement first** (ANT_CLOSURE_STATS=1 per-site counters, keyed by
child sv_func, dumped at teardown; hooks in jit_helper_closure +
sv_op_closure): newt Main's ~225M closures decompose into:
- **~60M+ eta-curried steps**: `(eta1) => KnownFn(eta0, eta1)` — instance
  method wrappers (`compare(ord)(x)(y)`), IO `return`. Created by calling
  the outer curried lambda, invoked exactly once, dropped. Locally
  fusable (see 7b below).
- **~78M monad plumbing**: M = MkM(tc => world => ...); every bind
  allocates the tc-closure (stored into the MkM record — ESCAPES), the
  world-closure, and a per-site continuation (upvals 4-9). Not locally
  elidable — requires monad-chain inlining.
- **Verdict: classic compile-time escape elision hits ~0% of newt** —
  everything flows into records or continuation arguments.

Flat profile of Main (10s `sample`, top-of-stack): gc_object_free 12.8%,
jit_helper_closure 5.5%, gc_run_minor 4.2%, memset 2.6%,
jit_helper_object 2.5%, malloc-subsystem ~5%, define_slot 2.0%,
gc_run 2.0%, mkobj 1.9%, gc_scan_range 1.6%, close_upval 1.4%.

**7a LANDED (this tree):**
1. gc_object_free fast path: plain data objects (no finalizer / native /
   sidecar / promise / overflow / exotic / container payload — checked as
   one OR-fold) take a 3-store exit instead of the branch ladder + two
   unconditional libc free(NULL) calls. Targets the 12.8%.
2. js_closure_alloc_hot + fixed_arena_alloc_uninit: the two hot creation
   paths skip the 136B element memset; all GC-observable fields
   (call_flags/upvalues/bound_argv/epoch/in_remember_set) are written at
   alloc so a mid-init sweep stays safe; stale inline_upvals words beyond
   upvalue_count are unreachable (closure upvalues are walked precisely).
   General js_closure_alloc keeps zeroing (new Function / bind paths rely
   on it).

7a A/B (interleaved, 2 rounds, pinned binaries /tmp/ant_base7_bin vs
/tmp/ant_p7a_bin): newt Main 44.5/45.9s → 42.2/43.2s = **−2.5s (−5.5%)**,
consistent across rounds. Gates green on the 7a binary: spec 3712/0, jit
0 fail, harness 167/0, DeltaBlue 4161, all GC regression micros.

**7b NEXT (designed, not implemented) — curried-call fusion (~60M
closures):** compiler flags `curried_step` funcs (body exactly
`CLOSURE k; RETURN`) and `leaf` children (no OP_CLOSURE in body); a
CALL-of-CALL fast path then runs the grandchild directly with a scratch
upvalue array (param captures as stack cells, parent upvals passed
through) — no intermediate closure. HAZARDS mapped: frame->closure
identity (module_ctx chain, error-stack capture, arguments), stack cells
must not outlive the call (guaranteed by leaf flag), GC scanning of
scratch cells (safe: precise frame walk marks cell values; containment
checks ignore non-arena cells). Needs devirt-fuzzer + full battery.
**7c LATER**: monad-chain inlining (JIT inline bind/runM through MkM) —
the remaining ~78M; and object-churn reduction (gc sweep touches ~150B
per dead object; the arena walk is memory-bound).

~2.5× on newt if everything lands (June: Prelude 6.2→2.47s, Main 70→~36s
across the rounds), Octane geomean from ~1692 toward the June ~2600-class
scores. The primitive-IC port already landed separately (charCodeAt 4.5×,
primitive-miss path 7×).

## Decision log

- 2026-08-02: port-per-feature over merge (swarm.c drift makes clean applies
  the dangerous case).
- 2026-08-02: utf8 chunk index from `9cfb48e3` **excluded** — a newer port of
  that idea was validated and then reverted on master over an unresolved +50%
  sequential-scan cost; decide against `utf16-random-access-index.md`, not the
  June diff.
- 2026-08-02: `claude-string-perf-fix` superseded by a direct port rather than
  a cherry-pick; same method chosen for this branch.
- 2026-08-02: `lastIndexOf` surrogate fix folded into Phase 1a (bug confirmed
  live on master).

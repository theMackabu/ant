# fable-perf-fixes Landing

Status: all phases implemented + interleaved A/B validated (2026-08-03);
regression-fix session 2026-08-06 closed every open item (see the
"Regression-fix session" section): R1 gc_async/coro FIXED (regex-cache
O(n²) scan), R2 regex-vs-prod FALSIFIED (PGO build artifact), R3
devirt-in-inline LANDED (call-op inlining re-enabled), R4 double-bind
FIXED, R5 numeric literal keys FIXED, R6 adaptive nursery REJECTED with
data, GC-stress pass DONE (hook removed).
R1 follow-up on 2026-08-06 added canonical RegExp result shapes and guarded
batched global match/replace: result-consuming `exec()` −21.8%, global
match+replace −77.3%, final harness 172/0.
R4 parity follow-up on 2026-08-06 fixed bound generator construction,
bound-function prototype/new-target semantics, sloppy primitive `this`
boxing, and the RegExp 32-capture result limit.
A 2026-08-07 property-delete follow-up replaced the correctness-preserving
quadratic slot shift with ordered tombstones and bounded stable compaction.
A 2026-08-07 curried-call review follow-up restored JavaScript argument
evaluation order without giving up newt's 11.96M fused calls.
A 2026-08-07 primitive-IC review follow-up made computed and transition-based
prototype additions invalidate both warmed absence entries and inherited hits.
Last reviewed: 2026-08-07

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
  execing the PATH binary.) FIXED 2026-08-06: children now use
  `process.execPath`, so a runner invoked through a pinned binary uses that
  exact binary without an override environment variable.
- **RegExp −6% was historical and is resolved/superseded.** The current
  tracked score is 930 versus master's 417; do not investigate the old
  `func_obj == 0` / dense-`arr_get` suspects from the pre-RegExp-work tree.

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

**7b LANDED (2026-08-05) — curried-call fusion:**
- Compiler: `is_curried_step` (body `CLOSURE k [SET_NAME] [CLOSE_UPVAL]
  RETURN` + unreachable epilogue — the naive 6-byte pattern matched
  NOTHING; SET_NAME names the skipped intermediate, CLOSE_UPVAL closes
  captures the fused path synthesizes pre-closed) and `is_fusable_leaf`
  (opcode blacklist: closures/this/special-obj/eval/exports/try/await/
  yield + any backward jump so no loops→no OSR; local captures only at
  parent slot 0; <=4 upvalues). `OP_CALL_CALL n1:u8 n2:u8` emitted for
  `X(a)(b...)` — plain non-member/super/eval inner callee, single inner
  arg, all outer args effect-free (literals or initialized const locals;
  that's when pre-evaluating them is spec-legal).
- Runtime `sv_op_call_call` (ops/calls.h, shared by the interpreter case
  and jit_helper_call_call): fuses via a C-stack `sv_closure_t fake`
  (upvalues = its own inline_upvals; fresh capture as a C-stack closed
  cell; in_remember_set=1 on both so gc_remember_* never list them;
  frame callee = the step closure, a real heap value) — the intermediate
  closure never exists. Fallback: two ordinary sv_vm_calls.
- **CRITICAL LESSON — fuse only into JIT'd children.** v1 ran the child
  via sv_call_closure (interpreter): newt Main went 44s → **84s** — the
  fused frame interpreted the child and, through its tail calls, the
  whole chain below it, wiping out the devirtualized JIT call path.
  Discriminator: fused-path-disabled build ran 44.2s (generic routing of
  32.7M chain calls through the helper is FREE). Fix: `if (!f2->jit_code)
  goto generic` — cold children take the generic path (which is what
  warms them up via normal tiering); hot children run their jit_code
  directly against the fake closure (bailout → sv_jit_on_bailout +
  interp fallback, mirroring sv_call_resolve_closure).

7b results: newt fuses **11.96M** of 32.7M chain calls (rest are
member-form inner callees / non-simple args — extension candidates).
Interleaved A/B vs pinned 7a (/tmp/ant_p7a_bin vs /tmp/ant_p7b_bin):
Main 42.5/46.8 → 41.2/43.3 = **−2.3s (−5%)**, both rounds faster. Gates:
spec 3712/0, jit 0 fail, harness 168/0 (incl. new
`tests/test_curried_call_fusion.cjs` — 18 semantics assertions covering
mutation-through-cell, parent upvals, bound/member/throwing/default/rest
cases), DeltaBlue 4271, Richards 2591, Splay 3586, GC micros green.
`ANT_CLOSURE_STATS=1` prints `[call-call] fused/generic`.

**7b C1 review follow-up — outer-argument order FIXED, 2026-08-07:**
- **Mechanism:** the original `fusion_arg_is_effect_free` admitted every
  resolved non-TDZ local. `OP_CALL_CALL` evaluates its complete stack input
  before calling `X`, so `let b=1; function X(a){ b=99; return y=>y; }
  X(0)(b)` returned 1 rather than 99 in both the interpreter and JIT. Checking
  `local.captured` at that source position is also unsound: capture metadata is
  discovered lazily, and a later closure can capture a binding used by an
  earlier call site.
- **Tried and ruled out:** restricting eager fusion to literals plus const
  locals fixed semantics, but a pinned whole-newt counter run fell from
  `[call-call] fused=11962008 generic=20775513` to
  `fused=0 generic=1051774`. Interleaved newt measured Main
  40.470/41.352s → 43.280/42.850s, a real ~2.15s (~5.3%) regression. Do not
  re-land the const-only gate without an order-preserving replacement.
- **Fix:** eager `OP_CALL_CALL` now accepts only literals and initialized const
  locals. A single mutable local outer argument uses
  `OP_CALL_CALL_SLOT slot:u16`: the curried-step fast path may read it directly
  after proving `X` side-effect-free, while the generic path calls `X` first
  and then reloads the current frame slot. The interpreter reacquires the slot
  after `sv_vm_call` because VM storage may move. The JIT passes captured and
  writable params/locals through their canonical slot buffers, and boxes an
  uncaptured numeric local into stable temporary storage. Raw string-builder
  slots are materialized only at the ordinary outer-read point.
- **Evidence:** final pinned identities are the unsafe baseline
  `/tmp/ant_c1_base_bin` (`213630b7ad3c3878d6be8f3bfe1d12dc`) and fixed
  `/tmp/ant_c1_final_bin` (`e8ed61858dd86ea730c71c0d565a3725`). AB/BA newt
  Main was 41.502/39.629s before versus 41.724/39.921s after (+0.26s,
  +0.6% on the means, inside newt's ~1s thermal noise); max-RSS means were
  ~892MB before and ~868MB after. The final counter run restored exactly
  `fused=11962008 generic=20775513` and kept Main at 40.898s / 853MB RSS.
- **Regression coverage and gates:** the curried-call test now checks the exact
  former tautological hot-loop result plus cold/hot side-effect order, a
  future-capture site, captured/plain/written parameters, d-only numeric
  locals, try/catch routing, and fast/generic string-builder slots. Final gates:
  spec 3713/0, JIT 9/0 + generated 125/0, harness 175/0 (hono 34,473,
  express 18,707, h3 21,918, elysia 65,095 RPS), and all four devirt-fuzz
  seeds agree.

**7b C2/C3 review follow-up — primitive IC invalidation FIXED,
2026-08-07:**
- **Mechanism:** `sv_ic_try_get_miss_prim` proved prototype-chain absence with
  shape pointers. `ant_shape_add_interned_tr` may add a computed string key to
  a detached shape in place, leaving that pointer unchanged and previously
  leaving the global IC epoch unchanged. A warmed `s => s.k1` therefore kept
  returning `undefined` after `String.prototype[computedKey] = 7`, even though
  a cold computed access returned 7. The same stale result occurred when the
  in-place addition was on the second guarded shape (`Object.prototype`).
  A positive primitive hit had the complementary hole: when the cached holder
  was level 2 (`Object.prototype`), `sv_ic_try_get_hit_prim` guarded that holder
  but not the level-1 absence it depended on. After warming
  `s => s.isPrototypeOf`, shadowing the key on `String.prototype` therefore
  kept returning the cached level-2 function.
- **Fix:** primitive negative fills mark every shape whose absence they cache;
  positive fills mark each skipped prototype before the cached holder. A
  successful in-place string-key insertion into a shape carrying a live mark
  clears it and bumps the epoch. A transition-tree addition from a marked
  source shape does the same, because positive hits do not otherwise retain
  that intermediate shape pointer. Unmarked shapes take a zero-field fast
  return and never load the global epoch. The hot positive and negative hit
  paths are unchanged. This avoids both a hash lookup on every positive hit
  and an unconditional epoch bump on every property addition. The mark occupies
  the existing alignment gap in `ant_shape_t`, so its allocation size is
  unchanged.
- **Correctness evidence:** pinned base `/tmp/ant_c2_base_bin`
  (`e8ed61858dd86ea730c71c0d565a3725`) fails both new computed-addition cases
  while their cold accesses pass; Node and final
  `/tmp/ant_c2_candidate_bin` (`41154daee76551f0e68a6293405a8d8b`) pass the
  complete primitive-IC test. Regression coverage forces detached shapes and
  checks both the first and second prototype guards, including deletion after
  each addition. The positive follow-up base `/tmp/ant_c3_base_bin`
  (`41154daee76551f0e68a6293405a8d8b`) fails both a computed in-place shadow and
  a constant-key transition shadow while cold reads pass; Node and final
  `/tmp/ant_c3_candidate_bin` (`00c666b96807bbe7f23ac25806f95d80`) pass both.
- **Performance evidence:** a fixed 300-million primitive-miss micro was flat
  over alternating AB/BA rounds: base **2.97/2.97s**, final **2.96/2.98s**.
  The positive follow-up's fixed 300-million level-2 hit micro was also
  non-regressing at **2.23/2.23s → 2.16/2.15s**. Property creation held at
  **38.27/37.62ns → 37.45/38.14ns** after adding the zero-mark fast return.
  Treat the apparent positive-hit improvement as rebuild/PGO noise rather than
  a claimed win.
- **Final gates:** spec **3713/0**, JIT **9/0**, harness **175/0**
  (`test_gc_async` 402ms, `test_gc_coro` 5.941s; hono 32,840, express 18,166,
  h3 20,778, elysia 61,937 RPS), and newt Main **42.791s** / **43.14s wall** /
  **611,827,712-byte max RSS**.

**7b C4 review follow-up — JIT value-fact stack parity FIXED,
2026-08-07:**
- **Mechanism:** `known_bool` was indexed by virtual-stack position but did not
  follow values through `DUP`, `NIP`, `INSERT`, `SWAP`, or `ROT` operations and
  was not destroyed when `OP_VOID` overwrote its operand. The conditional-jump
  fast path then trusted a stale boolean fact and interpreted the new non-boolean
  value with the boolean representation. The minimal repro
  `if (void (a === b)) return "T"` is correct while interpreted, then returns
  `"T"` on call 100 when the function becomes JIT-compiled.
- **Fix and invariant:** semantic facts (`known_func`, constant value, and
  `known_bool`) are now captured, moved, copied, and cleared as one
  `jit_value_info_t` bundle by the virtual-stack operations. `slot_type` remains
  separate because it describes physical MIR representation rather than the JS
  value. `OP_VOID` clears the old bundle before recording its new `undefined`
  constant; catch-slot writes receive the same invalidation discipline. This
  retains known-target dispatch and boolean jump specialization without allowing
  one fact array to drift away from the value it describes.
- **Correctness evidence:** pinned baseline `/tmp/ant_c4_base_bin`
  (`00c666b96807bbe7f23ac25806f95d80`) fails the exact repro at call 100.
  Candidate `/tmp/ant_c4_candidate_bin`
  (`c0caa327ca5a8d85fbbb3a078bd9387d`) passes the new cold/hot regression and
  100,000 warmed calls. The generated opcode suite remains **125/0** and all
  four devirt-fuzz seeds agree.
- **Performance evidence:** direct pinned AB/BA rounds over fixed bench-v8 work
  were non-regressing. Richards was **2864/2849 → 2863/2835** (midpoint -0.3%),
  DeltaBlue **4340/4465 → 4536/4558** (+3.3%), and their composite
  **3525/3567 → 3604/3594** (+1.5%). With two rounds, claim only no regression;
  the apparent gains may include rebuild/thermal noise.
- **Final gates:** spec **3713/0**, JIT **9/0**, harness **176/0**
  (`test_gc_async` 397ms, `test_gc_coro` 5.898s; hono 33,248, express 18,616,
  h3 21,290, elysia 62,355 RPS), and newt Main **41.996s** / **42.37s wall** /
  **612,777,984-byte max RSS**.
- **Build-graph cleanup:** `pkg_zig` no longer uses
  `build_always_stale: true`; its 18 actual Zig/C inputs are declared explicitly.
  The first `maid build` after the change did not invoke Zig, and an immediate
  unchanged second build reported `ninja: no work to do` in **351ms**. Package
  source edits still dirty the target through the declared input list.

**7c scoping session (2026-08-05, late) — two candidates investigated and
KILLED with data; record the negatives:**
1. **Member-form chain fusion (CALL_CALL_METHOD): DEAD.** Post-7b site
   counts prove the big monad sites are untouched by any syntactic
   fusion — MkIORes intermediates are exactly 19,618,539 before AND
   after 7b: their creating call (`return_(v)`) and consuming call
   (`action(w)`) live in *different functions* (the IO action is stored,
   then applied generically by the runner). No bytecode-local pattern
   can reach them; only semantic inlining (real 7c) can.
2. **Computed-key stub cache: built, validated, REVERTED.** A profile
   sample showed jit_helper_get_elem at ~19% top-of-stack; per-caller
   counters traced all 30.6M computed-key reads to newt's OWN
   self-instrumentation (timedM/record/recordSpan/statFor —
   `stats[name]`-style dictionary reads; also ~10.9M `(w)=>` wrapper
   closures — the harness taxes itself, same for node). A megamorphic
   (shape,key)->slot stub cache in sv_getprop_by_key achieved a 96.5%
   hit rate — and was wall-time NEUTRAL (interleaved A/B: 42.5 vs 44.4
   mean; pure dict micro: 66.8 vs 70.0ms). The cost is helper-call
   framing + key conversion, not the final lookup; node's 6x on the
   micro (11ms) comes from inline ICs in optimized code, not a better
   hash. Reverted rather than keep unproven complexity in the hottest
   property path.
   **METHODOLOGY LESSON: an 8-second `sample` window lied — get_elem's
   "19%" is ~2.4% over the full run (30M x ~33ns ~= 1s of 42s). Only
   interleaved A/B counts as evidence for wall-time claims.**

**Object-churn installment 1 (2026-08-05, post-commit) — fused young
sweep+promote:** minor phase breakdown (ANT_GC_LOG, accumulated
full-run — trustworthy unlike `sample` windows): remember 0.41s, roots
1.69s, **obj sweep+promote 5.53s**, closure sweeps 2.69s. The sweep and
promote passes each pointer-chased the whole young list (~176B objects,
cache-miss bound) — fused into ONE walk (`gc_sweep_young_and_promote`,
detach-first + incremental old-list linking keeps lists consistent under
mid-walk finalizers; +prefetch of next). Phase timer: obj phase 5.53 →
**3.67s**. Main-level: 5-round interleaved A/B vs committed base mean
43.25 → 42.57s (−0.7s; faster 3/5 — thermal noise σ≈1s swamps it at
run level, the phase timer is the evidence). Roster prefetch in closure
sweeps tried, measured nothing, dropped. Gates: harness 168/0,
DeltaBlue 4119, spec/jit clean, GC micros green.

**Inline-emitter widening (2026-08-05, late) — mixed, decisive data:**
Rejection histogram (`[inline-reject]`, per blocked call site) showed
Richards blocked on PUT_FIELD/bitwise, DeltaBlue on CALL_METHOD (33
sites) + size cap. Implemented: side-effect-sound inline discipline
(side-effecting ops raise errors at the JOIN — never to `slow`, whose
generic fallback re-executes the callee; after the first effect only
never-slow ops + forward jumps allowed), jit_inline_ext_t plumbing, new
inline cases for PUT_FIELD / GET_ELEM / GET_LENGTH / BAND..USHR
(num-guarded) / CALL / CALL_METHOD / TAIL_CALL(_METHOD), cap 128→192.
Results: mechanical set KEPT (Richards 2631→2662-2687, others neutral,
all gates green). **Call-op inlining DISABLED after measurement:
DeltaBlue 4271→3976 (−7%) — inlining a method turns its nested calls
from devirtualized direct JIT->JIT (in its standalone compilation) into
generic helper calls. RESOLVED 2026-08-06: devirt-in-inline landed and
the CALL/CALL_METHOD/TAIL_CALL(_METHOD) SV_OPF_JIT_INLINEABLE flags are
back ON (see the regression-fix session section).** Next 7c step:
virtual objects (allocation sinking) for RayTrace/EarleyBoyer/newt-monad
wins.

**sv_closure_t bound/pending union (2026-08-05, late):** 136 → 120B.
`{bound_argv, bound_args}` and `{pending_name, pending_name_len}` share a
union discriminated by SV_CALL_HAS_BOUND_ARGS — the sides never coexist
because bind() results always materialize func_obj at creation while the
pending stash is only touched when func_obj == 0 (SET_NAME writers and
sv_closure_materialize_func_obj both check it; materialize additionally
guards on the flag). GC marking and BOTH sweeps now branch on the flag —
an unguarded free would free pending.name (code-arena memory). Audit
subtleties: the T_FUNC bind path COPIES orig->call_flags, so a re-bound
bound function inherits HAS_BOUND_ARGS with zero new args — union init
must branch on the final flags. A/B (interleaved, 2 rounds, vs pinned
fused-sweep base; includes the inline-widening diff): Main 42.1/42.9 →
**41.2/41.1 (−1.3s, −3.2%)**, both rounds faster. Full battery green;
newt RSS 959MB. **Pre-existing bug found during audit (NOT a
regression — reproduced on the pre-union binary): double-bind drops the
inner binding's this and args (`f.bind({x:1},10).bind({x:2},20)(30)` →
[2,20,30] instead of spec's this={x:1}, args [10,20,30]) — the T_FUNC
bind path flattens onto orig->func without folding orig's bound state.
File separately.**

**Server RPS A/B vs installed prod binary (2026-08-05, late; interleaved
3-round oha, ANT_TEST_BIN; prod = ca8e720d release without the stack):**
h3 **+15%** (20.8k vs 18.1k), elysia **+11%** (64.7k vs 58.0k), hono even
(30.2k vs 30.4k), **express −14% (13.5k vs 15.9k) — REAL, tight
variance, all rounds.** Bisect via pinned phase binaries: all of
base7/p7b/union sit in the same 13.6-14.1k band → the regression is in
the committed June+Phase6 base, not the day's phases. Profiles: build's
top entry is gc_run (majors) + ~2x per-request shape_lookup_interned;
prod shows NO GC in top-13 (it never collects the closure arena — trades
RSS for RPS). Two backstop fixes landed (battery green, newt 41.3s
unchanged):
1. Direct closure-watermark path in gc_maybe now tries a MINOR on new
   watermark highs (free-list reuse stalls the watermark; its drain rate
   becomes the minor cadence) and majors only when the watermark makes no
   new highs yet stays over budget. Root cause: raising the closure
   nursery 16k→128k inverted minor(131k roster) vs major(118k closures =
   16MB watermark) trigger order — servers got majors instead of minors.
2. The 50ms periodic force-backstop ran a FULL MAJOR each interval under
   steady sub-threshold allocation; now minors at 50ms, major at 1s
   (GC_FORCE_MAJOR_INTERVAL_MS).
**The IC-invalidation theory above was WRONG — falsified by counters
(ANT_IC_STATS per-family refill counts: 9 total refills across 100k
requests). The real cause, found via per-major-path counters
([gc-majors] atexit dump): the DIRECT `live >= gc_live_major_threshold`
check fired 2,775 majors in 11s (252/s). Mechanism: after a major,
threshold = ~1.5x post-major live; for a server's small live set that is
reachable by YOUNG churn alone, long before the 32k nursery triggers a
minor — when the adaptive `gc_use_nursery_major_floor` is off, every
~7k young objects bought a FULL MAJOR. Fix (third instance of the same
pattern): the direct live path runs a minor first and majors only if
live stays over threshold (genuine old-gen growth). Majors 2775 → 80
(all via the intended cadence path). RESULT: express 13.5k → 16.1k —
regression ELIMINATED; final interleaved scoreboard vs prod: hono +3.6%,
express even, h3 +15%, elysia +11% — build wins or ties all four with
bounded closure memory. newt unchanged (41.5s). Battery green.
ANT_IC_STATS=1 keeps the objepoch-refill + gc-majors atexit dump.**

**Micro/demo/GC-bench A/B vs prod (interleaved, 2 rounds, wall+RSS):**
wins — fibonacci_recursive −33%, bench_gc_mark_func −36%, bench_dec −26%,
bench_epoch −19%, test_async_gc −10%, bench_churn −4%; flat — pi,
mandelbrot, event_loop, bench_gc, test_gc_large, test_gc_comprehensive.
game-of-life RSS sampler (`ant game-of-life/sample.js 10 <bin>
game-of-life/dist/play.js`, 10s, 21 samples): identical sawtooth envelope
both binaries, no growth trend; build avg 32.7MB (23.7–42.1) vs prod
29.9MB (22.4–39.6) — +~3MB avg from the 128k closure-nursery working set,
same signature as the churn benches. Throughput (interleaved, 2x12s,
play.js direct): build 4216/4186 ticks vs prod 3939/3910 = **+7.0%**;
World Tick avg 1.71 vs 1.79ms (−4.2%), Rendering avg 1.13 vs 1.25ms
(**−9.8%** — the string-builder/literal-shape-heavy half). Net: ~7%
faster for ~3MB more average RSS.
**Two regressions in synthetic GC stress tests: test_gc_async +72%
(0.87→1.51s), test_gc_coro +10% — RESOLVED 2026-08-06 (see the
regression-fix session section). The epoch-deopt theory was WRONG; the
mechanism (pinned by ANT_REGEX_STATS counters) was the obj-keyed
regex_cache's LINEAR lookup scan: regex literals create a fresh owner
object per evaluation, entries pile up until a sweep, and the evening
GC fixes made majors much rarer — 2.4e9 scan iterations over the run.
Fixed with a pointer-keyed open-addressing index; async 1.51→0.82s
(better than the union bisect point), coro 7.07→6.37s. The
generation-aware gc_sweep_regex_cache fix was kept as instructed.**

**Regex vs prod (tests/bench_regex.cjs, interleaved): −22% overall —
FALSIFIED as a code regression 2026-08-06.** The offset-conversion
theory was WRONG twice over: (a) `src/utf8.c` is byte-identical between
prod's commit (ca8e720d) and HEAD, and (b) bench_regex's "unicode
corpus" is pure ASCII — the utf16 conversion paths never execute in the
regressed benches. The real cause is a BUILD artifact: macos-aarch64
releases are built with `-Dpgo=enabled` (.github/versions.json) and
prod's PGO profile matched its source. Evidence (fixed-work token-scan
micro): prod 673-689ms; pre-stack master 66499b0b built locally
non-PGO 848-856ms (identical "regression" with none of the stack);
66499b0b built WITH the checked-in profdata 700-705ms (= prod); current
tree with the (now stale) profdata 795-832ms. The u16_idx index is
therefore NOT a fix for any current regression — it stays an
enhancement for non-ASCII positional ops, still gated on its +50%
sequential cost. Action: regenerate meson/pgo/profiles from post-stack
source at release time; never compare local non-PGO builds against the
installed prod binary for C-runtime-heavy microbenches (new trap).

**Remaining, updated ranking (now in the ~1s-class flat zone for GC
mechanics):**
1. Object-churn deeper cuts: mkobj-uninit (7a-style explicit-init audit,
   ~176B memset x ~100M objects), slimmer ant_object_t. Each ~1s-class.
2. Closure-sweep residual 2.6s + roots 1.7s — diminishing.
3. Real monad inlining = general call-target-directed inlining +
   allocation sinking in MIR (weeks; the long-term-correct machinery).
   The ~78M monad closures/records are only reachable this way.
4. The strategic fork: node-class needs an optimizing tier, not more
   phases (last ~8x is codegen quality).

~2.5× on newt if everything lands (June: Prelude 6.2→2.47s, Main 70→~36s
across the rounds), Octane geomean from ~1692 toward the June ~2600-class
scores. The primitive-IC port already landed separately (charCodeAt 4.5×,
primitive-miss path 7×).

## Regression-fix session (2026-08-06) — every open R-item closed

All changes uncommitted on `perf/vm-dispatch` on top of 50f99260. Final
battery on the finished tree: spec 3712/0, jit 9 files/0 fail, harness
**170**/0 (168 + test_double_bind + test_numeric_literal_keys; express
oha 16.4k ≥ 16k), devirt fuzzer all seeds agree, newt Main in band
(41.2-45.3s across the day — late-day readings ran hot; interleaved
final-vs-r3-pin showed no systematic direction, r3 itself swung
43.4/46.0), RSS 565-666MB.

**R1 test_gc_async +72% / test_gc_coro +10% — FIXED.** Counters before
code (new `ANT_REGEX_STATS=1`, kept, atexit dump like ANT_IC_STATS):
1.4M regexp_exec_internal calls, 200k obj-cache misses+inserts (fresh
regexp-literal owner object per replace call), **2.4e9 linear scan
iterations** in regex_cache_lookup, max cache 3560 entries, 56
compiles. The prior theories (epoch-deopt of an intrinsic, per-call
regex speed) were wrong: the union bin was only "fast" because its
pathological direct-path major rate (the express disease, 252/s)
constantly wiped this cache; the evening GC fixes made majors rare and
exposed the O(n) scan × O(n) entries growth.

The first fix (pointer-keyed open addressing over the owner array) proved
the mechanism and restored async to ~0.82s. It is now superseded by the
closer-to-V8 endpoint: each executed RegExp object stores its shared
compiled entry directly in the object's native payload and releases that
reference from its finalizer. Compiled PCRE2 data is shared through a
per-isolate, bounded two-generation hash cache keyed by `(source, flags)`
using `hash_key`/`ant_hash_mix`; majors age generation 0 into generation
1 and discard the old generation 1, while minors do nothing. Match data
uses a scoped reusable buffer; a nested use dynamically allocates its own
buffer. The scope remains borrowed until the caller has consumed PCRE2's
offset vector, avoiding a per-match capture copy. Match context, JIT stack,
and RegExp prototype-mutation guards are per-isolate too. There is no
owner table, tombstone, sweep walk, or rehash/rebuild path left.

Pinned binaries for the architectural A/B:
`/tmp/ant_r1_owner_bin` (`16fd839fb488cb139ec9e132677a3f36`)
versus `/tmp/ant_r1_native_deliver_bin`
(`cdd2b19cef0d171d2b777de2bf247e40`). Lower is better; the delta uses
the median so the first 160.18ms route sample cannot inflate the claim.

| Workload | Owner-table samples (ms) | Native-payload samples (ms) | Median delta |
|---|---:|---:|---:|
| `test_gc_async` | 849.16, 846.32 | 840.64, 827.66 | **−1.6%** |
| `test_gc_coro` | 6523.77, 6768.44 | 6468.42, 6474.01 | **−2.6%** |
| regex route matches | 160.18, 134.90, 133.57, 137.99 | 132.58, 133.38, 133.96, 134.92 | **−2.0%** |
| regex token scan | 846.06, 858.19, 830.42, 837.87 | 812.96, 825.56, 796.29, 831.16 | **−2.7%** |
| regex identifier split | 961.19, 971.05, 959.73, 974.53 | 955.70, 952.54, 944.50, 973.34 | **−1.2%** |
| compile route regexp | 7.11, 7.68, 7.23, 7.08 | 7.01, 7.16, 7.29, 7.98 | +0.8% (noise) |

The final counter signature is the same for async and coro: 1.4M execs,
1.2M direct object-data hits, 200k first-use attaches, 199,943
generation-0 compiled-cache hits, 56 generation-1 hits/promotions, one
compiled-cache miss/compile, and zero dynamic match-data allocations.
The regex micro records 19,590,900 direct hits after three attaches,
three compiles, and zero dynamic allocations.

Gates on the final tree: async 0.82-0.84s (≤0.95s), express **16,189
RPS** (≥16k), harness 170/0, and newt below 1GB. The late-day newt
interleave ran hot for both pins: owner Main 43.322/44.818s versus final
44.415/44.880s; paired deltas +1.093s/+0.062s are within the documented
~1s thermal sigma. Peak RSS averaged 960MB owner versus 951MB final.
The temporary GC-stress hook was removed after focused regexp tests
passed at stress 1/3 and the full spec passed 3712/0 at stress 10.

**R1 follow-up — direct internal regexp state, 2026-08-06.** The
V8-style direct-state experiments separated into two wins and one
falsification:

1. `regexp_flags_mask` now reads the attached compiled entry's immutable
   `flags_mask`, or the regexp object's internal mask before first compile.
   The public `flags` property is only a fallback for objects without internal
   regexp state. This is both faster and semantically correct: shadowing the
   public property cannot change `RegExpBuiltinExec`'s global/sticky behavior.
   Counters on `bench_regex`: 19,590,903 calls, 19,590,900 direct compiled
   reads, three internal-slot reads, zero cached-public reads and zero
   reparses. Isolated non-PGO interleaving versus the pre-change native-payload
   binary:

   | Workload | Before (ms) | Direct flags (ms) | Median delta |
   |---|---:|---:|---:|
   | `test_gc_async` | 850.74, 859.40 | 826.60, 834.52 | **−2.9%** |
   | `test_gc_coro` | 6863.12, 6854.88 | 6829.38, 6831.98 | −0.4% |
   | regex route | 143.14, 144.60, 144.41, 144.55 | 132.79, 130.15, 128.87, 130.95 | **−9.6%** |
   | regex token scan | 881.92, 865.63, 883.38, 873.81 | 727.68, 732.07, 714.98, 715.85 | **−17.8%** |
   | regex identifier split | 1026.84, 997.77, 1032.26, 1021.34 | 833.67, 823.17, 810.54, 813.61 | **−20.1%** |

2. `lastIndex` now has a guarded direct location cached on the shared compiled
   entry. It is usable only for a non-proxy, non-exotic receiver with the
   exact retained shape and an own writable data property at the cached slot.
   Shape changes, accessors, read-only descriptors, proxies, and exotic
   objects fall back to normal property semantics. The direct store still
   executes the GC write barrier. `bench_regex` records 19,590,900 guarded
   reads and 19,590,903 guarded writes out of 19,590,903 reads/writes total;
   the first three reads populate the compiled entries. A new Node-parity
   regression test covers public-flags shadowing, add/delete shape changes,
   non-writable `lastIndex`, and alternating differently-shaped regexp
   objects sharing the same compiled data.

3. **Literal boilerplate sharing was WRONG for this runtime and was
   reverted.** A feedback-site compiled-entry cache eliminated the async
   workload's 200,000 per-object compiled-cache lookups exactly (1.4M direct
   object-data hits, zero misses, one compiled-cache miss), but added site
   lookup/reference work on every fresh literal. Interleaved direct-flags
   versus literal-site candidate: async 841.98/839.68ms versus
   850.45/862.54ms (**+1.9%**, worse); coro 6872.71/6952.32ms versus
   6931.35/6917.32ms (flat). The entire experiment was removed; no literal
   site table or compiled-entry site references remain.

The final direct-flags + guarded-`lastIndex` source was retrained with a fresh
macOS AArch64 PGO profile. Pinned hashes:
pre-experiment fresh-PGO R1
`/tmp/ant_r1_native_fresh_pgo_bin`
(`f3482f161609ac411c03aef2d32e21d4`), final
`/tmp/ant_r1_direct_state_fresh_pgo_bin`
(`0ef8fdec844213b0d6f3f934ecadf24c`), installed ca8e720d
`/tmp/ant_installed_ca8e720d_bin`
(`33ea23ff2faa4c7f6c3302aff2e2d279`). Every row is AB/BA interleaved;
two-sample medians are shown.

| Workload | Pre-experiment R1 (ms) | Final direct state (ms) | Median delta |
|---|---:|---:|---:|
| `test_gc_async` | 768.62, 772.48 | 640.59, 632.83 | **−17.4%** |
| `test_gc_coro` | 6419.91, 6418.73 | 6293.78, 6284.11 | **−2.0%** |
| regex route | 115.46, 122.14 | 94.19, 96.19 | **−19.9%** |
| regex token scan | 646.09, 642.35 | 420.66, 426.31 | **−34.3%** |
| regex identifier split | 709.05, 707.16 | 472.92, 477.48 | **−32.9%** |

| Workload | Installed Ant (ms) | Final direct state (ms) | Median delta |
|---|---:|---:|---:|
| `test_gc_async` | 914.34, 931.89 | 653.80, 632.72 | **−30.3%** |
| `test_gc_coro` | 6644.40, 6663.48 | 6290.59, 6374.79 | **−4.8%** |
| regex route | 142.93, 139.00 | 96.50, 97.07 | **−31.3%** |
| regex token scan | 729.27, 724.29 | 489.15, 428.97 | **−36.8%** |
| regex identifier split | 752.11, 758.98 | 477.80, 470.71 | **−37.2%** |

Final gates after fresh PGO: spec 3712/0, JIT 9 files/0 fail, harness
**171/0** (including the new parity test; async 646ms, coro 6287ms),
express oha **17,797 RPS**, devirt fuzzer all seeds agree, and newt Main
43.034s / 706MB max RSS (902MB peak footprint). A temporary
`ANT_GC_STRESS` hook was re-added because the direct location retains a shape:
the new and existing regexp fast-path tests passed at stress 1/3 and the full
spec passed 3712/0 at stress 10; the hook was removed before finishing.

Bonus observed while instrumenting: the [gc-majors] atexit dump does
NOT count the direct `gc_run` at src/pool.c:311 (js_type_alloc
pool-pressure) — 24/25 majors in this workload were uncounted; add a
counter if majors need auditing again.

**R1 follow-up — canonical results and batched global execution,
2026-08-06.** Counters rejected the initial assumption that canonical result
allocation would speed `bench_regex`: that benchmark uses the truthy-only
`while (re.exec(...))` intrinsic and creates **zero** result arrays. A
fixed-work result-consuming micro instead records 1,602,000
`regexp_exec_internal` calls, 1,600,000 result arrays and 3,200,000 capture
slots. A separate global `match` + string `replace` micro recorded 3,204,000
execs, 3,200,000 generic result arrays, 6,400,000 capture slots, 1,600,000
results discarded by `@@match`, and 1,600,000 materialized only so
`@@replace` could consume their offsets.

Two changes landed:

1. Ordinary and `d`-flag exec results use one canonical per-isolate array
   shape for `index`, `input`, `groups`, and optional `indices`. The shape is
   built off a fresh array shape, retained by the isolate, installed before
   direct slot writes, and released at isolate cleanup. Stores retain the GC
   write barriers. Named-group accessor construction remains on the generic
   path. Property order and writable/enumerable/configurable descriptors match
   Node.
2. Built-in global `@@match` and string-replacement `@@replace` can iterate
   PCRE2 offsets directly. `@@match` materializes only the final matched
   strings; `@@replace` streams unmatched spans and substitutions into the
   output buffer. Neither creates an intermediate JS exec-result object.
   The guard requires a real non-proxy RegExp with internal slots, the exact
   built-in data-property `exec`, internal `g`, compiled data, and a guarded
   writable own `lastIndex` location. Accessors, custom exec, proxies,
   read-only or shape-changed `lastIndex`, non-global regexps, and function
   replacers use the generic semantic path.

The isolated canonical-shape A/B used counter-only
`/tmp/ant_regex_result_stats_base_bin`
(`7f7fe1656191fa8375df653d3b2a`) and shape candidate
`/tmp/ant_regex_result_shape_bin`
(`94e318b6bec32cd4abe1908096fa478a`). Four AB/BA rounds:

| Fixed work | Counter base (ms) | Canonical shape (ms) | Median delta |
|---|---:|---:|---:|
| 1.6M materialized `exec()` results | 336.29, 333.96, 333.36, 342.26 | 259.13, 260.88, 255.16, 262.98 | **−22.4%** |

The final source was retrained with a fresh macOS AArch64 PGO profile.
Pinned hashes: prior direct-state
`/tmp/ant_r1_direct_state_fresh_pgo_bin`
(`0ef8fdec844213b0d6f3f934ecadf24c`) versus final
`/tmp/ant_regex_result_batch_fresh_pgo_bin`
(`c6b47c2fc9d00ae0c54dd0913f57cfc0`). Four AB/BA rounds for the regex
micros and two for the long GC rows:

| Workload | Prior fresh PGO | Final fresh PGO | Median delta |
|---|---:|---:|---:|
| 1.6M materialized `exec()` results | 314.31, 313.04, 309.33, 311.21 ms | 238.67, 244.21, 248.40, 243.77 ms | **−21.8%** |
| 2k global match + replace rounds | 617.95, 626.41, 620.67, 623.56 ms | 140.71, 136.61, 142.08, 142.70 ms | **−77.3%** |
| `test_gc_async` | 0.62, 0.62 s | 0.38, 0.38 s | **−38.7%** |
| `test_gc_coro` | 6.19, 6.20 s | 5.96, 5.96 s | **−3.8%** |

The installed `$PATH` binary was separately pinned as
`/tmp/ant_installed_regex_result_compare_bin`
(`33ea23ff2faa4c7f6c3302aff2e2d279`). Four AB/BA rounds:

| Workload | Installed Ant (ms) | Final fresh PGO (ms) | Median delta |
|---|---:|---:|---:|
| 1.6M materialized `exec()` results | 405.20, 395.02, 411.43, 412.53 | 264.63, 259.86, 269.77, 268.70 | **−34.7%** |
| 2k global match + replace rounds | 838.63, 825.65, 843.63, 838.47 | 148.80, 159.74, 152.86, 161.52 | **−81.4%** |

The final global micro has the same checksum while recording zero
`exec_internal` calls, zero generic result arrays, 2,000 batched match calls
with 1.6M results, 2,000 batched replace calls with 1.6M results, and zero
guard rejections. The result-consuming micro records 1.6M canonical-shape
hits and zero fallbacks.

Eight interleaved fresh-PGO rounds on the existing truthy-only rows found no
actionable regression. The machine shifted into a 7-10% slower thermal band
halfway through both binaries; across all rounds, compile was −0.1%, route
+1.5%, token scan +0.8%, and identifier split +0.1%. Those deltas are within
the established noise floor and, as the counters show, do not exercise either
new optimization.

`tests/test_regexp_result_batch.cjs` covers canonical shape/order/descriptors,
captures and indices, global/sticky/Unicode-empty match and replace,
substitutions and RegExp statics, plus read-only `lastIndex`, accessor/custom
exec, and function-replacer fallbacks. It matches Node. Focused regexp tests
passed under a temporary `ANT_GC_STRESS=1/3` hook. Full stress 10 reached the
already-documented worker-teardown null-PC SIGSEGV after all relevant spec
work; the hook was removed and the ordinary full spec passed.

Final gates on the exact fresh-PGO artifact: spec 3712/0, JIT 9 files/0 fail,
harness **172/0** (`test_gc_async` 398ms; express oha **18,048 RPS**),
devirt fuzzer all four seeds agree, and newt Main 43.418s / 727MB max RSS
(933MB peak footprint). The regenerated profile and source remain
uncommitted.

**R2 regex −22% vs prod — FALSIFIED, no code change needed** (see the
amended entry above for full evidence). PGO build artifact:
macos-aarch64 prod is `-Dpgo=enabled` with profile data matching its
source; identical "regression" reproduces on pre-stack master built
locally, and disappears when 66499b0b is built with the checked-in
profdata. bench_regex's regressed benches never execute utf16
conversion code (ASCII corpus) and src/utf8.c is unchanged vs prod, so
the u16_idx plan does not apply. The sequential-scan micro (Buffer.from
ucs2 non-ASCII 100KB) is at parity with prod (19ms both) — no
sequential regression exists on this tree either. The bench-v8 RegExp
0.94x item (local-vs-local vs /tmp/ant_fable_base, so real) stays
deferred with its two suspects (js_as_obj func_obj==0 branch, arr_get
dense pre-check); note today's tree WINS the fixed-work token-scan
micro vs a fresh local 66499b0b build (805-835 vs 848-856ms), so
whatever remains is workload-specific to bench-v8's regexp.js.

**R3 DeltaBlue call-op inlining — LANDED (devirt-in-inline).** The
inline emitter's CALL/CALL_METHOD/TAIL_CALL(_METHOD) case now emits the
main emitter's known-target dispatch before falling back to the generic
helpers: super-value compare → tag check/closure extract →
HAS_BOUND_ARGS → func load → resolve_call_this → jit_code load → direct
call through `self_proto` (new field in jit_inline_ext_t, populated at
both construction sites). SV_OPF_JIT_INLINEABLE re-enabled on all four
call ops in opcode.h. Interleaved 3-round A/B vs same-day pre-R3 pin:
DeltaBlue 4192/4247/4251 vs 4122/4161/4208 (flags-on now BEATS
flags-off; the old 4271 bar was that day's thermal band — today's
flags-off baseline measures ~4160), Richards 2637-2644 vs 2623-2658
(holds), Crypto/Splay even, RayTrace −1-3% (one 2781 outlier in 5
rounds, rest 2992-3086 vs 3010-3108 — noise-adjacent), EarleyBoyer
6112 vs 5956 and NavierStokes 2764 vs 2633 (single runs, positive).
Devirt fuzzer clean, full battery green.

**R4 double-bind — FIXED + tests.** builtin_function_bind's T_FUNC path
now keeps the inner binding's `bound_this` and concatenates orig's bound
argv BEFORE the new args (`inner.args ++ outer.args ++ call args`). The
union is switched to the bound side (argv=NULL/args_arr=undef) BEFORE
mkarr can GC. A review follow-up found that the first version still used
`undefined` as both a valid bound-this value and the "not bound" sentinel:
an expanded deterministic bind-chain matrix reproduced **45/182**
failures across direct and method calls. `SV_CALL_HAS_BOUND_THIS` now
records the state explicitly in native, proxy, interpreter, tail-call,
and JIT call paths. The final matrix adds native/proxy, explicit-call,
tail-call, and constructor cases and passes **187/187 under both node
and Ant**.
`f.bind({x:1},10).bind({x:2},20)(30)` → this={x:1}, args [10,20,30] =
node; triple bind, cfunc-target double bind (Math.max/min), arrow
folding, name/length chain, constructors, and `undefined` bound-this
retention all match node. tests/test_double_bind.cjs is in the harness
REGRESSION_TESTS and retains the GC-churn survival check.

**R4 follow-up — re-bound Proxy argument duplication FIXED,
2026-08-06.** An independent parity pass found one hole in the 187-case
matrix: a bound Proxy with inner arguments was flattened into the outer
closure, but the copied `builtin_bound_proxy_call` wrapper still targeted
the inner bound function. The outer closure prepended the folded arguments,
then re-entering the inner closure prepended its arguments a second time:
`px.bind(A, 1).bind(B, 2)(3)` produced `[1, 1, 2, 3]` instead of
`[1, 2, 3]`; three bind levels compounded the duplication.

The fix keeps the normalized folded closure and, only when the copied C
entrypoint is `builtin_bound_proxy_call`, retargets `SLOT_TARGET_FUNC` to
that wrapper's original Proxy. This preserves the Proxy apply/construct
traps while invoking them exactly once; reverse nesting (a Proxy around a
bound function) is unchanged. The checked-in deterministic matrix expanded
from 187 to **360 assertions** by giving Proxy binds the same inner/outer
`this` x argument-shape x direct/method coverage as ordinary functions,
plus triple-bind and constructor trap-count cases. All 360 pass under both
Node and Ant.

Pinned same-work execution A/B (10M calls, identical checksum
`1284991808` and 10M apply traps): pre-fix
`/tmp/ant_r4_proxy_base_bin`
(`64bb915ea970449bde866cb87575c68d`) versus candidate
`/tmp/ant_r4_proxy_candidate_bin`
(`520d763a4b3c25ab8a2a9a9d807c6ddf`). Four AB/BA rounds measured
1125.978/1126.550/1108.872/1142.906ms versus
1007.076/1020.279/1021.364/1006.123ms; median
**1126.3 -> 1013.7ms (-10.0%)**. Removing the redundant wrapper traversal
improves performance as well as parity.

Final gates on the candidate: spec 3712/0, JIT 9 files/0 fail, devirt
fuzzer all four seeds agree, and harness **172/0** (168 substantive rows
plus all four oha rows after unsetting this shell's incompatible
`NO_COLOR=1`; express 18,491 RPS). Async rows include gc_async 382ms and
gc_coro 5.939s. newt completed in 44.09s wall with 966,197,248-byte max
RSS (<1GB). The ordinary `build/` configuration still fails in the current
Nix shell because the SDK cannot find `time.h`; the already-configured
`build-pgo/` tree produced and validated the candidate.

**R4 parity follow-up — bound prototypes/generators, sloppy `this`, and
full RegExp captures FIXED, 2026-08-06.** The incidental parity audit found
four older release defects:

1. Bound generator functions were dispatched through the wrapper's missing
   generator prototype, so `[...g.bind(obj)()]` threw instead of iterating.
2. Sloppy functions received primitive `this` values unchanged instead of
   boxing String/Number/Boolean/BigInt/Symbol values (and mapping nullish
   values to the global object).
3. Bound functions incorrectly had an own `prototype`; repeated binding then
   made `new` use the wrapper prototype and wrapper `new.target` instead of
   the ultimate target's.
4. RegExp result and replacement paths silently stopped materializing
   captures after capture 31.

Bound metadata now names the ultimate target and is resolved consistently by
ordinary/native/proxy calls, generator dispatch, `instanceof`, and all
interpreter/JIT construct paths. Bound functions no longer create an own
`prototype`. Construction normalizes an implicit bound wrapper `new.target`
to the target while preserving an explicit distinct
`Reflect.construct(..., newTarget)`; Proxy construct traps still run exactly
once. Sloppy non-arrow functions normalize `this` in the interpreter and JIT,
including fresh rooted primitive wrappers. The JIT only emits that work for
functions whose bytecode observes or captures `this` (`OP_THIS`,
`OP_CLOSURE`, or `OP_EVAL`), leaving unrelated and strict functions alone.

RegExp result loops now use the complete PCRE2 ovector. Template replacement
and function-replacer capture storage grows dynamically past the old stack
capacity, so capture 40 works in `exec`, `d` indices, `$40`, named groups,
and replacer arguments. The deterministic bound suite now passes **394/394**
under both Node and Ant; `test_regexp_result_batch.cjs` covers 40-capture
result/replacement behavior.

The final source was retrained with the checked-in Darwin AArch64 PGO script.
Pinned identities: pre-follow-up
`/tmp/ant_bound_regex_baseline_bin`
(`520d763a4b3c25ab8a2a9a9d807c6ddf`), fresh-PGO candidate
`/tmp/ant_bound_regex_followup_fresh_pgo_bin`
(`fde1bbba0c47c82eefca90f2629392d5`), and installed release
`/Users/themackabu/.ant/bin/ant`
(`33ea23ff2faa4c7f6c3302aff2e2d279`). Two alternating AB/BA rounds of
fixed work measured:

| Workload | Pre-follow-up median | Fresh PGO median | Delta |
| --- | ---: | ---: | ---: |
| Bind creation, 1M | 599.910ms | 220.755ms | **−63.2%** |
| Double-bound call, 20M | 242.154ms | 242.403ms | +0.1% / flat |
| Double-bound `new`, 5M | 299.450ms | 293.483ms | −2.0% |
| Sloppy primitive `this`, 5M | 122.811ms | 488.838ms | **3.98x slower** |
| Strict primitive `this`, 20M | 208.125ms | 208.071ms | flat |
| RegExp 20 captures, 600k | 314.735ms | 310.769ms | −1.3% / flat |

The sloppy row is an unavoidable equal-work but **not equal-semantics**
comparison: the old binary skips the required wrapper allocation. The cost is
confined to sloppy functions that observe or capture `this`; strict functions
and sloppy functions that never use it do not pay the normalization.

| Workload | Installed median | Fresh PGO median | Delta |
| --- | ---: | ---: | ---: |
| Bind creation, 1M | 602.934ms | 220.929ms | **−63.4%** |
| Sloppy primitive `this`, 5M | 139.342ms | 485.540ms | **3.49x slower, required semantics** |
| Strict primitive `this`, 20M | 258.029ms | 209.231ms | **−18.9%** |
| RegExp 20 captures, 600k | 398.396ms | 309.152ms | **−22.4%** |

Installed double-bound call/construct rows are excluded: their checksums
differ because the installed release has the incorrect binding semantics.
Final exact-artifact gates: spec 3712/0, JIT 9 files/0 fail, harness
**172/0** (`test_gc_async` 394ms, `test_gc_coro` 6.499s, express oha
18,414 RPS), devirt fuzzer all four seeds agree, and newt Main 43.139s /
43.52s wall / 732,217,344-byte max RSS (931,497,112-byte peak footprint).
The final routine `meson compile -C build` attempt still cannot link in the
current Nix shell (`ld: library not found for -lm`); it removed `build/ant`,
so the byte-identical validated pin was restored there and its MD5 rechecked.

**R4 construction follow-up — bound Proxy/newTarget/class parity and
cycle-safe target resolution FIXED, 2026-08-07.** The remaining construction
audit found that Proxy and bound-Proxy paths did not consistently implement
`IsConstructor`, `GetPrototypeFromConstructor`, or bound-function
`newTarget` normalization. This affected Proxy construct traps that forward
with `Reflect.construct`, `Reflect.construct(..., boundProxyNewTarget)`,
Proxy-of-bound constructors, `class extends boundProxy`, and explicit
`extends undefined`/non-constructors. The compiler now records whether class
heritage was explicit; class creation validates Proxy constructors and
prototype values; Reflect and Proxy construction validate constructors,
preserve an explicit distinct `newTarget`, normalize an implicit bound
wrapper to its target, and fall back to `Object.prototype` when the selected
constructor prototype is not an object. The deterministic Node/Ant parity
suite now passes **421/421** on both engines.

The bound-target resolver no longer has the arbitrary 64-edge cutoff.
`bind()` flattens `SLOT_TARGET_FUNC`, so the normal path performs one raw
`get_slot` and returns the terminal target. Only an unexpected nested bound
target enters a noinline/cold Floyd walker; a cycle is an explicit internal
fatal invariant violation rather than a hang or a silently truncated target.
Both the successor helper and walker are marked cold: the first version let
LTO inline the entire walker into the hot resolver (about 0x720 bytes of
emitted code), so it was rejected before final measurement. The resolver
continues to use private `get_slot`; it does not cross the public
`js_get_slot` abstraction.

Pinned same-configuration identities: capped baseline
`/tmp/ant_r4_construct_cyclecap_samebuild_bin`
(`5814b78d65d37e79ee0c178a098dc87a`), exact final
`/tmp/ant_r4_construct_cyclecheck_final_bin`
(`20c91ea2d59807bb95c0123cdba54016`), and shared PGO data
`900b4eb6b166aad116a8cca7369f36e9`. Four alternating AB/BA rounds on
the cycle-safe implementation before the final cold-section-only placement,
plus two exact-final confirmatory rounds, produced mixed sub-1.2% deltas with
identical checksums:

| Workload | Capped 4-round median | Cycle-safe 4-round median | Delta |
| --- | ---: | ---: | ---: |
| Bound `new`, 20M | 1168.0ms | 1169.4ms | +0.1% / flat |
| Double-bound `new`, 20M | 1178.2ms | 1170.3ms | −0.7% / flat |
| Reflect bound construct, 10M | 1150.2ms | 1159.4ms | +0.8% / flat |
| Bound-Proxy `new`, 20M | 2918.8ms | 2887.7ms | −1.1% / flat |

The exact-final two-round means were −0.8%, −0.9%, +1.2%, and +0.9%
respectively, again mixed and within thermal noise. Final exact-artifact
gates: spec **3712/0**, JIT **9/0**, harness **172/0**
(`test_gc_async` 398ms, `test_gc_coro` 5.915s, express 19,083 RPS),
devirt fuzzer all four seeds agree, and newt Main **42.28s** /
843,808,768-byte max RSS (916,948,144-byte peak footprint). `maid preflight`
and `git diff --check` are clean. This shell's `NO_COLOR=1` is incompatible
with oha 1.14.0's strict boolean environment parser; the unchanged harness
passes with `NO_COLOR=true`.

**R5 numeric literal keys — FIXED + tests.** New `literal_num_key()`
(compiler.c) routes all three %g sites (compile key emit ~276,
object_literal_static_keys ~4056, DEFINE_FIELD fallback ~4160) through
`ant_number_to_shortest` with NaN/±Infinity handling (1e999 literals
overflow to Infinity). `{123456789:1}` → key "123456789";
Object.keys ordering and exponent forms (1e21, 9007199254740993
rounding) match node exactly. tests/test_numeric_literal_keys.cjs in
the harness manifest.

**R6 adaptive closure nursery — REJECTED with data, reverted.** The
premise no longer holds: on today's tree Prelude at the fixed 131072
threshold measures **~3.0s**, not the 3.6s recorded when the trade was
banked — the +0.75s penalty was absorbed by later work (7a/fused
sweep era). Measurements: per-minor survival (promoted/roster,
ANT_GC_LOG) does NOT separate the phases (Prelude-only run: mean 3.74%,
p50 2.64%; full run: mean 5.84%, p50 5.41% — overlapping), so
survival-based sizing is unsound. A cumulative-allocation-based
adaptive version (start 16384, double when total closure allocs cross
8M/16M/32M, cap 131072; Prelude allocates ~7.6M total so it stays
small) was implemented and interleaved-A/B'd: Prelude FLAT (3051/2933
vs 3015/2998), Main **+1.4s both rounds** (44290/41957 vs 42826/40670).
Fails the "both phases improve" gate on both ends — reverted cleanly.
Do not retry without first re-establishing that a Prelude penalty
exists at all.

**Cleanup.** (1) examples/bench-v8/index.js spawns benchmark children through
`process.execPath` — runner-level A/Bs automatically inherit the exact pinned
runner binary and cannot become self-vs-self through a stale env override.
(2) ANT_GC_STRESS validation debt paid: temporary hook added to
gc_maybe (shape from completed/module-import-gc-flake.md), battery run,
hook REMOVED. Results: spec 3712/0 under stress=10; new regression
tests + test_gc_async pass under stress=1/3. The intermittent full-spec
stress=3 SIGSEGV (PC=0 in `uv__stream_io`) was reproduced IDENTICALLY on
clean pre-stack master 66499b0b, but the original worker-teardown
attribution was WRONG. `ANT_WT_TRACE` showed a successful spawn and no
worker exit/teardown callback before the crash. The minimal reproducer was
`websocket worker_threads`: a remote WebSocket close removed the socket
from `g_active_websockets`, forced GC finalized its native state while the
tlsuv transport was still live (`tr != NULL`), and the next loop turn
serviced the stale transport. The fix keeps the WebSocket rooted until
tlsuv transport close completion and makes tlsuv defer its close callback
until that completion. Evidence: the former reproducer is clean 20/20
with tracing plus 10/10 without tracing at stress=3; the full 3712-test
spec is clean twice at stress=3. The temporary stress and trace hooks were
removed.

WebSocket performance/resource follow-up versus installed release
`/Users/themackabu/.ant/bin/ant` (`ca8e720d`, pinned MD5
`33ea23ff2faa4c7f6c3302aff2e2d279`; candidate pinned MD5
`c29b799b8a18f09f67760cc69ea7d0d1`): after clearing localhost
`TIME_WAIT`, four alternating interleaved rounds of 50 connections ×
10,000 echoed messages measured installed 3.65/3.64/3.64/3.70s versus
candidate 3.63/3.64/3.67/3.72s. Means 3.658s versus 3.665s, **+0.2% /
flat**. The sockets were retained only to keep the release bug from
invalidating the timing comparison. On the close-lifecycle workload,
2,000 completed retained sockets left **2,010 open FDs** on installed
Ant versus **10** on the candidate. Without retention, installed Ant
SIGSEGVs during natural GC before completing 4,000 cycles while the
candidate completes; with retention, installed Ant reaches `EMFILE`
before 6,000 cycles while the candidate completes 10,000. The fix has
no measurable message-throughput cost and removes the native transport
leak/crash.

**Property-delete follow-up — ordered tombstones FIXED, 2026-08-07.**
The insertion-order correctness fix had made every successful delete shift
all later shape metadata and object values, then walk the entire uthash index
to decrement later slots. Whole-run diagnostic counters on the adaptive
`prop_delete` micro recorded 1,880,000 deletes and **939,060,000** visits in
each of those three loops (index entries, shape properties, and value slots).
The counters were diagnostic only and are not retained in the final source.

Deletion now removes the key from the shape index, clears the corresponding
value, and leaves an ordered tombstone. Re-adding the key appends a new slot,
which preserves ECMAScript insertion order. Trailing tombstones collapse
immediately; after at least 32 interior tombstones, a stable compaction runs
when holes equal or exceed live slots. Compaction updates each live hash
entry's slot without allocation, so the object remains usable under memory
pressure and its physical property span stays bounded. `ant_shape_prop_at`
exposes tombstones as absent, and the existing global IC epoch bump
invalidates slot caches before any moved slot can be reused.

Pinned exact identities: pre-change
`/tmp/ant_propdelete_current_bin`
(`20c91ea2d59807bb95c0123cdba54016`) and final
`/tmp/ant_propdelete_landed_bin`
(`b84cabf7cf3c3c9dfc24fa659ad866f0`). Two alternating AB/BA rounds measured:

| Workload | Pre-change | Final tombstones | Delta |
| --- | ---: | ---: | ---: |
| adaptive `prop_delete` | 1026.10, 1022.20 ns | 186.90, 184.18 ns | **−81.9% / 5.52x** |
| 1k numeric, delete front | 1057.22, 1018.81 ns | 88.21, 94.35 ns | **−91.2%** |
| 1k numeric, delete back | 455.04, 469.36 ns | 61.77, 66.12 ns | **−86.2%** |
| 1k numeric, random delete | 850.69, 886.76 ns | 108.42, 114.25 ns | **−87.2%** |
| 2k numeric, delete front | 4149.41, 4041.46 ns | 94.67, 95.50 ns | **−97.7%** |

The 1k→2k front-delete candidate remains ~95 ns/delete while the old path
quadruples, confirming the mechanism rather than an adaptive-benchmark
artifact. Common `prop_read`, `prop_write`, `prop_update`, and
`array_for_in` rows stayed within ~1%. `prop_create` was +2.6% in the final
two-round confirmation but flat/reversed in the independent interleaved
prototype run, so there is no repeatable regression signal.

`tests/test_property_delete_compaction.cjs` matches Node and covers stable
value/metadata pairing, delete/re-add ordering, symbols, integer keys,
accessors, prototype fallback, warmed read/write caches, shared literal
shapes, RegExp `lastIndex`, and repeated allocation churn. Final exact-artifact
gates: spec **3712/0**, JIT **9/0**, harness **173/0**
(`test_gc_async` 395ms, `test_gc_coro` 5.934s; oha express 18,648 RPS),
devirt fuzzer all four seeds agree, and newt Main **41.746s** /
976,322,560-byte max RSS. With a temporary `ANT_GC_STRESS` hook, the focused
deletion/location/RegExp/async tests passed at stress 1/3 and the full spec
passed 3712/0 at stress 10. The hook was removed, the final binary rebuilt,
and its MD5 rechecked against the measured pin.

**`array_for` follow-up — OSR entry rejection FIXED, 2026-08-07.** The
regression was not loop codegen. Whole-function numeric feedback made the OSR
prologue guard every inferred-numeric local before selecting the requested
loop header. When the cumulative backedge threshold was reached in an early
loop of a later invocation, locals initialized only by subsequent loops were
still `undefined`. The prologue returned `SV_JIT_BAILOUT`, so
`sv_jit_try_osr` called `sv_jit_on_bailout` and discarded otherwise-valid
compiled code. `ANT_DEBUG=dump/vm:op-warn` pinned the failure at `op=entry`
before any source opcode executed.

An incompatible OSR frame now returns `SV_JIT_RETRY_INTERP`: execution
continues from the same interpreter backedge without invalidating the compiled
function. The backedge counter is reset immediately before every OSR attempt,
which delays another attempt after any retry without adding a result-specific
branch. A first prototype reset the counter in a new post-call branch; that
changed `sv_jit_try_osr`'s control-flow hash, discarded its checked-in PGO
counts, and lost both newt A/B rounds, so that form was rejected.

Pinned identities are pre-change `/tmp/ant_array_osr_base_bin`
(`ce879ea43dafeebfb695e513c0edeb5d`) and final
`/tmp/ant_array_osr_candidate2_bin`
(`e4503096a665cd0b0f4a7c55358ef73e`). Two alternating AB/BA rounds measured
`array_for` at **21.00/20.64ns** before versus **3.34/3.21ns** after:
**−84.3% / 6.36x faster**. The focused regression test fails on the base pin
with `jit: bailout 1/5 ... op=entry` and passes on the final pin. Final newt
A/B was non-directional within the host's approximately one-second noise:
candidate/base **44.703/43.374s**, then base/candidate **43.545/43.356s**;
RSS stayed below 1GB. The second-round candidate is in the existing thermal
band, and the first-round loss did not repeat.

This does not explain the separate installed-main `array_for_in` gap.
`FOR_IN` remains JIT-ineligible, and the final pin's `js_for_in_keys` machine
code is byte-identical to the base pin; do not attribute noisy 136--147ns
readings of that row to the OSR change.

Final exact-artifact gates: spec **3712/0**, JIT **9/0**, harness **174/0**
(`test_gc_async` 402ms, `test_gc_coro` 6.508s; oha express 18,205 RPS),
devirt fuzzer all four seeds agree, and newt's final candidate run was
**43.356s** / 987,365,376-byte max RSS. `maid preflight` and `git diff
--check` also pass.

**String length/build follow-up — direct length + persistent snapshots FIXED,
2026-08-07.** All three regressions entered at PR #60, but they were two
different mechanisms. `OP_GET_LENGTH` only recognized arrays, so every string
receiver called the now-general `str_utf16_len` helper four times per loop.
The JIT now reads flat ASCII byte length or an already-cached UTF-16 length
directly, with unknown metadata, ropes, builders, and other receivers retaining
the helper. Main and inline emission share one implementation. An inlined
constant-string length micro improved from **436.646/439.426ms** to
**245.189/245.203ms** over two alternating AB/BA rounds (**−44.0%**).

The builder rows were not helper overhead. Temporary whole-run counters on a
fixed 500,000-append `string_build1` run recorded **5,000 materializations, 0
cache hits, and 1,250,250,000 copied bytes**: publishing `global_res` flattened
the entire cumulative builder after every 100 appends. Builders now retain the
immutable flat/rope string sealed by the last read as a prefix; a read only
seals the newly appended chunks/tail into that prefix. Actual byte consumers
still materialize lazily, and flattening compacts the builder back to one flat
prefix. The prefix is traced by GC and participates in lazy UTF-16 length.
`test_jit_string_builder_snapshot.cjs` now keeps 5,000 historical snapshots and
checks aliases on both sides of the 4,096 rope-depth compaction boundary.

Pinned exact identities: pre-change `/tmp/ant_string_current_base_bin`
(`e4503096a665cd0b0f4a7c55358ef73e`) and final
`/tmp/ant_string_final_bin` (`a9877a6a6ce09ea053be1803d44e2672`). Two
alternating AB/BA rounds measured:

| Workload | Pre-change | Final | Delta |
| --- | ---: | ---: | ---: |
| `string_length` | 2.87, 2.89ns | 1.00, 0.95ns | **−66.1% / 2.95x** |
| `string_build1` | 127.06, 118.73ns | 9.33, 9.35ns | **−92.4% / 13.16x** |
| `string_build2` | 126.35, 127.17ns | 8.91, 9.33ns | **−92.8% / 13.90x** |

The old PR #50 builder score (about 7.4--7.8ns) remains semantically invalid;
the correct persistent-snapshot path is now within roughly 2ns/append of it
instead of 15x slower. Final exact-artifact gates: spec **3712/0**, JIT
**9/0**, harness **174/0** (`test_gc_async` 423ms, `test_gc_coro` 6.532s;
oha hono 31,927, express 18,195, h3 20,743, elysia 62,361 RPS), devirt fuzzer
all four seeds agree, and newt Main **40.812s** / **41.20s wall** /
899,432,448-byte max RSS. With the temporary `ANT_GC_STRESS` hook, focused
snapshot/length tests passed at stress 1/3 and the full spec passed 3712/0 at
stress 10. The hook was removed before the final rebuild and measurement.

**String builder V8 follow-up — interpreter parity, inline append, canonical
ropes, and direct builder length FIXED, 2026-08-07.** Temporary whole-run
`ANT_STRING_STATS` counters separated four mechanisms before code changed:
the cold/interpreter publication probe performed **200 builder reads and 200
full materializations**, while the equivalent JIT read already made persistent
snapshots; the append probe made **511,008 JIT helper calls**, with every hot
append using an existing builder, flat ASCII RHS, and available tail capacity;
the length loop performed **100,200 builder length helper calls with zero
snapshots**; and final materialization walked **3,007 rope nodes**. The counters
and their atexit hook were diagnostic only and are not retained.

Interpreter local/argument reads now use the same persistent snapshot operation
as JIT reads. The JIT inlines the allocation-free one-byte ASCII append case:
it guards the builder and flat-string tags, byte length, ASCII byte, and tail
capacity, then updates the tail, byte length, and optional cached UTF-16 length
in place. All coercion, Unicode, spill, and non-builder cases retain the helper.
The length emitter reads `builder->len` directly only for ASCII builders at
sites the existing bytecode prescan proves can target a builder. An initial
generic layout put the extra builder discrimination at every string-length
site and regressed the flat `string_length` row from 0.99ns to 1.03--1.08ns;
that form was rejected. Builder-target gating restored flat length to
0.95ns while preserving the builder win.

After a rope is flattened, its cached flat string is now its canonical
representation: obsolete `left`/`right` edges are cleared and depth becomes
zero. Every semantic traversal already checks `cached` before the children,
and GC marks the cached flat string. A retained-root/major-GC probe with 64
live 640KB roots fell from **10,462,480 bytes / 160 rope blocks** to
**5,876,080 bytes / 90 blocks**. Two alternating AB/BA rounds improved that
fixed workload from **242.483/239.330/239.724/243.616ms** to
**176.654/174.580/169.516/173.879ms**. A separate leaf cursor was not justified:
the counter showed one required tree walk, after which canonicalization makes
the flat representation authoritative.

The fixed-work mechanism experiments used the same pinned pre-follow-up
`/tmp/ant_string_v8follow_base_bin`
(`a9877a6a6ce09ea053be1803d44e2672`) and a separately pinned candidate after
each named change. Their interleaved results were:

| Workload | Pre-follow-up | Final | Delta |
| --- | ---: | ---: | ---: |
| cold interpreter publication | 1.522, 1.466, 1.442, 1.600ms | 0.019, 0.014, 0.016, 0.014ms | **−99.0%** |
| JIT published append | 4.322, 4.110, 4.696, 4.164ms | 1.799, 1.738, 1.666, 1.763ms | **−59.0%** |
| append + length each iteration | 1.294, 1.383, 1.397, 1.355ms | 0.443, 0.447, 0.407, 0.419ms | **−69.2%** |

The final exact artifact is `/tmp/ant_string_v8follow_final_bin`
(`b9e99b4851622afc9b51f644cdcb6180`). The real filtered `tests/bench.js`
rows, also AB/BA against the base pin, measured `string_build1` at
**9.28/9.30ns → 7.00/7.00ns** (**−24.6%**) and `string_build2` at
**9.30/9.03ns → 7.19/7.21ns** (**−21.4%**). `string_length` held
**1.00/0.96ns → 1.03/0.95ns** (non-directional). Regression coverage now
includes the one-byte tail boundary, a cached length after every append, and
continued use of a canonicalized rope across later concatenation and major-GC
pressure. It also warms a builder-parameter append before calling it with the
parameter absent, covering bailout stack reconstruction at the fast-path
entry guard.

Final exact-artifact gates: spec **3712/0**, JIT **9/0**, harness **174/0**
(`test_gc_async` 385ms, `test_gc_coro` 5.898s; oha hono 34,619, express
19,076, h3 22,603, elysia 65,016 RPS), and devirt fuzz all four seeds agree.
Newt Main completed in **41.63s wall** with **704,167,936-byte max RSS**
(901,399,728-byte peak footprint). A temporary `ANT_GC_STRESS` hook passed the
focused mixed rope/builder probe at stress 1/3 and the full spec 3712/0 at
stress 10. Both the stress hook and string counters were removed before the
final rebuild and MD5 pin.

**Large-AST rope/JIT follow-up — iterative ropes, young allocation, direct
string ADD, and guarded PUT_FIELD FIXED, 2026-08-07.** The remaining
`todo/tests/ytdlp.js` generation cost was a retained output rope built by about
1.4 million small property appends. The old defensive depth limit repeatedly
flattened deep ropes. Removing that limit and writing a reverse iterative
flatten reduced the pinned full workload from **5.10/5.12s** to
**3.32/3.33s**. GC rope traversal is now iterative as well, so deep append
trees no longer consume the C stack.

Rope nodes now allocate in a per-isolate 8 MiB young pool. Minor collections
trace young ropes from roots, remembered old objects, and mutable builders;
live blocks promote and dead blocks recycle. The nursery improved the same
pinned workload from **3.34/3.23s** to **3.04/3.03s**. Temporary whole-run
logging showed six rope-pressure minors at roughly 8 MiB each and only two
object-live majors on the full workload. Those diagnostic counters and their
extra `ANT_GC_LOG` output were removed after validation.

All rope mark metadata is owned by the isolate. An initial OOM fallback used a
process-global isolate pointer and singleton fallback record; it was rejected.
It also failed when the first mark-table allocation returned null because an
early zero-count return bypassed linear lookup. The final design passes `js`
explicitly into rope lookup/mark operations, counts the isolate's rope blocks,
and reserves the complete table before changing any mark-cycle state. If that
single reservation fails, collection returns before marking or sweeping and
therefore conservatively retains every rope block. `rope_mark_find` remains a
pure binary search with no pool-walking fallback. Four worker isolates
concurrently build past the nursery threshold, collect, and materialize ropes
in the worker-thread regression.

At typed string-only `ADD` sites, generated code now loads flat/rope lengths,
applies the 13-byte short-string policy, and bump-allocates a young rope from an
existing pool block. Block allocation and collection remain in the C fallback.
A monomorphic guarded `PUT_FIELD` emitter validates epoch, receiver shape,
own-holder identity, slot bounds, exotic state, and the young-rope write
barrier before writing the cached in-object or overflow slot. A 10-million
property-append micro improved from **0.76/0.74s** to **0.68/0.67s**. The same
primitives are available to inline emission; a separate fused
`GET_FIELD + ADD + PUT_FIELD` path was not added because the inline-only
differential was flat.

Final clean pinned identities are pre-follow-up `/tmp/ant_rope_base_bin`
(`b9e99b4851622afc9b51f644cdcb6180`) and per-isolate final
`/tmp/ant_rope_final_bin` (`ec8ecbe1ce2f85ec50b91fb7304d4ffb`). Two clean
AB/BA rounds measured **5.04/5.04s** before versus **3.14/3.13s** after
(**−37.8%**) with checksum
`3a8b0cfae9bd6f3499333e6da5aed0ac8c5f68ac` in every run. The final
per-isolate ownership change was itself flat against the rejected global-state
pin: **3.04/3.07s** versus **3.00/3.04s**.

Filtered `tests/bench.js` AB/BA rows confirm the intended composition:
`prop_write` **3.51/3.54ns → 1.58/1.58ns**, `prop_update`
**4.92/4.85ns → 2.94/2.96ns**, `string_build1`
**6.83/6.74ns → 2.70/2.69ns**, and `string_build2`
**6.92/6.84ns → 2.76/2.83ns**. `array_push` and `array_for_of` appear about
10--14% lower only in stale-profile local builds: no-rope and no-minor-tracing
differential binaries reproduce the same result, while the compiler reports
the untouched functions' checked-in PGO counts discarded after translation
unit changes. Regenerate PGO at release time; do not special-case array code.

Final gates after the per-isolate refactor: spec **3713/0** (one new concurrent
worker assertion), JIT **9/0**, harness **175/0** (hono 34,552, express 18,988,
h3 22,201, elysia 63,709 RPS), devirt fuzz all four seeds agree, and newt Main
**41.516s** / **41.87s wall** / **779,829,248-byte max RSS**. The earlier
temporary GC-stress pass covered focused rope/builder tests at stress 1/3 and
the full spec at stress 10; the hook is absent from the final tree.

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

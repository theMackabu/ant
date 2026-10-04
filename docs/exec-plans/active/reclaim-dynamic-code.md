# Reclaim Dynamic Code

Status: active
Last reviewed: 2026-09-23
Owner: theMackabu

## Goal

Code compiled at run time by `eval` and the `Function` constructors is freed
once nothing can run it again. A loop that compiles, runs and drops unique
source reaches steady-state RSS. This is stage two of the compiled-code
lifetime entry in [tech-debt.md](../tech-debt.md): the code unit owns its
functions, literals and cached templates, and they die together.

## Scope

- Reclaimable units: direct and indirect `eval`, `Function`,
  `AsyncFunction`, `GeneratorFunction` and `AsyncGeneratorFunction`.
- Unchanged, still immortal: scripts, ES modules, CommonJS modules, the REPL,
  snapshots and N-API scripts. They keep the global code arena and permanent
  literals.
- Out of scope: the global atom intern table (property names and identifiers
  in dynamic code are still interned forever), and MIR machine code.

## Design

- **Code unit.** `sv_code_unit_t` owns every `sv_func_t` compiled for one
  dynamic source, listed in `unit->funcs`. `func->unit` is NULL for immortal
  code. While a unit compiles, `code_arena_bump` and `code_arena_alloc` route
  to the unit, so every compiler allocation (function structs, bytecode,
  constants, ICs, metadata, source text) lands in unit memory without touching
  the call sites. Source text in a unit is not interned.
- **Unit memory.** Units bump-allocate from a per-isolate dynamic-code arena
  of 16 KiB cage blocks (one page on arm64 macOS, four on 4 KiB-page
  systems), with the block header inside. Each block counts the units that
  allocated from it and returns to a free-block cache when that count reaches
  zero, so compiling and freeing a unit costs no system call. Recycled blocks
  are zeroed, because the compiler relies on zeroed arena memory. Units cannot
  move, so a surviving unit pins its page: 2,000 small survivors interleaved
  with dead code hold about 30 MiB more than when compiled together on macOS
  (master holds all of it: 128 MiB). Reusing holes inside blocks would not
  lower that, since the survivors still sit on separate pages.
- **Literals.** A unit's string and BigInt literals are ordinary collected
  values instead of permanent ones, because a literal can escape into any heap
  value. The unit's constant tables list them in `gc_const_slots`, so they are
  traced whenever the unit is. Flat strings and BigInts are swept only by
  majors, so no minor-collection handling is needed for them. While a unit is
  compiling, its literals are rooted through the unit's compile-root vector.
- **Liveness.** `gc_mark_func` marks the function's unit, and marking a unit
  marks every function in it, so a unit is live or dead as a whole. After a
  major, a unit is freed when it was not marked, has no pins and has not been
  touched by the JIT.
- **Pins.** A unit is created pinned. The compiling caller unpins it once the
  code is reachable from a frame or a closure: after eval returns, or after
  the `Function` constructor has created its closure. The error site pins the
  unit whose source it points at.
- **Templates.** `OP_PUT_CONST` and literal templates on unit functions stop
  calling `gc_pin_permanent`. They are traced through the unit on majors, and
  through a young-template vector on the next collection after they are
  stored, so a minor cannot free a template held only by unit memory.
- **JIT.** MIR cannot free one function's machine code, and generated code
  embeds raw pointers to its function, to inlined callees and to their
  constants, ICs, atoms and sites. Any unit the JIT compiles or embeds becomes
  immortal and is marked as a root on every major.
- **Other holders.**
  - `call_target_fb` targets are weak. A major records every traced function
    whose feedback points into a unit and, after marking, clears targets in
    units the sweep will free, so feedback never points at freed memory and a
    chain of functions that called each other does not keep itself alive.
    Units the JIT embeds are immortal, so their targets are never cleared.
  - Eval environments trace the function whose metadata they point into.
  - Freed closures get `func = NULL`, so the conservative scan cannot follow a
    stale function pointer from the closure free list.
  - Freeing a unit releases shape references held by its object sites and IC
    slots and frees the per-function feedback allocations. Each released IC
    slot gets `SV_IC_SHAPE_SLOT_DEAD`, and one pass over
    `js->ic_shape_ref_slots` drops the marked entries while unit memory is
    still readable. Nothing in the sweep allocates, so it cannot fail halfway.
  - `SLOT_CODE` holds a raw `kTypeSourceCode` pointer only into memory that is
    never freed. Text from a unit (class source, `Function` display text) is
    stored as a collected string instead, so it stays valid wherever the slot
    is copied, including bound functions and implicit class constructors,
    which have no function of their own to keep the unit alive.
- **No silent failures.** A unit lists its functions through
  `sv_func_t.unit_next`, so registering a function cannot fail. Building a
  function's constant tables raises an out-of-memory error that fails the
  compile, since a unit function's literals are traced only through them.
- **Pacing.** Allocation into units counts toward the pool allocation bytes,
  and the pool's live bytes count the capacity of blocks units hold (not the
  bytes they use) plus the malloc'd unit struct and block list, so memory
  pinned by partly-dead blocks still drives collection. When a unit finishes compiling (complete and pinned,
  so a collection is safe), a major runs if the pool threshold is reached,
  as `js_type_alloc` does for pool allocations.
- **Collector roots.** Pinned units (compiling units are always pinned) sit on
  their own list, which is all a minor visits. Majors walk every unit.

## Validation

Target:

- A loop compiling unique `new Function` and `eval` sources with object
  literals, tagged templates, BigInt literals and inner closures reaches
  steady-state RSS.
- Template identity per call site holds while the code is alive.
- Values escaping dead dynamic code (literals, closures, generators, classes,
  errors, direct-eval environments) stay valid across many majors.
- `ANT_GC_STRESS` sweeps pass on the verify build.
- bench-v8 and eval-heavy microbenchmarks show no slowdown.

Status (2026-09-23):

- 200k `new Function` compiles: peak RSS 940 MiB before, 26 MiB after, and
  the loop runs faster (2.5 s to 2.0 s). 1M `eval` compiles saw-tooth between
  20 and 26 MiB.
- `tests/test_dynamic_code_reclaim.cjs` covers escaping values, template
  identity, JIT-compiled and inlined dynamic functions, and runs under a
  64 MiB RSS cap in the harness (the pre-change build needs about 407 MiB).
- Harness passes except the known Hono adapter failure; GC and JIT C tests
  pass; all 102 spec files pass; `ANT_GC_STRESS=97` and `211` sweeps show
  only the known failures. Run the harness without `FORCE_COLOR`: colored
  child output breaks the jit_suite, tty and stdin range checks on every
  build, master included.
- Timing against master 001640e7, both built without PGO, 8 alternating
  rounds on a quiet machine: bench-v8 geomean -0.3% (RayTrace +1.3%,
  DeltaBlue, EarleyBoyer and RegExp about -1%, the rest level). The VM
  boundary microbench is level on every crossing. Against a PGO build of
  master the gap was -0.7%, from functions whose profile no longer matches;
  it should close once the profile is regenerated. Unique eval and
  `new Function` compile loops run 17% faster, since the code arena no longer
  grows.

## Decision Log

- 2026-09-23: Units cover only dynamic code. Scripts and modules are
  effectively immortal, and keeping them on the global arena avoids any cost
  for them.
- 2026-09-23: JIT-touched units stay immortal instead of freeing MIR code.
  Dead dynamic code is rarely hot enough to compile.
- 2026-09-23: Function marking moved to an explicit worklist. Unit membership
  and call-target feedback link functions across units, so the old recursion
  could exceed the C stack on long chains of eval code.
- 2026-09-23: Unit memory counts toward the pool bytes that pace majors.
  Without it, an eval loop that allocates few objects never runs a major and
  never frees a unit.
- 2026-09-23 (review): implicit class constructors pointed into unit memory
  with nothing keeping the unit alive, so `toString` after churn read freed
  memory. An owner slot would have worked but every copier of `SLOT_CODE`
  would have to carry it; unit text is a collected string instead. The
  allocating dead-slot list, whose failure path kept half-freed units, was
  replaced by the sentinel pass, minors stopped walking every unit, and
  `code_unit.c` was added to the WebAssembly source list.
- 2026-09-23 (second review): a failed function registration left
  `func->unit` NULL, so the function's collected literals were never traced;
  registration is now allocation-free. Constant-table failures fail the
  compile. Unit allocation now applies pool pressure; a loop of 300k
  `eval(s)` peaks at 20 MiB (master: 107 MiB).
- 2026-09-23 (third review): strong call-target feedback kept every
  predecessor in a chain of dynamic functions alive (60k functions: 186 MiB);
  feedback is now weak and the chain stays at 19 MiB. The function worklist
  and the feedback list are freed after a major once they grow past 1024
  entries.
- 2026-09-23 (fourth review): blocks went from 64 KiB, which the header
  pushed to 80 KiB, to 16 KiB with the header inside; pacing counts held
  block capacity. Page-sized blocks (4 KiB on Linux) were rejected as
  unmeasured churn on the cage's first-fit allocator. A failed
  `sv_code_unit_begin` keeps falling back to permanent storage: that is the
  pre-unit behavior, safe, and only reachable when a ~100-byte malloc fails.
- 2026-09-23: Eval binding names are copied into the code arena when
  metadata is finalized. They pointed into the parse buffer: the
  eval source string, or the Function constructor's buffer that is freed right
  after compiling, so direct eval inside such code read freed memory. This
  predates units.
- 2026-10-02: The unit being compiled lives on the isolate
  (`js->code_units.active`) instead of a thread-local, so isolates on one
  thread cannot see each other's unit. The global allocators
  (`code_arena_alloc`, `code_arena_bump`) no longer look at it; the compiler
  allocates through `sv_code_bump(js, ...)` and `sv_code_text(js, ...)`, and
  runtime allocations (IC entries, poly blocks) always use the global arena.
  When a unit is freed, a function's constructor `prototype` cache
  registration is dropped with its IC slots before the sidecar is freed
  (`tests/test_code_unit_ctor_proto.c`).
- 2026-10-03 (review): polymorphic read and store case blocks
  (`sv_gf_poly_t`, `sv_pf_poly_t`) lived in the global code arena with their
  shapes registered there, so a dead unit leaked its blocks and their shapes:
  200k `new Function` calls over 4 receiver shapes peaked at 117 MiB (reads)
  and 325 MiB (stores). Blocks are now malloc'd, own their shapes, sit on
  per-isolate lists for teardown, and are freed with their IC
  (`SV_IC_HAS_GET_POLY`/`SV_IC_HAS_PUT_POLY` say which union member is live)
  when it goes megamorphic or its unit dies: 28 and 32 MiB.
- 2026-10-03 (review): a minor that reached one function of a unit queued
  every function in the unit, and walked every pinned unit: one hot function
  out of 5,000 in a `new Function` body took 1.33M function visits against
  5.5k on master. Minors now trace only the function they reach and skip
  finished pinned units (their compile roots are still marked while
  compiling); unit liveness is decided by majors, and young literal templates
  are roots of their own. `tests/test_code_unit_minor.cjs`; verify build
  under `ANT_GC_STRESS` 97 and 211.
- 2026-10-03 (review): weak call-target feedback was cleared only in
  functions the major traced. A script or module function lives forever but
  need not be traced (nothing reaches it any more), so a target it held in a
  dead unit pointed into freed memory, for the JIT to read if the function
  ran again. A function outside units now goes on `code_units.fb_watch` the
  first time it records a unit target (`fb_unit_watched`), and every major
  clears that list's dead targets; if the list cannot grow, the target is not
  recorded. The list is bounded, since such functions are never freed.
  `tests/test_code_unit_feedback.c` fails without the clearing.
- 2026-10-03 (review, performance):
  - Majors scanned up to 32 feedback entries of every traced function for
    unit targets. A function now carries `fb_unit_target`, set when it records
    a unit callee and cleared when none is left, and only those are listed.
    Function-mark time per major dropped 13-70% depending on the workload;
    the profiler's per-major figure varies several-fold with what the rest of
    the program does (it includes minors), so whole-program instructions are
    the steadier comparison.
  - Unit memory counted toward the pool bytes, so minors started majors for
    it at the pool threshold however large the heap: 50k `new Function` with
    4M live objects took 421 ms and 4 majors. It now has its own counter
    (`js->gc.code_alloc`) against the larger of the pool threshold and an
    eighth of the live heap (`gc_code_major_threshold`), checked after
    minors, in idle collection and when a unit finishes: 137 ms, no major
    (master 156 ms). 300k `eval` still peaks at 20 MiB.
  - The free-block cache holds 64 blocks (1 MiB) instead of 16; no change in
    the eval loop's system time was measurable.

## Follow-ups

- Prune the atom intern table for dead units.
- Free MIR modules and code per function, which would let JIT-touched units
  die too.

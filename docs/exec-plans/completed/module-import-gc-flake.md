# Module Import GC Flake (TypeError "invalid object")

Status: completed
Last reviewed: 2026-08-01
Owner: theMackabu

## Problem

`examples/spec/run.js --all` intermittently (~13–20% of runs) fails at
`await import(fileUrl)` for `modules.js` with `TypeError: invalid object`
(0ms elapsed — the import fails immediately, no test runs). The error comes
from `mkprop` / `mkprop_interned_impl` in `src/ant.c`:

```c
if (!ptr || !ptr->shape) return js_mkerr(js, "invalid object");
```

i.e. property definition during module namespace construction receives an
object whose shape is gone. The flake only appears after ~50 prior in-process
imports and is GC-timing dependent; `modules.js` passes standalone every time.

## Evidence it is pre-existing

Measured 2026-08-01, both binaries on the same machine, same suite:

- clean build of committed HEAD (`e00454fb`, no uncommitted changes): 4/30 runs flake
- working tree with all pending fixes: 4/18 runs flake

Same rate — NOT introduced by the events/stream/url/define work on
`feat/unify-event-internals`. This is a latent GC bug in the module import
path, most likely an unrooted allocation window during namespace construction
(namespace or module object allocated, GC runs inside a subsequent allocation
or evaluation step, object is swept or its shape freed, then `mkprop` runs).

## Constraints

- No GC-stress hook exists today (`grep -rn "getenv\|GC_STRESS" src/gc/ src/gc.c`
  finds nothing), so the repro is probabilistic. Building a deterministic repro
  is step one.

## Task list

1. Add a GC-stress mode (env-gated, e.g. `ANT_GC_STRESS=1` forcing a
   collection on every allocation or every Nth) — this should make the flake
   deterministic and is independently useful.
2. Under stress, capture which object `mkprop` receives (namespace object,
   module record object, or an export value holder) and where it was
   allocated.
3. Audit the import path (`src/esm/loader.c`, namespace construction) for
   values held only in C locals across allocating calls; root them.
4. Re-run `examples/spec/run.js --all` 30+ times to confirm the flake is gone.

## Validation status

- Flake reproduced and rate-measured on clean HEAD and working tree (see
  above). Root cause not yet isolated.
- 2026-08-02: reproduced again via the full suite (hit on run 3 of 25). A
  minimal repro — plain sequential `await import()` of the same spec files in
  one process, 15 attempts — does NOT reproduce. The trigger needs the
  runner's surrounding state (per-file test execution between imports and the
  hook/summary promise churn), i.e. allocation pressure interleaved with the
  imports, not the imports alone. Don't retry the bare-import loop; go
  straight to the GC-stress hook.

## Resolution (2026-08-02)

Root cause: NOT the module system. `ffi_init_prototypes` (src/modules/ffi.c)
created four prototype objects into file-scope `static ant_value_t` globals that
were never GC-registered. Each subsequent allocation in that function could
trigger a collection; the just-created protos were unreachable from any root,
got swept, and the following `js_set` hit `mkprop` on a shape-freed object →
"invalid object". modules.js is the only spec file importing `ant:ffi`, and
~50 files of prior heap growth put a GC due exactly in that window — hence the
flake's shape. Caught via a temporary `ANT_GC_STRESS=<n>` hook in gc_maybe
(forced a full GC every n allocation checkpoints; removed after the fix landed)
plus an env-gated abort in mkprop and the macOS crash report stack.

An audit found the same disease across 30+ files: 129 module-level
`ant_value_t` globals — `static` caches plus extern-linked stream/fetch protos
kept alive by hand-written `gc_mark_*` hooks or (accidentally) by boot-pin
timing, with ~70 rooted by nothing at all (including the lazily-initialized
`node:stream` protos). Fix: all 129 migrated to per-isolate structs — `js->builtins` (immortal singletons) and
`js->mutable_roots` (reassignable slots: RegExp $1..$9 match strings, pending
wasm import throw) — with the field lists in `include/isolate_values.h` as an
opcode.h-style x-macro table. `gc_visit_roots` expands the same table to mark
every field, so a value stored in either struct is rooted by construction: no
per-field registration to forget, and no `GC_MAX_STATIC_ROOTS` capacity
concerns. This also resolves the multi-isolate half of the old
`g_eventemitter_proto` tech-debt item for these caches.

Verified: modules.js and the full suite pass under `ANT_GC_STRESS=1/3/10/25`;
30 consecutive un-stressed full-suite runs with the abort tripwire armed: 0
hits (pre-fix rate was ~13-20% per run, p < 0.002).

Open follow-up discovered while stressing: `ANT_GC_STRESS=3` can still segfault
near suite end with PC=0 under `uv__stream_io` (dead uv callback during the
module-eval event loop) — consistent with the worker_threads teardown UAF
already tracked in tech-debt; now known to be reachable under stress.

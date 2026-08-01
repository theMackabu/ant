# Module Import GC Flake (TypeError "invalid object")

Status: active
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

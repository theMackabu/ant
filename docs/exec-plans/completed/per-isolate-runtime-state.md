# Per-Isolate Code Arenas, Mark Tables and IC Epochs

Status: completed
Last reviewed: 2026-10-04
Owner: theMackabu

## Problem

Destroying one of two isolates freed the other's bytecode: `js_destroy` reset
the process-wide code arena that holds every script's and module's bytecode,
IC slots and source text. The string/bigint mark tables and the IC epoch
counters were also process globals.

## Decisions

- Code arena, parse arena and code-text intern table live on `ant_t`
  (`js->arenas`). Arena functions take the isolate; the parser reaches it
  through `mk`/`mk_plain`. The unused code-arena mark/rewind was dropped.
- String and bigint mark tables move to `js->gc`. They are rebuilt every
  collection, so there was no epoch collision, only shared scratch. Growth
  goes through `vec_grow`; a block that cannot be added is kept for a cycle.
- The string sweep epoch is thread-local, not per isolate: the caches it
  keys (`utf8.c`, `regex.c`) are thread-local and remember a string by
  address, so any isolate's sweep on that thread must invalidate them.
- IC epochs are per isolate. That is safe because the shape tree, though
  shared, is only edited in place for shapes with unshared descriptors, which
  belong to one object; objects hold shape references, so pruned shapes stay
  alive. The JIT bakes `&js->ic.epoch` of the compiling isolate.
- `set_slot` asserts it is never asked to write a prototype: that needs the
  write barrier and the isolate's epoch, which only `set_slot_wb` has.

## Validation

- `isolate-code-arenas` fails with SIGBUS on the previous global arena.
- `isolate-ic-epochs` checks cross-isolate epoch independence, own-isolate
  invalidation with compiled code present, and strings/bigints kept in one
  isolate across the other's collections. No scenario was found where only the
  compiled epoch guard decides correctness (shape and attribute guards also
  catch the edits tried), so the JIT address change rests on review.
- Harness, spec and native suites as on master.

## Remaining

The shape tree (`shapes.c`) is still process-wide; see the GC entry in
[tech-debt.md](../tech-debt.md).

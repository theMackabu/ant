# Symbol and Iterator Ownership

Status: completed
Last reviewed: 2026-09-20
Owner: theMackabu

## Outcome

Iterator result construction, array/string iterator objects, and the C
open/next/close protocol now belong to `src/modules/iterator.c` and its header.
The Symbol initializer owns Symbol itself. `init_intrinsic_symbols` wires core
symbol properties before Iterator/Generator initialization and snapshot capture.
Generator tags and Ant's tag live with their respective initializers.

Well-known symbols use direct isolate fields generated from the repeatedly
included `ANT_SYMBOL` list in `include/symbol_list.h`. The same list initializes,
publishes, and marks the identities. New helpers use the surrounding snake_case
style, as explicitly requested by the user.

Native iterator registrations are dynamically sized and isolate-owned. They
retain and trace both prototype and original next method, require both identities
for direct dispatch, and are freed at teardown. Closing an optimized iterator
still observes return and preserves an existing abrupt completion. Invalid next
methods/results are rejected, and native function return results remain valid.

The six redundant process-global root registrations were removed. Prototype
roots are traced explicitly from the collecting isolate. Symbol.for hash nodes
are released at destruction. Other modules' global roots and the process-wide
GC epoch remain outside this change; this is not a whole-runtime isolation claim.

Symbol descriptors and its prototype constructor link now match their contracts.
Array unscopables covers all 16 supported names; Array.values is captured at
definition time. Iterator tags use accessors so subclasses can define own tags.

## Integration

- Native bootstrap, the embedding example, and native tests follow the explicit
  Symbol -> intrinsic symbols -> Iterator -> Generator dependency order.
- Async iterator helper installation is wholly owned by Iterator initialization.
- Wasm now compiles the Iterator module and follows the same order. Its minimal
  Ant object retains its tag without importing the CLI builtin module.
- Wasm validation exposed stale pre-existing compatibility code: the removed
  coroutine reaper call and missing error-output adapters. The stubs now match
  current runtime entrypoints and provide plain diagnostics.
- Current ownership and startup rules are recorded in `ARCHITECTURE.md`.

## Validation

- Native Meson reconfiguration/build and codesign passed.
- New descriptor/protocol regression passed in Node, native Ant, and Wasm.
- 13 focused JavaScript regression files passed, covering iteration, symbol
  access, primordials, and generator GC.
- Native symbol-iterator-isolates, arguments-storage, typedarray-from-cleanup,
  and error-handoffs tests passed. The new lifecycle test covers two live
  isolates, alternating collections, both destruction orders, repeated runtime
  creation, and registry growth beyond eight entries with forced collection.
- Full spec suite: 4,240 tests across 102 files, zero failures.
- Wasm package: 15 tests passed; `npm pack --dry-run` passed (22 package files).
- `maid preflight`, knowledge/structure checks, and `git diff --check` passed.

Review caught and corrected native-function IteratorClose results, subclass tag
assignment, a remaining embedding bootstrap caller, and an outdated native test
that initialized generators before symbols. No performance claim is made.

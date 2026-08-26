# Promise Resolution and Await Semantics

Status: completed
Last reviewed: 2026-08-25
Owner: theMackabu

## Problem

Ant treated every Promise-branded value as an exact intrinsic Promise. `await`,
`Promise.resolve`, and Promise resolving functions therefore adopted internal
state directly and skipped observable `constructor` and `then` behavior. This
made Promise subclasses such as OpenAI SDK `APIPromise` resolve to their
superclass placeholder value instead of the parsed public result.

Repeated top-level awaits of the same fulfilled Promise also exposed a handler
queue bug: a handler appended while the current batch resumed JavaScript was
cleared with the processed batch, leaving the module suspended with no pending
work.

## Decisions

- Cache the intrinsic Promise constructor, prototype, and `then` function as
  isolate symbols.
- Reuse an exact intrinsic Promise only while a one-way constructor protector
  proves that its observable `constructor` is still intrinsic. Use the normal
  proxy-aware property lookup after invalidation or for a non-canonical
  prototype.
- Keep a separate one-way `then` protector for resolving functions. This
  preserves the required distinction between `await nativePromise`, which may
  reuse the exact Promise, and `resolve(nativePromise)`, which must observe an
  overridden `then`.
- Invalidate the protectors from descriptor operations, interpreter stores,
  deletes, and JIT stores. The JIT keeps direct property stores and emits the
  guarded byte write only for literal `constructor` and `then` keys.
- Share first-call state between each resolving-function pair without a
  separate record allocation.
- Preserve handlers appended during Promise callback or coroutine reentrancy;
  keep the common single-handler representation inline.
- Make Promise combinators run PromiseResolve and invoke observable `then` so
  subclass and exact-Promise overrides remain visible. Reuse the protected
  intrinsic `then` fast path for canonical Promises.

## Performance Boundary

The constructor and `then` protectors remove observable property lookup from
the canonical native-Promise paths. Ordinary JIT property stores emit no new
runtime instructions. Correct primitive `await` suspends and resumes through a
dedicated coroutine-resume microtask without allocating an intermediate
intrinsic wrapper Promise. Extending that direct path beyond primitives remains
a separate optimization.

The Promise resolution benchmarks accept an optional case selector so
candidate and baseline runs can measure one path per fresh process. A second
entrypoint avoids repeated same-Promise top-level awaits while retaining the
original benchmark as a regression workload.

## Validation

- Focused Promise subclass, thenable, callable-Proxy, top-level-await, and hot
  protector-store tests pass under Ant and Node.
- The async harness passes both Promise regressions within the 48 MB RSS limit
  and passes the repeated top-level-await regression.
- `examples/spec/run.js --all`: 3969 passed, 0 failed across 101 files after
  the final review fixes.
- The real Yet provider bundled with `openai` 7.5.0 consumes a mocked streamed
  Responses API result under Ant without changes to Yet.
- `maid preflight` and `maid knowledge` pass.

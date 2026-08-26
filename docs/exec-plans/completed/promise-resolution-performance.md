# Promise Resolution Performance

Status: completed
Last reviewed: 2026-08-25
Owner: theMackabu

## Goal

Reduce the overhead of Ant's corrected Promise resolution and `await` paths
without weakening the Node-compatible observable semantics established by the
Promise resolution fix.

## Decisions

- Connect canonical intrinsic Promises directly during the queued thenable job
  while the Promise species lookup chain is protected. Observable `then` and
  species behavior remains on the generic path after relevant mutation.
- Resume primitive `await` expressions from a dedicated microtask without an
  intermediate wrapper Promise. The fast path retains the mandatory async
  suspension and coroutine rooting.
- Specialize syntactic one-argument `Promise.resolve(value)` calls through a
  reusable stable-builtin opcode. Runtime identity, receiver, and arity guards
  fall back to ordinary method-call dispatch when JavaScript has replaced or
  shadowed anything.
- Keep ordinary JIT property stores on their direct path. Literal
  `constructor` and `then` stores additionally invalidate the corresponding
  one-way protector inline.
- Do not generate PGO data for this change. Treat timings as diagnostic because
  the existing backend profile does not match the changed source.

## Performance

Fresh-process, no-top-level-await medians used a pinned corrected-semantics
baseline and the final candidate. Each cell is the range from two seven-round
runs.

| Case                                            |       Baseline |      Candidate |           Result |
| ----------------------------------------------- | -------------: | -------------: | ---------------: |
| `await native Promise` (25,000)                 |        1.50 ms |   1.53-1.62 ms | effectively flat |
| `Promise.resolve(native Promise)` (2,000,000)   | 19.01-19.24 ms | 16.86-16.88 ms |    11-13% faster |
| ordinary JIT property store (2,000,000)         |   4.02-4.13 ms |   4.06-4.09 ms |             flat |
| `await primitive` (25,000)                      |   2.49-2.50 ms |   1.27-1.28 ms | about 49% faster |
| resolving function with native Promise (25,000) | 12.98-14.16 ms |   8.00-8.13 ms |    38-43% faster |
| `Promise.resolve(primitive)` (2,000,000)        | 89.82-92.09 ms | 89.23-89.68 ms |             flat |

The stable-builtin opcode closes a real Promise-specific dispatch gap. The
remaining difference from Node is mostly Ant's broader execution-throughput
gap and is not evidence for an unguarded Promise-only shortcut.

## Review Findings

- Direct JIT `constructor` stores now invalidate both the constructor and
  species protectors.
- Nested stable-builtin calls remain inlineable and use the same runtime guards
  as top-level calls.
- Canonical adoption does not create a `trigger_parent` edge from the target to
  the source; that edge formed a cycle when two pending Promises adopted each
  other.
- A proposed early exit from handled-parent traversal was rejected because
  existing callers may mark a child handled before its ancestors are visited.

## Validation

- `maid build` completed without generating PGO data.
- The async harness passed 18 files with zero failures, including Promise
  subclass, protector, fast-path, repeated-TLA, and coroutine-GC coverage.
- The JIT harness passed all 125 tests across 9 files.
- `examples/spec/run.js --all`: 3,969 passed and 0 failed across 101 files.
- Both top-level-await and no-top-level-await benchmark entrypoints execute and
  print all cases under Ant.
- The real Yet provider bundled with `openai` 7.5.0 consumed a mocked streamed
  Responses API result under Ant without changes to Yet.

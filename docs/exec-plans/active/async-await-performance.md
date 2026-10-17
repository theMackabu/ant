# Async Await Performance and Settle Order

Status: active
Last reviewed: 2026-10-17
Owner: theMackabu

## Problem

`compile_tail_return_expr` compiles `return await x` as `return x` in async
functions outside `try`. That changes when the returned promise settles, which
programs can observe:

```
node: t0 t1 withAwait inTry plainThenable awaitPrim t2 plain awaitThenable t3 ...
ant:  t0 awaitPrim t1 inTry awaitThenable plainThenable t2 withAwait plain t3 ...
```

`return await p` settles one tick late; `return await 3` and
`return await thenable` settle one tick early. The rewrite exists because a
suspend and resume is expensive, so removing it alone makes those forms
19-69% slower.

Sequential awaits (`for (...) await fn()`, 1M calls, best of 5):

| case | Ant | Node |
|---|---|---|
| `await i` in a loop | 35 ns | 34 ns |
| `await` an async fn with no await | 87 ns | 36 ns |
| `const v = await 3; return v` in the callee | 142 ns | 65 ns |
| `const v = await thenable; return v` in the callee | 293 ns | 77 ns |

A plain `await` in a running coroutine is already at parity. The gap is the
cost of calling an async function and of the first suspend and resume of a
fresh coroutine.

## Profile (no-LTO line build, callee with one await)

- frame copy on suspend and resume (`sv_activation_capture`,
  `sv_activation_install`): ~9%
- GC root pins in the async call and resume paths: ~7%
- coroutine `calloc`/`free`, refcount, hold bits, active list: ~5%
- promise plumbing (create, resolve, reaction append, pending root): ~8%
- interpreter dispatch and the function body: ~16%

`AWAIT` and `YIELD` are not JIT-eligible, so any function that awaits runs in
the interpreter.

## Plan

1. Remove the `return await` rewrite and add a settle-order test that matches
   Node.
2. Make suspend and resume cheap: no per-call coroutine allocation, no per-call
   root pin loops, and no frame copy for a coroutine's own frames.
3. JIT `await` as a planned bailout into an interpreter frame, with OSR resume
   entries at await resume offsets.
4. Follow-up, separate: `Promise.all`, `then` and `new Promise` overheads.

## Validation

- Settle-order test against Node's output.
- `async_seq.js`-style sequential await timings against Node; instruction
  counts for A/B within one build directory.
- Harness and spec suite.

## Decisions

- `return await x` compiles to `OP_AWAIT_RETURN` when the function is async,
  not an async generator, not top-level await, outside `try`, with no pending
  `using` cleanup and no enclosing iterator loop. It registers the await like
  `OP_AWAIT`, sets `coroutine_t.tail_await` and leaves the frame. The resume
  job settles the async promise directly. The settle tick is unchanged because
  the promise settles in the microtask the resume would have run in, so the
  frames are never captured or reinstalled.
- Microtask entries with at most one argument and coroutines come from
  per-isolate free lists (256 entries each). Promise states live in a fixed
  arena (`promise_arena`). A pooled coroutine keeps its sealed activation
  buffer up to 4 KB.
- Default promises no longer store `SLOT_CTOR`; readers fall back to
  `constructor` on the prototype, which is the same function. Subclass and
  species promises still set it.
- Interpreted `CALL`/`CALL_METHOD` of a same-realm async closure runs the body
  in the caller's dispatch loop. `sv_async_inline_call` creates the promise and
  a coroutine, pushes a normal frame and records `(fp, coroutine)` on
  `vm->async_inline`. Every path that leaves or suspends such a frame settles
  through `sv_async_inline_settle` and continues the caller with the promise:
  `RETURN`, `RETURN_UNDEF`, `FINALLY_RET`, the OSR frame pop, `AWAIT`,
  `AWAIT_ITER_NEXT` and `AWAIT_RETURN`. `sv_vm_throw` stops at the innermost
  inline frame, which rejects its promise; terminating errors still unwind the
  entry, and `sv_leave` discards records at or above `entry_fp`. A suspended
  inline frame resumes later as the entry frame of the resumed run, so the
  existing resume path is unchanged. Generators and JIT callers keep the old
  path.
- `then` skips the `constructor` and `@@species` lookups when the species
  protector holds and the receiver's prototype is `Promise.prototype`.
- `Promise.all` on the intrinsic constructor, with intact promise protectors
  and a dense array of primitives and plain native promises, registers
  internal reactions (`nextPromise == PROMISE_ALL_ELEMENT`, tracker in
  `onFulfilled`, index in `onRejected`) instead of resolve and reject element
  functions and a `then` call. Primitives still get a fresh fulfilled promise
  each, so every element settles in its own reaction job as the spec requires.
- Data cfuncs and the `Promise.all`/`allSettled` element functions reserve
  their extra slots up front.
- Rejected: a thread-local pool for small extra-slot arrays (rounded to 2 or 4
  entries). It cost 3% more cycles on `new Promise` and thenable awaits; the
  system allocator is already fast at these sizes.
- Removed the never-emitted `OP_RETURN_ASYNC`.

## Checkpoint

Steps 1 and 2 done, plus the promise follow-ups. Interleaved medians, ns per
operation (`orig` is master with the incorrect rewrite):

| case | orig | now | Node |
|---|---|---|---|
| `await` empty async fn | 81 | 69 | 38 |
| `return await p` | 120 | 90 | 60 |
| `const v = await p` | 142 | 116 | 60 |
| `return await 3` | 80 | 86 | 66 |
| `const v = await 3` | 138 | 113 | 66 |
| `return await thenable` | 192 | 194 | 84 |
| `const v = await thenable` | 282 | 222 | 78 |
| `await new Promise(r => r(i))` | 194 | 174 | 46 |
| `await p.then(f)` | 135 | 93 | 36 |
| `await Promise.resolve(i)` | 64 | 51 | 26 |
| `await Promise.all([4 items])` | 1506 | 296 | 188 |

Validation: harness 463 passed (the 17 failures are the npm examples without
modules, `test_hono_adapter`, and `test_uv_nested_poll`, which also fails
about 1 in 30 runs on master); spec 4342/4342; the 102 async-related test
files match master except timings and the settle-order fixes, which match
Node. New tests: `test_async_inline_calls`, `test_promise_all_fast_path`.

Next: step 3, JIT `await`. `new Promise` and thenable awaits remain 3-4x Node;
most of that is the two resolving functions each needs.

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
  in the caller's dispatch loop. `sv_async_inline_call` pushes a normal frame
  and records `(fp, NULL)` on `vm->async_inline`; the promise and coroutine are
  created only when the frame first suspends or enters OSR
  (`sv_async_inline_ensure_coro`). Every path that leaves or suspends such a
  frame settles through `sv_async_inline_settle` and continues the caller with
  the promise: `RETURN`, `RETURN_UNDEF`, `FINALLY_RET`, the OSR frame pop,
  `AWAIT`, `AWAIT_ITER_NEXT` and `AWAIT_RETURN`. `sv_vm_throw` stops at the
  innermost inline frame, which rejects its promise; terminating errors still
  unwind the entry, and `sv_leave` discards records at or above `entry_fp`. A
  suspended inline frame resumes later as the entry frame of the resumed run,
  so the existing resume path is unchanged. Generators keep the old path.
- An inline frame without a coroutine that returns a primitive to a caller
  whose next op is `AWAIT` or `AWAIT_RETURN` pushes the raw value instead of a
  promise. Awaiting a fulfilled native promise and awaiting a primitive both
  take one tick, so the order is unchanged.
- `return await x` in an inline frame without a coroutine does not suspend.
  When the caller's next op is `AWAIT`, it pushes `x` (or its assimilated
  promise) and sets `vm->await_deferred`; the caller's await then registers
  with `PROMISE_AWAIT_DEFER` / `MT_AWAIT_DEFER`, which re-queues the resume once
  to add the tick the callee's own await would have taken. Otherwise
  `js_promise_tail_await` returns a promise that adopts `x` with the same tick
  count. `test_async_call_await_ticks` pins every form to Node's order.
- JIT, phase J1: async functions (not generators or top-level await) are
  JIT-eligible. `AWAIT`, `AWAIT_RETURN` and `AWAIT_ITER_NEXT` compile to a
  boxed exit tagged `SV_JIT_RESUME_AWAIT`; the interpreter runs the op itself,
  without bailout accounting. The prologue sends every non-OSR entry to
  `jit_helper_async_start`, so compiled bodies run only through OSR, and OSR
  is refused for loops that contain an await (`jit_loop_has_await`). Self tail
  calls are off in async functions, and the known-self call path now returns
  its result when it is in tail position (it fell through to the trailing
  `RETURN_UNDEF` before).
- The JIT no longer clears double-register locals for shared slots whose
  hoisted `SET_LOCAL_UNDEF` is not followed by a numeric init. That rule
  predates the maybe-unassigned dataflow (`jit_clear_maybe_unassigned_dnum`),
  which already covers it, and it kept any `let` declared after a call or an
  `await` boxed (4x slower loops). Functions that can observe TDZ use
  `GET_LOCAL_CHK`, which is not JIT-eligible. `shared_slot` and `bound` were
  removed from `sv_type_info_t`.
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

Steps 1 and 2 done, the promise follow-ups, and step 3 phase J1. Interleaved
medians, ns per operation (`orig` is master with the incorrect rewrite; Node is
22.22):

| case | orig | now | Node |
|---|---|---|---|
| `await` empty async fn | 82 | 48 | 38 |
| `return await p` | 121 | 58 | 63 |
| `const v = await p` | 146 | 122 | 63 |
| `return await 3` | 82 | 54 | 69 |
| `const v = await 3` | 139 | 119 | 69 |
| `return await thenable` | 194 | 172 | 88 |
| `const v = await thenable` | 290 | 236 | 84 |

Not re-measured since the previous checkpoint (ns, orig / then / Node):
`await new Promise(r => r(i))` 194 / 174 / 46, `await p.then(f)`
135 / 93 / 36, `await Promise.resolve(i)` 64 / 51 / 26,
`await Promise.all([4 items])` 1506 / 296 / 188.

Hot loops inside async functions, 4e7 iterations (ms):

| case | orig | now | Node |
|---|---|---|---|
| sync function | 27.5 | 28.0 | 25.5 |
| async, no await | 349.8 | 26.9 | 24.0 |
| async, loop after `await` | 329.6 | 27.4 | 24.2 |

Validation: harness 469 passed, 16 failed (the npm examples without modules
and `test_hono_adapter`, which need packages that are not installed in the
worktree; the baseline binary fails them the same way); spec 103 suites, 0
failures. New tests: `test_async_inline_calls`, `test_promise_all_fast_path`,
`test_async_call_await_ticks`, `test_jit_async_await`,
`test_jit_late_let_numeric`, `test_jit_late_let_numeric_codegen`.

Next: J2, loops that contain an await (resume entries at await offsets, so a
resumed coroutine re-enters compiled code). `const v = await x` is about 2x
Node: the coroutine suspends and resumes through the activation capture.
`new Promise` and thenable awaits remain 3-4x Node; most of that is the two
resolving functions each needs.

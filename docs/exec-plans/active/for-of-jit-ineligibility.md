# for-of JIT Ineligibility

Status: active
Last reviewed: 2026-07-31
Owner: theMackabu

## Goal

Make `for...of` loops JIT-compilable. Today a single `for...of` disqualifies its entire
enclosing function from compilation, so every statement in that function -- property reads,
arithmetic, stores, calls -- runs interpreted.

This is not a loop-body cost. It is a function-level veto.

Scope is **every synchronous `for...of` loop**: arrays, Maps, Sets and strings, and also
user-defined `Symbol.iterator` objects and generators.

Generic iterators cannot be excluded, because the compiler emits the same two opcodes for
them. `sv_iter_advance`'s `default:` case (`src/silver/ops/iteration.h`) calls `next()`,
checks the result is an object, and unpacks `{value, done}` inline -- all inside
`OP_ITER_NEXT`. `OP_ITER_GET_VALUE` is emitted **nowhere**; `grep OP_ITER_GET_VALUE src/`
finds it only in the opcode table and the interpreter dispatch label. It is a dead opcode.

Confirm with the checked-in benchmark's sibling case:

```console
$ ANT_DEBUG="dump/vm:op-warn" ./build/ant /tmp/genericiter.mjs 2>&1 | grep ineligible | sort -u
jit: ineligible op ITER_CLOSE in arrayIter
jit: ineligible op ITER_CLOSE in generatorIter
jit: ineligible op ITER_CLOSE in userIter
jit: ineligible op ITER_NEXT in arrayIter
jit: ineligible op ITER_NEXT in generatorIter
jit: ineligible op ITER_NEXT in userIter
```

**This raises the risk of steps 2-3, it does not lower it.** Flagging `OP_ITER_NEXT`
eligible opts generic iterators in whether or not this plan mentions them, and those are the
loops that call arbitrary user JS through `sv_vm_call` in the middle of the iteration --
directly across the back-edge whose vstack merge the failed attempt showed is the blocker.
Validation must cover a user `Symbol.iterator` and a generator, not just arrays and Maps.

## Evidence

`jit_is_eligible` (`src/silver/swarm.c:2773`) walks the bytecode and rejects the function
if any opcode lacks `SV_OPF_JIT_ELIGIBLE`. `OP_ITER_NEXT` and `OP_ITER_CLOSE` have no
`OP_FLAG` entry in `include/silver/opcode.h` at all, so they default to zero flags. Those
two are the whole veto -- every synchronous `for...of` emits them and nothing else. The JIT
says so itself:

```console
$ ANT_DEBUG="dump/vm:op-warn" ./build/ant repro.mjs
jit: ineligible op ITER_CLOSE in useForOf
jit: ineligible op ITER_NEXT  in useForOf
```

Cost, 200k iterations over a 100-element array of objects:

| loop form                | ant         | node    |
| ------------------------ | ----------- | ------- |
| `for (const x of arr)`   | 26.1 ns/step | 1.2 ns |
| `for (let i = 0; ...)`   | 7.0 ns/step  | 0.9 ns |
| `for..of`, no prop read  | 23.3 ns/step | 0.4 ns |

The indexed loop is 3.7x faster than `for...of` in ant purely because it compiles. In node
the two are indistinguishable.

### Real-world impact

`game-of-life/dist` (150x40 board), phases timed separately after warmup:

| phase    | ant      | node     | ratio     | loop style      |
| -------- | -------- | -------- | --------- | --------------- |
| `dotick` | 1.720 ms | 0.062 ms | **27.7x** | all `for...of`  |
| `render` | 1.340 ms | 0.388 ms | 3.5x      | indexed `for`   |

Overall 322 vs 2183 ticks/sec (6.8x). `dotick` runs ~60,000 iterator steps per tick
(two passes over `Map.values()`, plus `for (const n of this.neighbours)` per cell); at
26 ns that is ~1.56 ms of the measured 1.72 ms, roughly 90% of the phase. `render` uses
indexed loops, compiles, and is only 3.5x off.

Note `cell.js` carries comments claiming the indexed variant is "slower". That is true on
V8 and inverted on ant -- the indexed version is the only one that compiles.

## What already exists

Most of the supporting machinery is in place, which is what makes this tractable:

- `jit_iter_advance_from_buf` (`src/silver/glue.c:323`) -- buffer-based advance, already
  takes the `hint` operand, already writes the updated index back to `iter_buf[1]`. Its
  `default:` case already drives generic iterators, so no new helper is needed for them --
  which is exactly why they cannot be left out of scope.
- `r_iter_roots` -- GC-visible shadow of the three iterator stack slots, populated by
  `OP_FOR_OF` (`src/silver/swarm.c:7696`).
- `r_args_buf` marshalling plus the error/catch protocol.
- `OP_DESTRUCTURE_NEXT` (`swarm.c:7749`) and `OP_DESTRUCTURE_CLOSE` (`swarm.c:7812`) are
  already compiled and are structural siblings of what is missing.
- `jit_helper_destructure_close` (`glue.c:469`) is semantically identical to
  `sv_op_iter_close` -- `OP_ITER_CLOSE` can share its emit case outright.

Stack effects, from `include/silver/opcode.h:217-219`:

```text
OP_DEF(  ITER_NEXT,         2,   3,   5, u8)   /* pops 3, pushes 5, u8 type hint */
OP_DEF(  ITER_GET_VALUE,    1,   2,   3, none)   /* dead: the compiler never emits it */
OP_DEF(  ITER_CLOSE,        1,   3,   0, none)
```

`ITER_NEXT`'s "pop 3 / push 5" is the abstract effect. The interpreter
(`src/silver/ops/iteration.h:206`) leaves the three iterator slots in place, mutates
`sp-2` (the index) and pushes `value` then `done`.

## What was tried and failed

A helper-call port was attempted and reverted. The plumbing worked -- adding the two
`OP_FLAG` entries plus a `jit_helper_iter_next` wrapper cleared both ineligible-op warnings,
and array / `Map.values` / string / generator / throw-mid-loop all behaved correctly on
small inputs.

Anything large enough to actually reach the JIT then failed with
`TypeError: iterator.next is not a function`. Two stack treatments were tried:

1. pop 3 / push 5, copied from `OP_DESTRUCTURE_NEXT` -- reassigns the MIR registers holding
   the three iterator slots.
2. write the three slots back into their existing `vs.regs[]` and push only 2 -- preserves
   register identity, still wrong.

The destructure opcodes are a misleading template because they run in **straight-line
code**. `OP_ITER_NEXT` sits under a **loop back-edge**, so the three iterator slots must
survive the vstack register merge at the loop head, and there is also an OSR entry path
into the middle of the loop. Getting that wrong produces a silent miscompile rather than a
compile error, which is why this needs someone who knows the merge invariants rather than
pattern-matching from the destructure cases.

The estimate given before attempting it -- "a few hundred lines of copy-adapt boilerplate,
no new infrastructure" -- was wrong. The MIR emission is boilerplate; the loop-carried
vstack state is the actual problem.

## Task list

1. **Understand the vstack merge at a loop back-edge.** How are `vs.regs[]` reconciled
   between the loop head and the back-edge, and what does OSR entry expect the iterator
   slots to look like? This is the blocker; everything else is mechanical.
2. **`OP_ITER_NEXT` as a helper call.** `jit_helper_iter_next(vm, js, iter_buf, hint)`
   wrapping `jit_iter_advance_from_buf`, writing `value`/`done` to `iter_buf[3]`/`[4]`.
   Add `OP_FLAG(ITER_NEXT, SV_OPF_JIT_ELIGIBLE | SV_OPF_JIT_NEEDS_ARGS_BUF |
   SV_OPF_JIT_NEEDS_ITER_ROOTS)`, plus `LOAD_EXT` (`swarm.c:66`), a 4-arg proto
   (near `swarm.c:3254`) and import (near `swarm.c:3284`).
3. **`OP_ITER_CLOSE`.** Add the same flags and fall through to the existing
   `OP_DESTRUCTURE_CLOSE` case.
4. **Validate against generic iterators, not just arrays.** Step 2 opts in user
   `Symbol.iterator` objects and generators automatically. Those reach `sv_vm_call` from
   inside `OP_ITER_NEXT`, so user code runs across the back-edge and can reassign the loop
   variable, close the iterator, or throw. Cover all of: array, `Map.values()`, string,
   user `Symbol.iterator`, generator, `return()` on early `break`, and a throw mid-loop.

   `OP_ITER_GET_VALUE` needs no work. The compiler never emits it -- it is dead, and worth
   deleting separately rather than flagging.
5. **Re-measure and re-run `dump/vm:op-warn` on the real `dotick`.** Steps 2-3 only remove
   the veto; if anything else in that function is ineligible the win will not materialise,
   and the warning names the blocker directly.
6. **Then consider inlining the array case.** Only after the glue lands and is measured.

## Decision log

- Helper-call glue first, inlining second. The glue is where the function-level veto is
  lifted, which is most of the win; inlining only addresses the per-step cost.
- `OP_ITER_CLOSE` shares `OP_DESTRUCTURE_CLOSE` rather than getting its own helper --
  `sv_op_iter_close` and `jit_helper_destructure_close` already do the same thing.
- Deliberately *not* landed in a partial state. The failure mode is a miscompile that
  passes small tests and corrupts iterator state under load, which is worse than the
  current slow-but-correct behaviour.

## Scope note: this is glue, not iterator JIT support

There is currently **no** inlined iterator codegen anywhere -- `grep SV_ITER_ARRAY
src/silver/swarm.c` returns nothing. The three iterator-adjacent opcodes the JIT does
support are each a C call:

```text
imp_for_of  -> jit_helper_for_of
imp_dnext   -> jit_helper_destructure_next
imp_dclose  -> jit_helper_destructure_close
```

So the task-list steps above buy function-level eligibility, not fast iteration. Per-step
cost would land somewhere between the current 26 ns and the 7 ns an indexed loop achieves
-- a C call plus the `sv_iter_advance` body. Closing the rest of the gap to node's 1.2 ns
needs the `SV_ITER_ARRAY` case emitted inline in MIR (tag guard, bounds check against
`js_arr_len`, dense load, index bump) with a bail to the helper for `SV_ITER_MAP` /
`SV_ITER_STRING` / `SV_ITER_GENERIC`. That is new codegen, not boilerplate.

The split between "veto cost" and "per-step cost" has not been measured, because that
requires step 2 to work first.

## Validation status

- Timings taken 2026-07-31 on darwin-aarch64, PGO release build
  (`meson --buildtype=release`, pgo enabled), best-of-5, at branch `af22111a`, against
  node v26.5.1.
- Baseline at time of writing: harness 123/0 on `spec tests async`, conformance 1511/1511.

The iteration benchmark is checked in. Reproduce with:

```console
$ ./build/ant examples/jit/bench_iteration.mjs
for..of array                381ms  26.5 ns/step
indexed for                  102ms  7.1 ns/step
for..of, no prop read        340ms  23.6 ns/step
for..of map.values()         348ms  24.2 ns/step

$ node examples/jit/bench_iteration.mjs
for..of array                 17ms  1.2 ns/step
indexed for                   12ms  0.8 ns/step
for..of, no prop read          6ms  0.4 ns/step
for..of map.values()          16ms  1.1 ns/step
```

Confirm the veto is still in place before you trust a "no change" result -- if these opcodes
stop appearing, the ratio has moved for a reason:

```console
$ ANT_DEBUG="dump/vm:op-warn" ./build/ant examples/jit/bench_iteration.mjs 2>&1 | grep ineligible
jit: ineligible op ITER_CLOSE in <anonymous>
jit: ineligible op ITER_NEXT in <anonymous>
```

Absolute times move between machines. The ratio between `for..of array` and `indexed for`
is the stable signal: 3.7x on ant, 1.4x on node. Any fix has to move that ratio toward 1.

The `dotick`/`render` split came from `game-of-life/dist`, timed by calling `w.dotick()` and
`w.render()` in separate loops after 200 warmup ticks. That one is not checked in -- it
depends on the `game-of-life` working tree, which is not part of this repository.

## Follow-ups

- `Map.get` is separately 8-10x node (41.7 ns vs 4.2 for a string key) and scales with key
  length where node is flat, because V8 caches a string's hash in the string object while
  ant re-hashes on every lookup. Unrelated to this plan and ~13% of the game-of-life tick.
  `ant_flat_string_t` is 16 bytes behind a hard `static_assert`, with `meta` holding a
  56-bit UTF-16 length and 2 of 8 ASCII bits -- so caching a hash means either +8 bytes per
  string header or shrinking the wildly oversized length field.

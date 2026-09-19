# Silver Instruction Sizes

Status: completed
Last reviewed: 2026-09-18
Owner: theMackabu

## Outcome

Removed the interpreter TODO by deriving all 213 `NEXT` advances and six
saved continuation addresses from `sv_op_size`, which is generated from
`include/silver/opcode.h`.

## Decisions

- Pass explicit opcode constants to `NEXT` so ordinary handler advances can
  still fold to constants. The shared `yield*` handler uses its current opcode.
- Convert saved continuation addresses for calls, await, and yield as well.
- Preserve operand offsets. In particular, `UNWIND_JMP` is seven bytes long
  but its relative displacement is based at byte five, before its two counts.
- Keep the previously completed local `bp` removal; frame argument storage
  and the live `lp` cache remain required.

## Validation

- Verified that all 213 dispatch advances and six saved continuations resolve
  to their original lengths, with no other engine logic changes.
- `maid preflight` and the changed-file whitespace check passed.
- `meson compile -C build` passed with PGO profile-mismatch warnings.
- Five focused regressions passed: arguments metadata, async arguments,
  function call/apply/bind, JIT tail calls, and existing parameter upvalues
  across OSR.
- The functions, exceptions, generators, async, async loops, and async
  iterators specs passed: 227 tests, zero failures.
- Full spec and performance suites were not run; this refactor preserves
  instruction lengths and makes no performance claim.

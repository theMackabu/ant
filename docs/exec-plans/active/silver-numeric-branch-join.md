# Silver Numeric Branch Join Regression

Status: active
Last reviewed: 2026-09-07
Owner: theMackabu

## Problem and invariant

PR #95 made integer constants and range-proven expressions use `SLOT_I32`.
Explicit bytecode jumps box the virtual stack, but the fallthrough path into
a branch target reset its slot types to `SLOT_BOXED` without converting values.
Every incoming path must supply boxed values before the target label.

In `examples/jit/bailout_resume.js`, the `: 1` arm reaches ADD at bytecode offset
96 in `nestedForOf` with raw integer bits. MIR emits `mov s4, 1`, the target
label, then `i2db add_d2_0, s4`: the bitcast reads `5e-324` instead of `1`.
The sum is already corrupted before the coercion bailout at iteration 600.
The double-local mirror, bailout reboxing, and interpreter resume copy preserve
that corrupted value; they cannot recover the intended operand.

## Decision and scope

Flush the fallthrough virtual stack before emitting a branch target label and
resetting its type metadata, using the same helper as explicit jumps. This
covers integer and double slots while taken branches skip the conversion.
Disabling integer lowering would discard PR #95's optimization without fixing
the missing conversion for double slots. Changing local writeback or the
resume interface cannot repair corruption introduced earlier by ADD.

Retain the three bailout resume examples and add focused hot branch coverage
for constants, integer arithmetic, a live numeric operand, and short circuits.

## Validation

- Before the fix, Ant returns `1065` for both nested sums and
  `10+606,20+399,30+3.46e-321` for the per-outer totals. Node returns `2166`
  and `10+706,20+700,30+700`.
- The focused branch test fails before the fix at call 101:
  `integer expression join at call 101: 0.5 != 104.5`. Node passes.
- Build, focused tests, JIT harness, spec suite, and final preflight pending.
- The configured tree needs `nix develop -c meson compile -C build` for its
  pinned compiler, linker, and macOS SDK environment.

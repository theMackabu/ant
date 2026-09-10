# Eval Binding Fixes

Status: completed
Last reviewed: 2026-09-10
Owner: theMackabu

Historical implementation record extracted from the execution-plan index.
The validation below was recorded during implementation, not rerun during
the documentation cleanup.

## Outcome And Decisions

- `typeof arguments` skips the implicit object when the function owns an eval
  environment, retaining the existing undefined-safe lookup fallback.
- The eval pre-scan checks parameter patterns and scoped body declarations
  before deciding `owns_eval_env`. Body bindings do not shadow parameter
  initializers; block, loop, catch, and switch bindings do not hide calls
  outside their scope. Static local/upvalue and spread eligibility checks
  remain in place.
- Inherited eval calls resolve the callee before arguments and compare it
  with the named builtin installed by global initialization (`js_builtin_eval`).
  Arguments are emitted once with the identity flag above them on the stack;
  dispatch selects an ordinary call or `OP_EVAL`. This uses existing opcodes.
  Inherited builtin calls use runtime eval rather than literal inlining.

## Recorded Validation

The saved pre-fix binary failed all eight core parameter/body-shadowing cases
with `value` undefined. Its nested-bytecode regression grew from 541 to 8,941
bytes at depths 4 and 8. The fixed function grew linearly: 148, 280, and 412
bytes at depths 4, 8, and 12.

Focused eval regressions passed, including scope-boundary and argument-effect
controls. The full spec suite passed 4,221 tests across 102 files. Repository
preflight and diff checks passed.

## Original Record

```sh
git show bc206f10a9ea73d3d91302eb208adf1479e4d22e:docs/exec-plans/index.md
```

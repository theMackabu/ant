# Iterator Cleanup on Return

Status: complete
Owner: theMackabu
Baseline: `87c9299b` with the preceding error-boundary fixes applied locally

## Outcome

Returns now close active for-of and for-await-of iterators from inner to outer,
including when finally blocks intervene. Close failures propagate with their
original values, and async closing completes before the function returns.
The implementation shares the existing labeled-jump unwind emitter and checked
iterator-close opcodes; it adds no opcode or interpreter/JIT-specific path.

The return payload uses the outermost iterator's existing completion local.
That local outlives intervening finally blocks, preventing their local slots
from overwriting the payload. Explicit unwinding stops at the outermost
iterator; ordinary return handling runs any enclosing finally blocks.

Tests also exposed normal exhaustion unnecessarily calling iterator.return.
The compiler now simply drops the exhausted iterator record. This matters
when a finally block cancels a pending return with continue and the loop
subsequently finishes normally.

## Validation

- Both new regression files fail on the pinned pre-fix binary and pass in Ant
  and Node. They cover normal and bare returns, nesting, finally ordering and
  replacement completions, exact throw identity, closure captures, generators,
  for-using disposal, and synchronous/asynchronous exhaustion.
- `test_return_for_of_close.cjs` repeats functions across the JIT threshold.
  Debug output confirms JIT compilation of `first`, `noValue`, and
  `nestedPlain`. Finally-heavy functions retain the existing interpreter
  fallback; no new JIT support is claimed for finally opcodes.
- Configured macOS ARM64 build passes. Focused iterator-close, labeled-jump,
  disposal, and new sync/async return regressions pass.
- Full specs: **4,240 tests across 102 files, zero failures**.
- Stack-depth sweep: 604 runtime files plus specs and JIT examples, no operand
  analysis rejections or JIT stack overflows. Exit 2 remains attributable to
  the same three baseline failures: `test_node_events_once_prototype_spoof.cjs`,
  `test_throw_stack.cjs`, and `test_with_strict.cjs`.
- Preflight and whitespace checks pass. The unrelated PGO profile has the same
  SHA-256 as at task entry. No build-graph changes require reconfiguration.

No subagents were used. Local baseline, JIT evidence, and validation logs are in
`/tmp/ant-iterator-return`; the two regression files are registered in the harness.

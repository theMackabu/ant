# Guarded charCodeAt opcode

Status: active
Last reviewed: 2026-09-05
Owner: theMackabu

## Scope and design

`CALL_CHAR_CODE_AT(argc)` follows the existing `CALL_ARRAY_INCLUDES` pipeline.
The compiler selects ordinary `.charCodeAt(...)` calls and bindings destructured
from `StringPrototypeCharCodeAt`, including renamed locals and nested captures.
Binding hints only select an opcode: they do not prove runtime identity. No
bare-identifier name heuristic, primordial runtime coupling, or generic call
dispatch hook is introduced. Optional, spread, and tail calls retain their
existing lowering.

Interpreter and JIT use the same operation. It recognizes genuine native
charCodeAt calls and supported bound forms, including call.bind(charCodeAt).
Flat ASCII strings with numeric indices use an allocation-free read. Other
inputs, proxies, pre-applied arguments, and replaced functions use normal VM
dispatch. Original property lookup and argument evaluation are retained.
The JIT now inlines guarded reads through native Function.call.bind wrappers
for in-range integer indices on known-ASCII flat or cached-flat rope strings.
Other cases use the dedicated C helper. The immutable wrapper flag is general
call metadata, not a primordial lookup. See the path performance plan for
per-change measurements and expanded guard tests.

Fallback testing also requires propagating receiver/index conversion exceptions
and rejecting extreme numeric indices before a signed integer conversion.

## Validation

Build, focused guards, primordial tests, and 100 Node path differential probes
pass. MIR inspection confirms dedicated helper calls in minified path code.
The full spec suite fails only fetch DNS resolution (`unknown node or service`),
also reproduced with the preserved binary. Artifacts:
`/tmp/ant-charcode-opcode.qBcmk0`.

Serial ABBA (six samples per binary, 100k calls, 20k warmup) against the supplied
pre-change artifact: POSIX normalize 85.91 -> 68.29 ms, Windows normalize
126.76 -> 111.17 ms, POSIX join 143.05 -> 120.45 ms. POSIX relative regressed
257.68 -> 283.79 ms; resolve/Windows relative were approximately unchanged.
Checksums matched. This is not a blanket speedup or installed-C-path parity
claim; builds use existing PGO inputs with stale-profile warnings. Further
performance attribution remains open. Raw hashes and samples are in results.json.

## Restored compatibility fixes

The reset also removed two earlier fixes, restored separately at user request:
Reflect.set now rejects own non-writable/getter-only string properties before
assignment and propagates setter exceptions. assert.throws checks constructor
matches before predicates, rejects mismatched Error subclasses, and roots its
inputs and thrown value across user callbacks. This is not a complete rewrite
of either API's other compatibility behavior.

New focused tests cover frozen objects/functions, accessors, constructor and
subclass matching, and predicate callbacks. Both tests pass under Node and Ant.
The incremental build, assert/reflect specs, primordial and charCodeAt guard
tests, and preflight all pass.

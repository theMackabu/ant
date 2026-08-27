# WebAssembly Embedding Package

Status: active
Owner: theMackabu

## Problem

Ant can be embedded through its C API, but JavaScript applications cannot yet
instantiate the Silver interpreter as a WebAssembly module. The first package
must expose an isolated evaluator without pulling the Ant CLI, package manager,
native FFI, subprocess, server, or JIT surfaces into the browser build.

## Intended outcome

Add `packages/wasm` as the source of `@antjs.org/wasm` with this initial API:

- `Ant.create({ memoryLimit, timeout, globals })`
- `ant.eval(source)` with JavaScript value conversion
- synchronous host functions supplied through `globals`
- `ant.dispose()` and `Symbol.dispose`

Each `Ant` owns its WebAssembly instance and linear memory. Evaluation is
stateful within an instance and isolated from other instances.

## Constraints

- Compile the parser, bytecode compiler, interpreter, and GC; do not compile
  the Ant CLI or Silver JIT.
- The npm package must work in browsers and Node without a native executable.
- Reject unsupported cross-boundary values explicitly instead of silently
  changing their meaning.
- Enforce timeouts inside the interpreter so a loop cannot permanently block
  the caller.
- Treat `memoryLimit` as a hard per-instance WebAssembly memory ceiling.

## Milestones

1. Establish and build an interpreter-only Clang/WASI reactor target.
2. Add a narrow C ABI for isolate lifecycle, evaluation, host calls, and value
   serialization.
3. Add the TypeScript API, declarations, package metadata, and documentation.
4. Validate lifecycle, isolation, conversions, globals, limits, and packaging.
5. Add runnable Node use cases and an interactive browser playground under
   `packages/wasm/examples`.

## Decision log

- Use the public eval semantics (`js_eval_bytecode_eval`) so evaluation returns
  the completion value and preserves state across calls.
- Instantiate one WebAssembly module per `Ant`; this makes the linear-memory
  ceiling an isolate boundary rather than a shared process-wide setting.
- Keep v1 evaluation synchronous inside the module but expose a Promise-based
  JavaScript API. A worker transport can be added without changing callers.
- Use wasi-sdk directly instead of Emscripten. The package pins wasi-sdk 34,
  verifies the downloaded archive, and owns its loader and ABI.
- Import each instance's memory from the host so `memoryLimit` is enforced by
  the WebAssembly memory maximum. Link with a 2 GiB absolute upper bound.
- Use a versioned binary value format across the ABI. This avoids generated
  runtime helpers and preserves non-finite numbers, negative zero, BigInts,
  `undefined`, arrays, and enumerable object data.
- Keep wire tags, transfer limits, response statuses, and memory bounds in
  `packages/wasm/abi.json`; the build generates the C and JavaScript views of
  that contract.
- Preserve JavaScript strings as WTF-8 across the host boundary so lone UTF-16
  surrogates survive round trips. Define decoded keys as exact own data
  properties so `__proto__` cannot change prototypes.
- Keep the execution deadline active through Promise checkpoints and result or
  exception transfer. A dedicated timeout response status cannot be forged by
  guest exception text.
- Drain the interpreter's synchronous Promise jobs in a package-local
  microtask queue. Promise objects remain unsupported transfer values because
  the v1 host call itself is synchronous.
- Keep the complete Wasm import/export list as a build-time allowlist. The WASI
  subset has no filesystem preopens, environment entries, network access, or
  process surface beyond trapping `proc_exit`.
- Do not enable Wasm exception handling solely for conservative GC register
  spilling. The Wasm target scans its linear-memory C stack and is covered by a
  repeated-allocation/root-retention test; native targets retain `setjmp`.

## Validation status

Validated locally on macOS arm64 with wasi-sdk 34:

- `npm test` in `packages/wasm`
- exact Wasm import/export contract inspection
- lifecycle, state isolation, binary value transfer, synchronous host calls,
  guest errors, uncatchable timeout checks, repeated GC pressure, disposal,
  and a 32 MiB hard-ceiling OOM case
- `npm pack --dry-run`, including Ant's license and notices for every statically
  linked dependency
- TypeScript declaration checking and the package test suite on Node 18 and 22
- the native Ant build and full 3,972-test spec suite

The browser fixture could not be run because no browser runtime was available
in the local test environment. A Node 18/22 Linux and Node 22 Windows CI matrix
is configured for the remaining host toolchains; its first remote execution is
still pending.

The examples gallery was added after this validation pass. Its execution and
browser smoke test were not run at the user's request.

The post-review boundary hardening, shared portable utility extraction, ABI
generation, packaging fixes, and CI build reuse were implemented after the
validation listed above. No new builds or tests were run at the user's request;
browser automation remains intentionally out of scope.

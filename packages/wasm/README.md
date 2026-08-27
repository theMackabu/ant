# @antjs.org/wasm

Embed the Ant Silver interpreter for sandboxing using WASM. Each `Ant`
instance owns its WebAssembly memory and keeps its own global state.

```js
import { Ant } from '@antjs.org/wasm';

const ant = await Ant.create();
try {
  const result = await ant.eval(`
    Array.from({ length: 5 }, (_, i) => i ** 2)
  `);
  console.log(result); // [0, 1, 4, 9, 16]
} finally {
  ant.dispose();
}
```

Configure a hard memory ceiling, a guest execution timeout, and synchronous
host functions at creation time:

```js
import { Ant } from '@antjs.org/wasm';

const ant = await Ant.create({
  memoryLimit: 64 * 1024 * 1024,
  timeout: 1_000,
  globals: { greet: name => `Hello, ${name}!` }
});

try {
  const value = await ant.eval(`greet("Silver")`);
  console.log(value); // Hello, Silver!
} finally {
  ant.dispose();
}
```

## API

`Ant.create(options?)` asynchronously instantiates a fresh engine. The default
memory limit is 128 MiB. The minimum is 32 MiB and WebAssembly applies the
limit in 64 KiB pages, so `memoryLimit` must be page-aligned. A timeout of `0`
(the default) disables the execution deadline.

`ant.eval(source)` evaluates source with persistent global state and returns a
Promise for its completion value. Call `dispose()` when explicit resource
management syntax is not available. On runtimes that implement explicit
resource management, `using ant = await Ant.create()` disposes automatically.

Numbers (including non-finite values and negative zero), booleans, strings,
BigInts, `null`, `undefined`, arrays, and enumerable string-keyed object data
cross the boundary. Cycles, symbols, and nested functions are rejected. A
function supplied directly in `globals` can be called by guest code, but it
must return synchronously. Guest module imports, native APIs, subprocesses,
networking, the package manager, and JIT compilation are not part of this
interpreter-only build.

Promise reactions and resolved `await` continuations run at the evaluation
checkpoint. Promise objects themselves cannot cross the host boundary, and
this package does not provide timers or external asynchronous I/O inside the
guest.

The timeout interrupts Silver bytecode execution. It cannot preempt a host
callback while that callback is running on the same JavaScript thread. Put an
instance in a Web Worker when the embedding must also isolate host callbacks
from the page's main thread.

## Examples

The [`examples`](./examples) gallery covers basic evaluation, host functions,
state isolation, a rules-engine pattern, limits, and error handling. In a
repository checkout, build once and then run all Node examples:

```sh
npm run build
npm run examples
```

## Build

```sh
npm run build
```

Run `npm test` for package validation.

The build downloads the host build of wasi-sdk 34, verifies its SHA-256 digest,
and compiles a `wasm32-wasip1` reactor with Clang and wasm-ld. Set
`WASI_SDK_PATH` to use an existing wasi-sdk 34 installation. Generated build,
cache, and package artifacts stay under this package and are ignored by Git.

The published package contains the stripped `ant.wasm`, the Ant-owned ESM
loader, and declarations. It contains no Emscripten loader. The build rejects
any unreviewed Wasm import or export; the retained WASI Preview 1 calls are a
small libc subset implemented by the loader without filesystem preopens or
environment access.

Serve `ant.wasm` with `Content-Type: application/wasm` for streaming browser
compilation. The loader falls back to buffered compilation when that MIME type
is unavailable.

# 🐜 Ant

**Ant-sized JavaScript Runtime**

A JavaScript runtime that fits in your pocket — Full async/await, modules, HTTP servers, crypto, and more.

📖 [Read the blog post about Ant](https://s.tail.so/js-in-one-month)

## Installation

```bash
curl -fsSL https://ant.themackabu.com/install | bash

# or with MbedTLS (darwin only)
curl -fsSL https://ant.themackabu.com/install | MBEDTLS=1 bash
```

## Build from Source

```bash
meson setup build && meson compile -C build
```

## Quick Example

See `examples/server/server.js` for a complete server example using `Ant.serve()` with `rou3` routing, parameter handling, and various response types.

## Modules

### HTTP & Networking

| API           | Description                                     |
| ------------- | ----------------------------------------------- |
| `Ant.serve()` | HTTP server with uv_tcp (TLS support via tlsuv) |
| `fetch()`     | HTTP client with TLS support (GET, POST, etc.)  |
| URL imports   | Import directly from URLs                       |

### Timers & Scheduling (built-in)

| API                | Description                              |
| ------------------ | ---------------------------------------- |
| `setTimeout()`     | Execute callback after delay             |
| `setInterval()`    | Execute callback repeatedly              |
| `setImmediate()`   | Execute callback on next event loop tick |
| `clearTimeout()`   | Cancel scheduled timeout                 |
| `clearInterval()`  | Cancel scheduled interval                |
| `queueMicrotask()` | Queue microtask for execution            |

### File System

```js
import { readFile, writeFile } from 'ant:fs';
```

**Async:**
`readFile()`, `writeFile()`, `unlink()`, `mkdir()`, `rmdir()`, `readdir()`, `stat()`

**Sync:**
`readFileSync()`, `writeFileSync()`, `unlinkSync()`, `mkdirSync()`, `rmdirSync()`, `readdirSync()`, `statSync()`

### Shell Commands

```js
import { $ } from 'ant:shell';

const result = await $`ls -la`;
console.log(result.text());
```

| Property          | Description                  |
| ----------------- | ---------------------------- |
| `result.text()`   | Get stdout as string         |
| `result.lines()`  | Get stdout as array of lines |
| `result.exitCode` | Command exit code            |
| `result.stdout`   | Raw stdout                   |
| `result.stderr`   | Raw stderr                   |

### Child Process

```js
import { spawn, exec } from 'child_process';
```

`spawn()`, `exec()`, `execSync()`, `spawnSync()`, `fork()`, `kill()`

Events: `on('exit')`, `on('close')`, `on('error')`, `on('data')`

### Readline

```js
import { createInterface } from 'readline';
```

`createInterface()`, `question()`, `on('line')`, `on('close')`, `pause()`, `resume()`, `close()`

Includes command history with navigation support.

### OS

```js
import os from 'os';
```

`arch()`, `platform()`, `type()`, `release()`, `version()`, `hostname()`, `homedir()`, `tmpdir()`, `cpus()`, `totalmem()`, `freemem()`, `uptime()`, `networkInterfaces()`, `userInfo()`, `constants`, `EOL`

### Navigator

```js
navigator.userAgent;            // User agent string
navigator.platform;             // Platform string
navigator.hardwareConcurrency;  // CPU thread count
navigator.locks;                // Web Locks API
```

### Cryptography

```js
crypto.random();           // Secure random number
crypto.randomBytes();      // Generate random bytes
crypto.randomUUID();       // UUID v4
crypto.randomUUIDv7();     // UUID v7 (time-ordered)
crypto.getRandomValues();  // Fill TypedArray with random values
btoa();                    // Base64 encoding (built-in)
atob();                    // Base64 decoding (built-in)
```

### Path Utilities

```js
import { join, resolve, basename } from 'ant:path';
```

`basename()`, `dirname()`, `extname()`, `join()`, `resolve()`, `normalize()`, `isAbsolute()`

### Process

```js
Ant.process.env;         // Environment variables (with .env support)
Ant.process.cwd;         // Current working directory
Ant.process.argv;        // Command line arguments
Ant.process.pid;         // Process ID
Ant.process.exit();      // Exit with code
Ant.process.cpuUsage();  // CPU usage statistics
```

### Performance

```js
performance.now();       // High-resolution timestamp
performance.timeOrigin;  // Time origin for measurements
```

### Ant Global

```js
Ant.version;    // Runtime version
Ant.target;     // Build target
Ant.revision;   // Git revision
Ant.buildDate;  // Build date
Ant.serve();    // Start HTTP server
Ant.signal();   // Register signal handlers
Ant.sleep();    // Sleep in seconds
Ant.msleep();   // Sleep in milliseconds
Ant.usleep();   // Sleep in microseconds
Ant.gc();       // Trigger garbage collection
Ant.alloc();    // Get memory allocation info
Ant.stats();    // Get runtime statistics
Ant.typeof();   // Get internal type name
```

### Foreign Function Interface (FFI)

```js
import { dlopen, define } from 'ant:ffi';
```

`dlopen()`, `define()`, `alloc()`, `free()`, `read()`, `write()`, `callback()`, `freeCallback()`, `pointer()`, `readPtr()`, `suffix`, `FFIType`

### Binary Data

**Buffers:** `ArrayBuffer`, `SharedArrayBuffer`, `Buffer`

**TypedArrays:** `Int8Array`, `Uint8Array`, `Uint8ClampedArray`, `Int16Array`, `Uint16Array`, `Int32Array`, `Uint32Array`, `Float32Array`, `Float64Array`, `BigInt64Array`, `BigUint64Array`

**Utilities:** `DataView`, `TextEncoder`, `TextDecoder`

### Atomic Operations

```js
Atomics.add()      Atomics.sub()       Atomics.and()
Atomics.or()       Atomics.xor()       Atomics.load()
Atomics.store()    Atomics.exchange()  Atomics.compareExchange()
Atomics.wait()     Atomics.notify()    Atomics.isLockFree()
```

### Web Storage

**localStorage** (file-persistent):

```js
localStorage.setFile('./data.json'); // Required before use
localStorage.setItem('key', 'value');
localStorage.getItem('key');
```

**sessionStorage** (in-memory): Same API without `setFile()`.

### Module System

```js
import 'ant:fs'                 // Built-in fs module
import 'ant:path'               // Built-in path module
import 'ant:shell'              // Built-in shell module
import 'ant:ffi'                // Built-in FFI module
import 'node:*'                 // Node.js-style aliases
import from 'https://...'       // Import from URLs
import data from './data.json'  // JSON imports
import text from './file.txt'   // Text imports
```

## TypeScript Support

Built-in TypeScript type stripping via oxc (no type checking, strip only):

```bash
./build/ant script.ts
```

Type annotations are stripped at parse time. Full type definitions available in `src/types/`.

## JavaScript Features

### ES1-ES5 Core

Automatic Semicolon Insertion (ASI), var hoisting, try/catch/finally, for...in loops, regular expressions, strict mode, `Object.defineProperty()`, `Object.freeze()`/`seal()`/`preventExtensions()`, `Function.prototype.call`/`apply`/`bind`, Array methods (map, filter, reduce, forEach, etc.), String methods, Math object, arguments object, labeled statements.

### ES6+ Extensions

Async/await and Promises, arrow functions, classes with private fields (`#privateField`), template literals, destructuring, spread/rest operators, optional chaining (`?.`), nullish coalescing (`??`), logical assignment (`??=`, `&&=`, `||=`), for...of loops, let/const, BigInt, numeric separators (`1_000_000`), Map/Set/WeakMap/WeakSet, Symbol, Proxy/Reflect, default parameters, computed property names, shebang support.

## Concurrency

- Minicoro-based coroutines for async/await
- Event loop with microtask queue
- Atomic operations for lock-free concurrent programming
- SharedArrayBuffer for shared memory between workers
- Atomics.wait/notify for thread synchronization
- Virtual memory allocation for coroutine stacks

## System

- Signal handlers (SIGINT, SIGTERM, etc.) via `Ant.signal()`
- Mark-copy compacting garbage collector + Boehm-Demers-Weiser
- libuv-based async I/O for files and networking
- TLS support via mbedtls or tlsuv
- LTO (Link Time Optimization) build support
- Gzip compression support for HTTP responses
- Native library integration via FFI

## License

MIT License - See [LICENSE.txt](LICENSE.txt) for details.

ANT - Ant sized JavaScript Runtime
===================================

A minimal embedded JavaScript engine with async/await, promises, modules,
and built-in APIs for HTTP servers, timers, crypto, and JSON.

BUILD:
  meson setup build && meson compile -C build

RUN:
  ./build/ant script.js

EXAMPLE:
  See examples/server/server.js for a good server example using Ant.serve()
  with Radix3 routing, parameter handling, and various response types.

MODULES:

  HTTP & Networking:
    - Ant.serve()         - HTTP server with uv_tcp (TLS support via tlsuv)
    - fetch()             - HTTP client with TLS support (GET, POST, etc.)

  Timers & Scheduling (built-in):
    - setTimeout()        - Execute callback after delay
    - setInterval()       - Execute callback repeatedly
    - setImmediate()      - Execute callback on next event loop tick
    - clearTimeout()      - Cancel scheduled timeout
    - clearInterval()     - Cancel scheduled interval
    - queueMicrotask()    - Queue microtask for execution

  File System (import from 'ant:fs'):
    Async:
      - readFile()        - Read file asynchronously
      - writeFile()       - Write file asynchronously
      - unlink()          - Delete file asynchronously
      - mkdir()           - Create directory asynchronously
      - rmdir()           - Remove directory asynchronously
      - stat()            - Get file statistics
    Sync:
      - readFileSync()    - Read file synchronously
      - writeFileSync()   - Write file synchronously
      - unlinkSync()      - Delete file synchronously
      - mkdirSync()       - Create directory synchronously
      - rmdirSync()       - Remove directory synchronously
      - statSync()        - Get file statistics synchronously

  Shell Commands (import { $ } from 'ant:shell'):
    - $`command`          - Execute shell commands with tagged template literals
    - result.text()       - Get stdout as string
    - result.lines()      - Get stdout as array of lines
    - result.exitCode     - Command exit code
    - result.stdout       - Raw stdout
    - result.stderr       - Raw stderr

  Cryptography (crypto):
    - random()            - Cryptographically secure random number
    - randomBytes()       - Generate random bytes
    - randomUUID()        - Generate UUID v4
    - randomUUIDv7()      - Generate UUID v7 (time-ordered)
    - getRandomValues()   - Fill TypedArray with random values
    - btoa()              - Base64 encoding (built-in)
    - atob()              - Base64 decoding (built-in)

  Path Utilities (import from 'ant:path'):
    - basename()          - Get filename from path
    - dirname()           - Get directory from path
    - extname()           - Get file extension
    - join()              - Join path segments
    - resolve()           - Resolve absolute path
    - normalize()         - Normalize path
    - isAbsolute()        - Check if path is absolute

  Process (Ant.process):
    - env                 - Environment variables (with .env file support)
    - pid                 - Process ID
    - exit()              - Exit process with code

  JSON:
    - JSON.parse()        - Parse JSON string (using yyjson)
    - JSON.stringify()    - Stringify to JSON
    - console.log()       - Log with colored JSON output
    - console.error()     - Log errors
    - console.warn()      - Log warnings
    - console.dir()       - Log objects

  Foreign Function Interface (import from 'ant:ffi'):
    - dlopen()            - Load dynamic library
    - define()            - Define C function signature
    - alloc()             - Allocate memory
    - free()              - Free memory
    - read()              - Read from pointer
    - write()             - Write to pointer
    - callback()          - Create C callback from JS function
    - freeCallback()      - Free callback memory
    - pointer()           - Get pointer from value
    - readPtr()           - Read pointer value
    - suffix              - Platform library suffix (.so, .dylib, .dll)
    - FFIType             - Type constants for FFI

  Binary Data:
    - ArrayBuffer         - Fixed-length binary data buffer
    - SharedArrayBuffer   - Shared memory buffer for concurrency
    - TypedArrays:
      * Int8Array, Uint8Array, Uint8ClampedArray
      * Int16Array, Uint16Array
      * Int32Array, Uint32Array
      * Float32Array, Float64Array
      * BigInt64Array, BigUint64Array
    - DataView            - Low-level interface to ArrayBuffer
    - Buffer              - Node.js-compatible Buffer (Uint8Array subclass)

  Atomic Operations:
    - Atomics.add()             - Atomic addition
    - Atomics.sub()             - Atomic subtraction
    - Atomics.and()             - Atomic bitwise AND
    - Atomics.or()              - Atomic bitwise OR
    - Atomics.xor()             - Atomic bitwise XOR
    - Atomics.load()            - Atomic load
    - Atomics.store()           - Atomic store
    - Atomics.exchange()        - Atomic exchange
    - Atomics.compareExchange() - Atomic compare-and-exchange
    - Atomics.wait()            - Wait for shared memory location
    - Atomics.notify()          - Notify waiters on shared memory
    - Atomics.isLockFree()      - Check if size is lock-free

  Events:
    - addEventListener()    - Register event listener
    - removeEventListener() - Remove event listener
    - dispatchEvent()       - Dispatch custom event

  Web Storage:
    localStorage (file-persistent):
      - setFile()           - Set storage file path (required before use)
      - setItem()           - Store key-value pair
      - getItem()           - Retrieve value by key
      - removeItem()        - Remove key-value pair
      - clear()             - Clear all stored data
      - key()               - Get key at index
      - length              - Number of stored items

    sessionStorage (in-memory, session-scoped):
      - setItem()           - Store key-value pair
      - getItem()           - Retrieve value by key
      - removeItem()        - Remove key-value pair
      - clear()             - Clear all stored data
      - key()               - Get key at index
      - length              - Number of stored items

  Symbol:
    - Symbol()              - Create unique symbol
    - Symbol.iterator       - Well-known iterator symbol
    - Symbol.toStringTag    - Well-known toStringTag symbol

  Module System:
    - import()              - Dynamic ESM module import
    - import.meta.url       - Current module URL
    - import.meta.dirname   - Current module directory
    - import.meta.resolve() - Resolve module specifier
    - export / import       - Static ESM imports/exports
    - import 'ant:fs'       - Import built-in fs module
    - import 'ant:path'     - Import built-in path module
    - import 'ant:shell'    - Import built-in shell module ($`...`)
    - import 'ant:ffi'      - Import built-in FFI module

JAVASCRIPT FEATURES:
  Full ES1-ES5 compliance with ES6+ extensions:

  ES1-ES5 Core:
  - Automatic Semicolon Insertion (ASI)
  - var hoisting and function declarations
  - try/catch/finally error handling
  - for...in loops and property enumeration
  - Regular expressions with full pattern matching
  - Strict mode support ("use strict")
  - Object.defineProperty() with property descriptors
  - Function.prototype.call/apply/bind
  - Array methods (map, filter, reduce, forEach, etc.)
  - String methods (split, replace, includes, startsWith, etc.)
  - Math object with all standard functions

  ES6+ Extensions:
  - Async/await and Promises (Promise.all, Promise.race, Promise.resolve, etc.)
  - Arrow functions, classes, template literals, destructuring
  - Spread operator and rest parameters
  - Optional chaining (?.) and nullish coalescing (??)
  - for...of loops with iterables
  - let/const block scoping
  - BigInt support with arithmetic operations
  - Number literals (binary 0b1010, octal 0o755, hex 0xFF)
  - Built-in collections: Map, Set, WeakMap, WeakSet
  - Getter/setter methods in class expressions (get/set)
  - Symbol with well-known symbols (iterator, toStringTag)
  - Block-level function declaration hoisting

CONCURRENCY:
  - Minicoro-based coroutines for async/await
  - Event loop with microtask queue
  - Atomic operations for lock-free concurrent programming
  - SharedArrayBuffer for shared memory between workers
  - Atomics.wait/notify for thread synchronization

SYSTEM:
  - Signal handlers (SIGINT, SIGTERM, etc.)
  - Mark-and-sweep garbage collector with free list optimization
  - Property lookup caching for performance
  - Dynamic memory growth with configurable limits
  - Native library integration via FFI
  - libuv-based async I/O for files and networking
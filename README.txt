ANT - Ant sized JavaScript Runtime
==================================

A minimal embedded JavaScript engine with async/await, promises, modules,
and built-in APIs for HTTP servers, timers, crypto, and JSON.
Check about my blog post about Ant! https://s.tail.so/js-in-one-month

INSTALL:
```
curl -fsSL https://ant.themackabu.com/install | bash

# or with MbedTLS (darwin only)
curl -fsSL https://ant.themackabu.com/install | MBEDTLS=1 bash
```

BUILD:
  meson setup build && meson compile -C build

EXAMPLE:
  See examples/server/server.js for a good server example using Ant.serve()
  with `rou3` routing, parameter handling, and various response types.

MODULES:

  HTTP & Networking:
    - Ant.serve()         - HTTP server with uv_tcp (TLS support via tlsuv)
    - fetch()             - HTTP client with TLS support (GET, POST, etc.)
    - URL module imports  - Import directly from URLs

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
      - readdir()         - Read directory contents asynchronously
      - stat()            - Get file statistics
    Sync:
      - readFileSync()    - Read file synchronously
      - writeFileSync()   - Write file synchronously
      - unlinkSync()      - Delete file synchronously
      - mkdirSync()       - Create directory synchronously
      - rmdirSync()       - Remove directory synchronously
      - readdirSync()     - Read directory contents synchronously
      - statSync()        - Get file statistics synchronously

  Shell Commands (import { $ } from 'ant:shell'):
    - $`command`          - Execute shell commands with tagged template literals
    - result.text()       - Get stdout as string
    - result.lines()      - Get stdout as array of lines
    - result.exitCode     - Command exit code
    - result.stdout       - Raw stdout
    - result.stderr       - Raw stderr

  Child Process (import from 'child_process'):
    - spawn()             - Spawn child process with async streaming
    - exec()              - Execute command and buffer output
    - execSync()          - Execute command synchronously
    - spawnSync()         - Spawn synchronous child process
    - fork()              - Fork module as child process
    - ChildProcess events - on('exit'), on('close'), on('error'), on('data')
    - kill()              - Send signal to child process

  Readline (import from 'readline'):
    - createInterface()   - Create readline interface
    - question()          - Prompt for user input with promise support
    - on('line')          - Handle line input events
    - on('close')         - Handle interface close
    - pause()/resume()    - Control input stream
    - close()             - Close interface
    - History support     - Command history with navigation

  OS (import from 'os'):
    - arch()              - CPU architecture (x64, arm64, etc.)
    - platform()          - Operating system platform
    - type()              - Operating system name
    - release()           - OS release version
    - version()           - OS version string
    - hostname()          - System hostname
    - homedir()           - User home directory
    - tmpdir()            - Temporary directory path
    - cpus()              - CPU core information
    - totalmem()          - Total system memory
    - freemem()           - Free system memory
    - uptime()            - System uptime in seconds
    - networkInterfaces() - Network interface information
    - userInfo()          - Current user information
    - constants           - OS signals and errno constants
    - EOL                 - Platform-specific end-of-line marker

  Navigator
    - navigator.userAgent     - User agent string
    - navigator.platform      - Platform string
    - navigator.hardwareConcurrency - CPU thread count
    - navigator.locks         - Web Locks API for coordination

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
    - cwd                 - Current working directory
    - argv                - Command line arguments
    - pid                 - Process ID
    - exit()              - Exit process with code
    - cpuUsage()          - Get CPU usage statistics

  Performance:
    - performance.now()       - High-resolution timestamp
    - performance.timeOrigin  - Time origin for measurements

  Ant Global
    - Ant.version         - Runtime version
    - Ant.target          - Build target
    - Ant.revision        - Git revision
    - Ant.buildDate       - Build date
    - Ant.serve()         - Start HTTP server
    - Ant.signal()        - Register signal handlers
    - Ant.sleep()         - Sleep in seconds
    - Ant.msleep()        - Sleep in milliseconds
    - Ant.usleep()        - Sleep in microseconds
    - Ant.gc()            - Trigger garbage collection
    - Ant.alloc()         - Get memory allocation info
    - Ant.stats()         - Get runtime statistics
    - Ant.typeof()        - Get internal type name

  JSON:
    - JSON.parse()        - Parse JSON string
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
    - TextEncoder         - Encode strings to UTF-8 bytes
    - TextDecoder         - Decode UTF-8 bytes to strings

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

  URL:
    - URL()                 - Parse and manipulate URLs
    - URLSearchParams       - Work with query strings

  Symbol:
    - Symbol()              - Create unique symbol
    - Symbol.iterator       - Well-known iterator symbol
    - Symbol.toStringTag    - Well-known toStringTag symbol

  Proxy & Reflect:
    - Proxy                 - Create proxy objects with custom behavior
    - Reflect               - Object reflection methods

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
    - import 'node:*'       - Node.js-style module aliases
    - import from URL       - Import modules from HTTP/HTTPS URLs
    - import '.json'        - Import JSON files as modules
    - import '.txt'         - Import text files as strings

TYPESCRIPT:
  Built-in TypeScript type stripping via oxc (no type checking, strip only):
    - Run .ts files directly: ./build/ant script.ts
    - Type annotations are stripped at parse time
    - Full type definitions available in src/types/

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
  - Object.defineProperties() for batch property definition
  - Object.freeze(), Object.seal(), Object.preventExtensions()
  - Object.isFrozen(), Object.isSealed(), Object.isExtensible()
  - Function.prototype.call/apply/bind
  - Array methods (map, filter, reduce, forEach, sort with comparator, etc.)
  - String methods (split, replace, replaceAll, includes, startsWith, etc.)
  - Math object with all standard functions
  - arguments object with callee support
  - Labeled statements and labeled loops (break/continue)

  ES6+ Extensions:
  - Async/await and Promises (Promise.all, Promise.race, Promise.any, Promise.resolve, etc.)
  - Arrow functions, classes, template literals, destructuring
  - Private fields and methods in classes (#privateField)
  - Spread operator and rest parameters
  - Optional chaining (?.) and nullish coalescing (??)
  - Logical assignment operators (??=, &&=, ||=)
  - for...of loops with iterables
  - let/const block scoping
  - BigInt support with arithmetic operations
  - Number literals (binary 0b1010, octal 0o755, hex 0xFF)
  - Numeric separators (1_000_000)
  - Built-in collections: Map, Set, WeakMap, WeakSet
  - Getter/setter methods in class expressions (get/set)
  - Symbol with well-known symbols (iterator, toStringTag)
  - Block-level function declaration hoisting
  - Default parameters in functions
  - Array and object destructuring with defaults
  - Computed property names in object literals
  - Shebang support (#!/usr/bin/env ant)

CONCURRENCY:
  - Minicoro-based coroutines for async/await
  - Event loop with microtask queue
  - Atomic operations for lock-free concurrent programming
  - SharedArrayBuffer for shared memory between workers
  - Atomics.wait/notify for thread synchronization
  - Virtual memory allocation for coroutine stacks

SYSTEM:
  - Signal handlers (SIGINT, SIGTERM, etc.) via Ant.signal()
  - Mark-copy compacting garbage collector + Boehm-Demers-Weiser
  - Coroutine execution tracking for proper GC
  - Forward reference tracking for memory compaction
  - Internal slots for efficient object metadata storage
  - Property lookup caching with interned strings
  - Dynamic memory growth with configurable limits
  - Non-configurable properties support (Object.defineProperty)
  - Native library integration via FFI
  - libuv-based async I/O for files and networking
  - TLS support via mbedtls or tlsuv
  - LTO (Link Time Optimization) build support
  - Gzip compression support for HTTP responses

LICENSE:
  MIT License - See LICENSE.txt for details

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
  - Ant.serve()    - HTTP server
  - import()       - ESM module loading
  - Timers         - setTimeout, setInterval, queueMicrotask
  - fetch()        - HTTP client
  - crypto.*       - Cryptography
  - JSON.*         - JSON parsing
  - console.*      - Logging

FEATURES:
  - Async/await and Promises
  - ES6+ syntax (arrow functions, classes, template literals)
  - Signal handlers (SIGINT, SIGTERM, etc.)
  - Embedded garbage collector
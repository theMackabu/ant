# 🐜 Ant

_An ant carries 50× its weight. So does this one._

Ant is a lightweight, high-performance JavaScript runtime built from scratch. <br>
Built to carry more than it weighs while delivering near-V8 speeds.

```bash
$ ls -lh ant
-rwxr-xr-x⠀8.7M⠀ant*

# built with -Os
-rwxr-xr-x⠀5.9M⠀ant*
```

## Table of contents

- [Why Ant?](#why-ant)
- [Installation](#installation)
- [Benchmarks](#benchmarks)
- [Spec conformance](#spec-conformance)
- [Building Ant](#building-ant)
- [Security](#security)
- [Community](#community)
- [Contributing to Ant](#contributing-to-ant)

## Why Ant?

|                     | Ant        | Node    | Bun    | Deno   |
| ------------------- | ---------- | ------- | ------ | ------ |
| Binary size         | **~9 MB**  | ~120 MB | ~60 MB | ~90 MB |
| Cold start          | **~5 ms**  | ~31 ms  | ~13 ms | ~25 ms |
| Engine              | Ant Silver | V8      | JSC    | V8     |
| JIT                 | ✓          | ✓       | ✓      | ✓      |
| WinterTC conformant | ✓          | partial | ✓      | ✓      |

Ant is designed for environments where size and startup time matter: serverless functions, edge computing, embedded systems, CLI tools, and anywhere you'd want JavaScript but can't afford a 50MB+ runtime.

The engine, Ant Silver is hand-built, not a wrapper around V8, JSC, or SpiderMonkey. The JIT compiler uses a fork of [MIR](https://github.com/themackabu/mir), a lightweight backend that enables near compiled performance.

## Installation

```bash
curl -fsSL https://ant.themackabu.com/install | bash
```

## Spec conformance

Ant targets the [WinterTC Minimum Common API](https://min-common-api.proposal.wintertc.org/) specification, the standard for server-side JavaScript interoperability developed by Ecma TC55.

| Suite        | Pass rate | Notes                                      |
| ------------ | --------- | ------------------------------------------ |
| compat-table | **100%**  | 1511/1511 (ES1–ES5, ES6, ES2016+, ESNext)  |
| test262      | ~64%      | Improving; focus is on real-world coverage |

## Benchmarks

### Cold start

Measures the time to import [Hono](https://hono.dev), register routes, and exit. Each runtime loads the same `bench-coldstart.js` script from `examples/npm/hono/` that creates a Hono app with two routes, prints "ready", and calls `process.exit(0)`. No HTTP server is actually started, this isolates module resolution and initialization overhead.

Measured with hyperfine (10 warmup runs, 100 timed runs):

```bash
hyperfine --warmup 10 --runs 100 \
  'ant  examples/npm/hono/bench-coldstart.js' \
  'node examples/npm/hono/bench-coldstart.js' \
  'bun  examples/npm/hono/bench-coldstart.js' \
  'deno run --allow-read --allow-env examples/npm/hono/bench-coldstart.js'
```

| Runtime | Mean       | Min     | Max      | Relative     |
| ------- | ---------- | ------- | -------- | ------------ |
| **Ant** | **5.4 ms** | 5.0 ms  | 7.1 ms   | **1.00**     |
| Bun     | 12.8 ms    | 11.6 ms | 16.4 ms  | 2.37× slower |
| Deno    | 24.8 ms    | 22.2 ms | 29.4 ms  | 4.59× slower |
| Node    | 31.1 ms    | 27.1 ms | 151.7 ms | 5.76× slower |

<details>
<summary>Environment</summary>

| Detail   | Value                             |
| -------- | --------------------------------- |
| Hardware | Apple M4 Pro, 24 GB RAM, 14 cores |
| OS       | macOS 15.7.5 (arm64)              |
| Ant      | 0.11.0                            |
| Node     | 25.9.0                            |
| Bun      | 1.3.13                            |
| Deno     | 2.7.12                            |

</details>

## Building Ant

See [BUILDING.md](BUILDING.md) for instructions on how to build Ant from source and a list of supported platforms.

## Security

For information on reporting security vulnerabilities in Ant, see [SECURITY.md](SECURITY.md).

## Community

- [Discord](http://discord.gg/CH7YSjWGzY)
- [Blog: Working was the beginning](https://themackabu.dev/blog/ant-part-two)
- [DeepWiki: Ant internals](https://deepwiki.com/theMackabu/ant)

## Contributing to Ant

We welcome contributions through pull request. See [CONTRIBUTING.md](CONTRIBUTING.md) for more details. <br>
For information about the governance of Ant, see [GOVERNANCE.md](GOVERNANCE.md).

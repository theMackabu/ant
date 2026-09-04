# 🐜 Ant

_An ant carries 50× its weight. So does this one._

Ant is a lightweight, high-performance JavaScript runtime built from scratch. <br>
Built to carry more than it weighs without compromising performance.

```bash
$ ls -lh ant
-rwxr-xr-x⠀9.0M⠀ant*

# built with -Os
-rwxr-xr-x⠀4.7M⠀ant*
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
| Binary size         | **9 MB**   | ~140 MB | ~61 MB | ~77 MB |
| Cold start          | **~4 ms**  | ~30 ms  | ~10 ms | ~18 ms |
| Engine              | Ant Silver | V8      | JSC    | V8     |
| JIT                 | ✓          | ✓       | ✓      | ✓      |
| WinterTC conformant | ✓          | partial | ✓      | ✓      |

Ant is designed for environments where size and startup time matter: serverless functions, edge computing, embedded systems, CLI tools, and anywhere you'd want JavaScript but can't afford a 50MB+ runtime.

Ant Silver is an independent engine, not a wrapper around V8, JSC, or SpiderMonkey. The JIT compiler uses a fork of [MIR](https://github.com/themackabu/mir), a lightweight backend that enables near-native performance.

## Installation

```bash
curl -fsSL https://antjs.org/install | bash
```

## Spec conformance

Ant targets the [WinterTC Minimum Common API](https://min-common-api.proposal.wintertc.org/) specification, the standard for server-side JavaScript interoperability developed by Ecma TC55.

| Suite        | Pass rate | Notes                                     |
| ------------ | --------- | ----------------------------------------- |
| compat-table | **100%**  | 1511/1511 (ES1–ES5, ES6, ES2016+, ESNext) |
| Temporal     | **100%**  | 4603/4603 at revision 2026-08-10          |
| test262      | ~65%      | 34758/53578 at revision 2026-08-10        |

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

| Runtime | Mean       | Min     | Max     | Relative     |
| ------- | ---------- | ------- | ------- | ------------ |
| **Ant** | **3.9 ms** | 2.1 ms  | 5.3 ms  | **1.00**     |
| Bun     | 8.8 ms     | 6.7 ms  | 10.7 ms | 2.26× slower |
| Deno    | 18.6 ms    | 16.2 ms | 20.8ms  | 4.74× slower |
| Node    | 29.4 ms    | 27.3 ms | 32.6 ms | 7.50× slower |

<details>
<summary>Environment</summary>

| Detail   | Value                             |
| -------- | --------------------------------- |
| Hardware | Apple M5 Pro, 64 GB RAM, 18 cores |
| OS       | macOS 27.0 Golden Gate (26A5406e) |
| Ant      | 14.1.aa53d9d1.0                   |
| Node     | 26.5.1                            |
| Bun      | 1.4.0                             |
| Deno     | 2.9.5                             |

</details>

## Building Ant

See [BUILDING.md](BUILDING.md) for instructions on how to build Ant from source and a list of supported platforms.

### Docker

You can build the container from source. It contains the statically linked musl Ant binary, CA certificates, and time-zone data.

```bash
docker build -t ant .
docker run --rm ant --version
docker run --rm -v "$PWD:/app" ant index.js
```

## Security

For information on reporting security vulnerabilities in Ant, see [SECURITY.md](SECURITY.md).

## Community

- [Discord](http://discord.gg/CH7YSjWGzY)
- [Blog: Working was the beginning](https://themackabu.dev/blog/ant-part-two)
- [DeepWiki: Ant internals](https://deepwiki.com/theMackabu/ant)

## Contributing to Ant

We welcome contributions through pull request. See [CONTRIBUTING.md](CONTRIBUTING.md) for more details. <br>
For information about the governance of Ant, see [GOVERNANCE.md](GOVERNANCE.md).

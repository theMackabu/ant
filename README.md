# 🐜 Ant

**A 8MB JavaScript runtime with 1ms cold starts.**

Ant is a lightweight, high-performance JavaScript runtime built from scratch. <br>
Fits in your pocket while Delivering near-V8 speeds in a binary smaller than most npm packages.

```
$ ls -lh ant
-rwxr-xr-x⠀8.1M⠀ant*
```

## Table of contents

- [Why Ant?](#why-ant)
- [Installation](#installation)
- [Spec conformance](#spec-conformance)
- [Building Ant](#building-ant)
- [Security](#security)
- [Community](#community)
- [Contributing to Ant](#contributing-to-ant)

## Why Ant?

|                     | Ant         | Node      | Bun      | Deno      |
| ------------------- | ----------- | --------- | -------- | --------- |
| Binary size         | **~8 MB**   | ~120 MB   | ~60 MB   | ~90 MB    |
| Cold start          | **~3-5 ms** | ~30-50 ms | ~5-10 ms | ~20-30 ms |
| Engine              | Ant Silver  | V8        | JSC      | V8        |
| JIT                 | ✓           | ✓         | ✓        | ✓         |
| WinterTC conformant | ✓           | partial   | ✓        | ✓         |

Ant is designed for environments where size and startup time matter: serverless functions, edge computing, embedded systems, CLI tools, and anywhere you'd want JavaScript but can't afford a 50MB+ runtime.

The engine, Ant Silver is hand-built, not a wrapper around V8, JSC, or SpiderMonkey. The JIT compiler uses [MIR](https://github.com/vnmakarov/mir), a lightweight JIT backend that enables compiled performance in a fraction of the binary size of traditional JIT engines.

## Installation

```bash
curl -fsSL https://ant.themackabu.com/install | bash

# or with MbedTLS
curl -fsSL https://ant.themackabu.com/install | MBEDTLS=1 bash
```

## Spec conformance

Ant targets the [WinterTC Minimum Common API](https://min-common-api.proposal.wintertc.org/) specification, the standard for server-side JavaScript interoperability developed by Ecma TC55.

| Suite            | Pass rate | Notes                                      |
| ---------------- | --------- | ------------------------------------------ |
| js-zoo (ES1–ES5) | ~100%     |                                            |
| js-zoo (ES6)     | ~80%\*    | \*Generators unsupported                   |
| js-zoo (ES2016+) | ~90%      |                                            |
| test262          | ~50%      | Improving, focus is on real-world coverage |

## Building Ant

See [BUILDING.md](BUILDING.md) for instructions on how to build Ant from source and a list of supported platforms.

## Security

For information on reporting security vulnerabilities in Ant, see [SECURITY.md](SECURITY.md).

## Community

- [Discord](http://discord.gg/CH7YSjWGzY)
- [Blog: Building Ant](https://s.tail.so/js-in-one-month)
- [DeepWiki: Ant internals](https://deepwiki.com/theMackabu/ant)

## Contributing to Ant

We welcome contributions through pull request. See [CONTRIBUTING.md](CONTRIBUTING.md) for more details. <br>
For information about the governance of Ant, see [GOVERNANCE.md](GOVERNANCE.md).

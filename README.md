# 🐜 Ant

**Ant-sized JavaScript Runtime**

A JavaScript runtime that fits in your pocket. <br>
Full async/await, modules, HTTP servers, crypto, and more.

📖 [Read the blog post about Ant](https://s.tail.so/js-in-one-month)

## Installation

```bash
curl -fsSL https://ant.themackabu.com/install | bash

# or with MbedTLS (darwin only)
curl -fsSL https://ant.themackabu.com/install | MBEDTLS=1 bash
```

## Building from Source

```bash
git clone https://github.com/theMackabu/ant.git && cd ant

meson subprojects download
meson setup build
meson compile -C build
```

## Security

For information on reporting security vulnerabilities in Ant, see [SECURITY.md](SECURITY.md).

## Contributing to Ant

We welcome contributions through pull request. See [CONTRIBUTING.md](CONTRIBUTING.md) for more details. <br>
For more information about the internals, read the [ant deepwiki](https://deepwiki.com/theMackabu/ant).

## Current project team members

For information about the governance of Ant, see [GOVERNANCE.md](GOVERNANCE.md).

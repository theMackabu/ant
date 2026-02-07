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

## Build from source

```bash
meson subprojects download
meson setup build
meson compile -C build
```

For more information about the internals, read the [ant deepwiki](https://deepwiki.com/theMackabu/ant).

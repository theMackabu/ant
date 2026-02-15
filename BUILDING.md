# Building Ant

Depending on what platform or features you need, the build process may
differ. After you've built a binary, running the
test suite to confirm that the binary works as intended is a good next step.

If you can reproduce a test failure, search for it in the <br>
[Ant issue tracker](https://github.com/theMackabu/ant/issues) or file a new issue.

## Table of contents

- [Supported platforms](#supported-platforms)
  - [Platform list](#platform-list)
  - [Supported toolchains](#supported-toolchains)
  - [Official binary platforms and toolchains](#official-binary-platforms-and-toolchains)
- [Building Ant on supported platforms](#building-ant-on-supported-platforms)
  - [Prerequisites](#prerequisites)
  - [Unix and macOS](#unix-and-macos)
    - [Unix prerequisites](#unix-prerequisites)
    - [macOS prerequisites](#macos-prerequisites)
    - [Building Ant](#building-ant-1)
    - [Installing Ant](#installing-ant)
    - [Running tests](#running-tests)
    - [Building a debug build](#building-a-debug-build)
    - [Building an ASan build](#building-an-asan-build)
    - [Speeding up frequent rebuilds when developing](#speeding-up-frequent-rebuilds-when-developing)
    - [Troubleshooting Unix and macOS builds](#troubleshooting-unix-and-macos-builds)
  - [Windows](#windows)
    - [Windows prerequisites](#windows-prerequisites)
    - [Building Ant](#building-ant-2)
- [Meson build options](#meson-build-options)
- [TLS library selection](#tls-library-selection)

## Supported platforms

### Platform list

Ant builds and runs on the following platforms. Official CI builds are
produced for each platform listed below.

| Operating System | Architectures | Variant    | Static | Notes                        |
| ---------------- | ------------- | ---------- | ------ | ---------------------------- |
| GNU/Linux        | x64           | glibc      | No     | Ubuntu 22.04 (CI)            |
| GNU/Linux        | aarch64       | glibc      | No     | Ubuntu 22.04 (CI)            |
| GNU/Linux        | x64           | musl       | Yes    | Alpine Edge (CI)             |
| GNU/Linux        | aarch64       | musl       | Yes    | Alpine Edge (CI)             |
| macOS            | x64           | openssl    | No     | macOS 15 (CI)                |
| macOS            | aarch64       | openssl    | No     | macOS 15 (CI)                |
| macOS            | x64           | mbedtls    | No     | macOS 15 (CI)                |
| macOS            | aarch64       | mbedtls    | No     | macOS 15 (CI)                |
| Windows          | x64           | mingw/msys | No     | MSYS2 MINGW64 toolchain (CI) |

### Supported toolchains

Ant is built with the GNU C23 standard (`-std=gnu23`). A compiler with
C23 support is required.

| Operating System | Compiler Versions                     |
| ---------------- | ------------------------------------- |
| Linux            | GCC >= 14 or Clang >= 18              |
| macOS            | Xcode CLT (Apple Clang) or LLVM >= 18 |
| Windows          | MinGW-w64 GCC via MSYS2 (MINGW64)     |

### Official binary platforms and toolchains

CI binaries are produced using:

| Binary package             | Platform and Toolchain                       |
| -------------------------- | -------------------------------------------- |
| ant-linux-x64              | Ubuntu 22.04 (glibc), LLVM/Clang             |
| ant-linux-aarch64          | Ubuntu 22.04 (glibc), LLVM/Clang             |
| ant-linux-x64-musl         | Alpine Edge (musl), statically linked, Clang |
| ant-linux-aarch64-musl     | Alpine Edge (musl), statically linked, Clang |
| ant-darwin-x64             | macOS 15 Intel, LLVM/Clang                   |
| ant-darwin-aarch64         | macOS 15 ARM, LLVM/Clang                     |
| ant-darwin-x64-mbedtls     | macOS 15 Intel, LLVM/Clang, mbedTLS          |
| ant-darwin-aarch64-mbedtls | macOS 15 ARM, LLVM/Clang, mbedTLS            |
| ant-windows-x64            | MSYS2 MINGW64 toolchain                      |

## Building Ant on supported platforms

### Prerequisites

The following tools are required to build Ant regardless of platform:

- **C compiler** with C23 support (GCC >= 14 or Clang >= 18)
- **[Meson](https://mesonbuild.com/)** build system (and Ninja backend)
- **[CMake](https://cmake.org/)** (for the tlsuv subproject)
- **pkg-config**
- **Node.js** >= 22 (used to generate the JS snapshot at build time)
- **[Rust](https://rustup.rs/)** toolchain (stable) with `cargo` (builds the OXC type-strip library)
- **[Zig](https://ziglang.org/)** >= 0.15 (builds the package manager component)
- **Git**

System libraries required:

- **OpenSSL** (default) or **mbedTLS** (alternative TLS backend)
- **libsodium**
- **libuuid** (Linux/macOS)
- **llhttp** (if not building from source via cmake)

The remaining dependencies are vendored as Meson subprojects under `vendor/`
and are fetched automatically:

- libuv 1.52.0
- yyjson 0.12.0
- zlib-ng 2.3.3
- nghttp2
- pcre2
- libffi
- lmdb (OpenLDAP LMDB 0.9.33)
- minicoro
- argtable3
- uthash
- uuidv7
- tlsuv (cmake subproject)

### Unix and macOS

#### Unix prerequisites

Installation via package manager:

- Ubuntu/Debian:

  ```bash
  sudo apt-get install python3 python3-pip gcc-14 g++-14 ninja-build cmake \
    pkg-config uuid-dev libssl-dev libsodium-dev nodejs npm
  pip3 install meson
  ```

- Fedora:

  ```bash
  sudo dnf install python3 gcc gcc-c++ ninja-build cmake pkgconf \
    libuuid-devel openssl-devel libsodium-devel nodejs npm
  pip3 install meson
  ```

- Alpine (musl):
  ```sh
  apk add clang lld llvm meson ninja cmake pkgconf nodejs npm \
    musl-dev openssl-dev openssl-libs-static libsodium-dev libsodium-static \
    util-linux-dev util-linux-static linux-headers libunwind-dev libunwind-static
  ```

You will also need Rust and Zig installed. The recommended approach:

```bash
# Rust (via rustup)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Zig (download from https://ziglang.org/download/)
# Or via package manager if available
```

#### macOS prerequisites

- Xcode Command Line Tools (provides Apple Clang):

  ```bash
  xcode-select --install
  ```

- Install remaining tools via [Homebrew](https://brew.sh):

  ```bash
  brew install meson ninja llvm openssl@3 libsodium node
  ```

- Rust and Zig:

  ```bash
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
  brew install zig
  ```

#### Building Ant

> [!IMPORTANT]
> If the path to your build directory contains a space, the build will likely
> fail.

To build Ant:

```bash
meson subprojects download
meson setup build
meson compile -C build
```

Alternatively, if you have [maid](https://github.com/exact-labs/maid) installed, <br>
you can use the task runner:

```bash
maid setup       # downloads subprojects + configures with ccache and lld
maid build       # compiles
maid run <file>  # builds and runs a JS file
```

> [!TIP]
> `maid setup` automatically configures ccache and lld for faster builds.
> Use `maid run <file>` during development to build and execute in one step.

To verify the build:

```bash
./build/ant --version
./build/ant -e "console.log('Hello from Ant ' + Ant.version)"
```

#### Installing Ant

You can install the built binary using:

```bash
maid install
```

This copies the binary to the directory of an existing `ant` installation, or
falls back to `~/.ant/bin/`. It also creates an `antx` symlink.

Alternatively, copy the binary manually:

```bash
cp ./build/ant /usr/local/bin/ant
```

#### Running tests

To run a single test:

```bash
./build/ant tests/test_async.cjs
```

To run the spec suite:

```bash
./build/ant examples/spec/run.js
```

> [!NOTE]
> Remember to recompile with `meson compile -C build` (or `maid build`)
> between test runs if you change code in the `src/` directory.

#### Building a debug build

A debug build disables optimizations and LTO, and preserves debug symbols:

```bash
meson subprojects download
CC="ccache $(which clang)" \
  meson setup build --wipe --buildtype=debug \
  -Doptimization=0 -Db_lto=false -Dstrip=false -Db_lundef=false -Dunity=off
meson compile -C build
```

Or with maid:

```bash
maid debug
maid build
```

When using the debug build, core dumps will be generated in case of crashes.
Use `lldb` or `gdb` with the debug binary to inspect them:

```bash
lldb ./build/ant core.ant
(lldb) bt
```

#### Building an ASan build

[ASan](https://github.com/google/sanitizers) can help detect memory bugs.

> [!WARNING]
> ASan builds are significantly slower than release builds. The debug flags
> are not required but can produce clearer stack traces when ASan detects
> an issue.

```bash
meson subprojects download
CC="ccache $(which clang)" \
  meson setup build --wipe \
  -Db_sanitize=address -Doptimization=0 -Db_lto=false -Dstrip=false -Db_lundef=false
meson compile -C build
```

Or with maid:

```bash
maid asan
maid build
```

Then run tests against the ASan build:

```bash
./build/ant tests/test_gc.js
```

#### Speeding up frequent rebuilds when developing

If you plan to frequently rebuild Ant, installing `ccache` can greatly
reduce build times. The `maid setup` task configures ccache automatically.

> [!TIP]
> Using both `ccache` and `lld` together provides the best rebuild
> performance. `ccache` caches compilation, while `lld` speeds up linking
> (which cannot be cached).

On GNU/Linux:

```bash
sudo apt install ccache
export CC="ccache gcc"    # add to your .profile
```

On macOS:

```bash
brew install ccache
export CC="ccache cc"     # add to ~/.zshrc
```

Using `lld` as the linker also speeds up link times:

```bash
export CC_LD="$(which ld64.lld)"  # macOS with brew llvm
# or
export CC_LD="$(which lld)"       # Linux
```

> [!NOTE]
> LTO is enabled by default with 8 threads (`b_lto=true`,
> `b_lto_threads=8`). Disable it with `-Db_lto=false` for faster iteration
> during development.

#### Troubleshooting Unix and macOS builds

Stale builds can sometimes result in errors. Clean the build directory and
reconfigure:

```bash
rm -rf build
meson setup build
meson compile -C build
```

If you encounter "file not found" errors for vendored dependencies:

```bash
meson subprojects download
```

If the build runs out of memory, reduce parallelism:

```bash
meson compile -C build -j2
```

### Windows

#### Windows prerequisites

Ant on Windows is built using the MSYS2 MINGW64 toolchain.

> [!IMPORTANT]
> Native MSVC builds are not currently supported. You must use the MSYS2
> MINGW64 environment.

1. Install [MSYS2](https://www.msys2.org/)
2. Open the **MINGW64** shell and install dependencies:
   ```bash
   pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-meson \
     mingw-w64-x86_64-ninja mingw-w64-x86_64-cmake \
     mingw-w64-x86_64-openssl mingw-w64-x86_64-libsodium \
     mingw-w64-x86_64-lld mingw-w64-x86_64-nodejs git
   ```
3. Install Rust via [rustup](https://rustup.rs/) (select the
   `x86_64-pc-windows-gnu` target)
4. Install [Zig](https://ziglang.org/download/)

#### Building Ant

From the MSYS2 MINGW64 shell:

```bash
git clone https://github.com/theMackabu/ant.git
cd ant
meson subprojects download
meson setup build -Dc_std=gnu2x
meson compile -C build
```

> [!NOTE]
> Windows builds use `-Dc_std=gnu2x` instead of `gnu23` due to MinGW
> toolchain compatibility.

To verify:

```bash
./build/ant.exe --version
```

> [!WARNING]
> Windows builds require bundling the following DLLs alongside `ant.exe`:
>
> - `libssl-3-x64.dll`
> - `libcrypto-3-x64.dll`
> - `libsodium-26.dll`
>
> These are found in the MSYS2 MINGW64 bin directory
> (`/mingw64/bin/` or equivalent).

## Meson build options

Configure options are set via `meson setup` or `meson configure`:

| Option              | Type    | Default   | Description                                |
| ------------------- | ------- | --------- | ------------------------------------------ |
| `static_link`       | boolean | `false`   | Statically link the final binary           |
| `build_timestamp`   | string  | (auto)    | Build timestamp (defaults to current time) |
| `tls_library`       | combo   | `openssl` | TLS backend: `openssl` or `mbedtls`        |
| `deps_prefix_cmake` | string  | (empty)   | Prefix path for cmake dependency lookup    |

Standard Meson built-in options used by Ant:

| Option          | Default   | Description                       |
| --------------- | --------- | --------------------------------- |
| `buildtype`     | `release` | Build type (release, debug, etc.) |
| `optimization`  | `3`       | Optimization level (0-3)          |
| `c_std`         | `gnu23`   | C language standard               |
| `b_lto`         | `true`    | Link-time optimization            |
| `b_lto_threads` | `8`       | LTO parallelism                   |
| `strip`         | `true`    | Strip debug symbols from binary   |
| `b_sanitize`    | `none`    | Sanitizer (e.g. `address`)        |

Example:

```bash
meson setup build -Dtls_library=mbedtls -Dstatic_link=true --prefer-static
```

## TLS library selection

Ant supports two TLS backends:

- **OpenSSL** (default): Widely available, used on all platforms by default.
- **mbedTLS**: Lighter alternative, useful for embedded or constrained
  environments. Currently only tested on macOS CI.

To build with mbedTLS:

```bash
meson setup build -Dtls_library=mbedtls
```

When using mbedTLS, the target triple in the version string will include
`-mbedtls` as a suffix.

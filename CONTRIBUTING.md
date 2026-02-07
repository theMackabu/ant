# Contributing to Ant

Thank you for your interest in contributing to Ant! This document provides guidelines for contributing.

## Getting Started

### Prerequisites

- C compiler with C23 support (GCC 14+ or Clang 18+)
- Meson build system
- Git
- OpenSSL
- libSodium

### Building from Source

```bash
git clone https://github.com/theMackabu/ant.git && cd ant

meson subprojects download
meson setup build
meson compile -C build
```

## How to Contribute

### Reporting Bugs

1. Check existing issues first
2. Include reproduction steps
3. Provide system info (OS, compiler version)
4. Include relevant error messages

### Suggesting Features

1. Open an issue with the `enhancement` label
2. Describe the use case
3. Provide examples if possible

### Pull Requests

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-feature`)
3. Make your changes
4. Ensure tests pass
5. Submit a pull request

## Code Style

- **C code**: GNU23 standard, 2-space indent, no trailing whitespace
- **Naming**: `snake_case` for functions, `UPPERCASE` for macros
- **Headers**: Local includes (`"..."`) before system includes (`<...>`)
- **Comments**: Avoid unless code is complex

## Project Structure

```
src/
├── cli/        # Command line interface helpers
├── core/       # Bundled snapshot code
├── modules/    # Built-in JS modules (fs, path, shell, etc.)
├── esm/        # ES module system
├── pkg/        # Zig-based package manager
include/        # C header files
tests/          # JavaScript test files
vendor/         # External dependencies
```

## Testing

- Add tests for new features in `tests/`
- Run specific tests: `./build/ant tests/test_<name>.js`
- Run `./build/ant examples/spec/run.js` to ensure nothing else broke

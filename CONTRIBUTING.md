# Contributing to Ant

Thank you for your interest in contributing to Ant! This document provides guidelines for contributing.

## Getting Started

For the full list of prerequisites and platform-specific setup instructions, <br>
see [BUILDING.md](BUILDING.md#prerequisites).

### Building from Source

```bash
git clone https://github.com/theMackabu/ant.git && cd ant

meson subprojects download
meson setup build
meson compile -C build
```

For detailed build instructions including debug builds, ASan builds, <br>
ccache setup, and Windows/Linux/macOS specifics, see [BUILDING.md](BUILDING.md).

## Repository Knowledge

Ant keeps agent-facing repository context in versioned markdown so future
changes do not depend on chat history or tribal knowledge.

- Start with [AGENTS.md](AGENTS.md) for the quick repository map
- Use [ARCHITECTURE.md](ARCHITECTURE.md) for subsystem boundaries
- Use [docs/repo/index.md](docs/repo/index.md) for durable workflow docs
- Use [docs/exec-plans/index.md](docs/exec-plans/index.md) for multi-step work
- Run `maid knowledge` after updating these docs
- Run `maid structure` to catch generated-path and scratch-doc violations
- Run `maid validate_changes` to see the recommended validation set for the current diff

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
├── esm/        # ES module system (loader, resolver, cache)
├── highlight/  # Syntax highlighting (emit, iterators)
├── modules/    # Built-in JS modules (fs, path, shell, etc.)
├── pkg/        # Zig-based package manager
├── silver/     # Silver bytecode compiler and VM
│   └── ops/    # Bytecode operation definitions
├── strip/      # Rust-based type stripping (oxc)
├── tools/      # Code generation scripts (snapshot gen)
├── types/      # TypeScript type declarations
include/        # C header files
tests/          # JavaScript test files
vendor/         # External dependencies
```

For more information about Ant's internal structure, read the [Ant DeepWiki](https://deepwiki.com/theMackabu/ant).

## Testing

- Add tests for new features in `tests/`
- Run specific tests: `./build/ant tests/test_<name>.js`
- Run `./build/ant examples/spec/run.js` to ensure nothing else broke

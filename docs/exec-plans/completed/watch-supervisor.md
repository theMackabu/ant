# Watch Supervisor

Status: completed
Last reviewed: 2026-05-15
Owner: theMackabu

## Goal

Make `ant --watch` a dependable process supervisor before any future hot-reload
work. The watch mode should restart the child for entry-file or local import
changes without recursively re-entering watch mode.

## Scope

- Keep `--watch` process based.
- Strip `-w`, `--watch`, and `--no-clear-screen` only from Ant's own argv
  segment before spawning the child.
- Watch the local static import/export/require graph that resolves through
  Ant's module resolver.
- Debounce filesystem events and make child termination idempotent.

## Decisions

- Reuse the existing ESM path resolver through a small exported helper instead
  of duplicating extension, TypeScript fallback, and package resolution logic.
- Only local relative or absolute specifiers are added to the watch graph.
  Builtins, URLs, and bare package specifiers remain out of scope for this
  supervisor pass.
- Rebuild the graph before each child spawn so edits to imports are picked up on
  the following restart.

## Validation

- Added `tests/test_watch_supervisor.cjs` to cover `-w`, script args after `--`,
  and restart on a local dependency edit.

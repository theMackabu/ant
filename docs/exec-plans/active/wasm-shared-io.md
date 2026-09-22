# Shared I/O Formatting in Wasm

Status: active
Last reviewed: 2026-09-21
Owner: theMackabu

## Goal

Build the Wasm reactor with src/modules/io.c so native and embedded error
formatting have one implementation. Resolve the existing missing
io_print_error_stack/header/props link symbols with explicit portability handling.

## Design

- Compile io.c in the Wasm Meson target; remove the remaining io_no_color stub.
  The temporary duplicate printers are absent from the current source tree.
- Guard libuv and inspector integration with ANT_WASM_EMBED. Console timing uses
  ant_wasm_now_ms on Wasm and uv_hrtime on native builds. Wasm retains plain output.
- Extend the exact import contract with WASI Preview 1 fd_fdstat_get. Descriptors
  1 and 2 are non-terminal, write-only output sinks; unknown descriptors return
  BADF, and invalid memory ranges return FAULT. Initialize the complete 24-byte
  fdstat structure, including padding and rights fields.
- Align fd_write descriptor validation with fd_fdstat_get. The loader continues
  consuming output without forwarding it to the embedding host.
- Validate descriptor layout, bounds, memory growth, and shared error formatting
  through the actual loader imports and guest unhandled rejections.

## Validation

Clean baseline: 6bdd7588. Initial maid preflight passed. Build and focused
native/package tests, exact import/export inspection, and npm package validation
are pending. Preserve native console behavior and inspector event delivery.

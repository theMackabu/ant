# Single-File Executables (`ant compile`)

Status: active (spike)
Last reviewed: 2026-07-16
Owner: spike/ant-compile
Upstream issue: theMackabu/ant#54

## Goal

Give Ant a `deno compile`-equivalent: bundle a JS/TS app plus the Ant runtime
into one self-contained executable that runs headless with no external files.
Primary driver: embedding Ant as a headless runtime (Nano) where the single
blocker is single-file output.

## Scope (this spike)

- Host-target only. No cross-compilation yet.
- Reuse the existing `ANTAPP01` archive format from `packages/desktop`
  (`packages/desktop/app/archive/archive.c`) rather than inventing a new one.
- `ant compile <entry> [-o out] [--root dir] [--runtime base]`.
- Runtime auto-detects an embedded payload and runs it.

## Design

Fused binary layout (all little-endian):

```
[ ant base runtime copy ]
[ payload: u32 entry_len + entry_rel_bytes ]
[ ANTAPP01 archive: u32 count + entries ]
[ trailer: "ANTFUSE1"(8) + u64 payload_offset + u64 payload_size ]  # 24 bytes at EOF
```

- Packaging embeds the whole app directory (root = entry's dir by default),
  so multi-file apps and relative imports work without a module-graph resolver.
- At startup `main()` seeks the 24-byte trailer at EOF (O(1), base-size
  independent). If present, the archive is extracted to a temp dir and the
  entry is injected as `argv[1]` so the normal file-run path executes it. This
  reuses all existing runtime init (`execute_module` / `js_run_event_loop`).

## Task list

- [x] `include/compile.h`, `src/compile/compile.c` (packaging + detect/extract)
- [x] `sources.json` engine pattern `src/compile/*.c`
- [x] `src/main.c`: subcommand entry + fused-payload hook + argv injection
- [x] `tests/test_compile_single_file.cjs`
- [ ] Robust macOS re-signing (see follow-ups)
- [ ] Cross-compilation (download prebuilt runtimes per target)

## Validation status

- Builds clean (`meson compile -C build`).
- `ant compile` produces a runnable binary; fused binary resolves relative
  imports and forwards args; plain `ant` is not misdetected as fused.
- `tests/test_compile_single_file.cjs` passes.

## Follow-ups / known gaps

- **macOS code signing**: appending bytes after the Mach-O linkedit
  invalidates the signature; `codesign --force --sign -` reports "failed strict
  validation". The locally-built ad-hoc binary still runs, but distribution
  needs the Deno/Bun approach (embed payload in a dedicated Mach-O segment so
  the signature stays valid), not a trailing append.
- **True VFS + module-graph** (deno compile parity): embed only the reachable
  graph instead of the whole directory.
- **Leading-dash app args**: args before the entry are parsed as ant flags;
  only args after the entry reach the app.
- **CLI error style**: uses plain `fprintf(stderr, ...)` instead of the
  `crfprintf`/messages.toml system.
- **Cross-compilation**: fetch a prebuilt runtime for the target triple and
  append the payload to that instead of the running binary.

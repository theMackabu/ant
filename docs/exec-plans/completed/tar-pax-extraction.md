# PAX Tar Extraction

Status: implemented and validated
Last reviewed: 2026-09-05
Owner: theMackabu

## Problem and decision

Installing `@mistralai/mistralai@1.15.1` overwrote a JavaScript module with its
JSON source map. The tarball uses a PAX `path` record for the long `.js.map`
filename; its fallback header has the same name as the `.js` file. The extractor
previously treated PAX headers as files and used that fallback name.

Consume per-entry PAX headers as metadata, including across input boundaries.
Apply `path`, `linkpath`, and `size` to the following entry, then clear the
pending overrides. Validate full paths before prefix stripping, retain the
existing traversal checks, and use the effective size for data and padding.
Allow paths up to the existing validation limit of 4096 bytes. Bound each PAX
metadata body to 64 KiB and reject malformed or oversized records. Unknown PAX
keys are ignored. Global PAX, GNU extension headers, and other unsupported entry
types fail explicitly rather than being extracted as ordinary files.

## Validation

- `zig test src/pkg/extractor.zig -lc -lz`: four tests passed, covering the
  JS/source-map collision, fragmented input, override lifetime, size and link
  overrides, malformed records, unsafe paths, and buffer limits.
- `meson compile -C build`: passed.
- `maid preflight` and its recommended `maid knowledge`: passed.
- Streamed the actual Mistral 1.15.1 npm tarball through `Extractor` in 137-byte
  compressed chunks. All 3260 output files matched npm's extraction byte for
  byte, with no additional files. The extracted SDK loaded in Node and Ant.

## Existing cache

This changes future extraction only. Previously corrupted installed files and
package-cache entries need removal and reinstallation; the extractor does not
rewrite cache hits. The reproduction confirmed that a cache hit can retain the
old corruption after rebuilding. No global cache was cleared during validation.

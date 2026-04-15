# Technical Debt Tracker

Status: active
Last reviewed: 2026-04-09
Owner: theMackabu

Use this file to record debt that is important enough to preserve but not yet
scheduled.

## Format

- Area:
- Issue:
- Impact:
- Proposed fix:
- Owner:
- Status:

## Open Items

- Area: `src/modules/readline.c`
  - Issue: Rendering assumes readline owns the full visible prompt line, so redraws are anchored to the logical prompt text instead of the terminal position where editing actually begins.
  - Impact: Full redraw paths can clobber externally rendered prefixes, boxed prompts, or other same-line UI written before `question()` or `prompt()` starts editing.
  - Proposed fix: Track an explicit render origin / prompt anchor, separate logical prompt text from the screen position where input begins, and make redraws preserve external prefixes and custom prompt chrome.
  - Status: open

- Area: Silver compiler
  - Issue: `sv_compiler_t` scratch storage is still allocated per compilation, so repeated compiles in a long-lived process pay allocator churn for locals, bytecode buffers, constants, atoms, upvalue descriptors, loops, srcpos data, and maybe slot-type scratch.
  - Impact: One-shot CLI compiles are fine, but a REPL, watch mode, embedder, or other long-lived process cannot yet recycle compiler scratch space across compiles.
  - Proposed fix: Add a real `compile_pool` scratch allocator after the `compile_ctx` extraction. Pool the resizable arrays for `locals`, `local_lookup_heads`, `code`, `constants`, `atoms`, `upval_descs`, `loops`, `srcpos`, and potentially `slot_types`. Keep `line_table` separate or make it poolable scratch, since it is derived from the current source buffer rather than a semantic cache.
  - Status: backlog

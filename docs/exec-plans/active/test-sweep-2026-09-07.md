# Test Sweep 2026-09-07

Status: active
Last reviewed: 2026-09-07
Owner: theMackabu

Results of running every `tests/test_*` file, the JIT harness, the spec suite,
the Node differential runner, and `maid preflight`. The first pass ran against
`13e340d5`; the recorded full-sweep numbers are from the working-tree build
at `87077b56` plus the constructor-context implementation later committed in
`87c85b9c`. Subsequent focused validation is recorded in the completed plans;
these totals are not a fresh sweep of HEAD. The v14 comparison uses the
`v14.0.ff84a70d.0` release binary from 2026-08-17. Note that v14 is only
useful for tests that existed unchanged at that tag; see the native addon row.

## Recorded totals

| Suite | Result |
| --- | --- |
| `tests/test_*` (569 files) | 563 pass, 2 fail, 3 exit non-zero by design, 1 expected timeout |
| JIT harness (`examples/jit/run.js`) | 10 of 10 files pass |
| Spec suite (`examples/spec/run.js --all`) | 4221 tests, 102 files, 0 failures |
| Differential runner (`--cases 5 --seed 44`) | 11 of 25 probes mismatch, all pre-existing |
| `maid preflight` | knowledge and structure checks pass |

## Resolved during the sweep

| Test | Cause | Fix |
| --- | --- | --- |
| `test_jit_for_of.cjs` | PR #95 broke integer-range locals across an OSR bailout resume; the nested for-of case returned 1065 instead of 2166. | Repaired in 0bcab93b. The case moved to `examples/jit/bailout_resume.js` so the harness catches it. |
| `test_websocket_client_buffered_frames.cjs` | `new.target` lived in one global slot on `ant_t`, so the WebSocket constructor's target leaked into the native accept callback. `net_socket_create` built the accepted socket with the WebSocket prototype and the connection listener got an object with no `.on`. The `module.exports` data-property change exposed the leak by removing an incidental getter call that cleared the slot. | JS/JIT frames now carry invocation state, and native callbacks receive explicit `call_new_target` through `ant_params_t`. Native constructor frames only root targets for GC; the native accept callback explicitly selects the default Socket prototype. Committed in `87c85b9c`, including `tests/test_new_target_frames.cjs`; header and internal-call follow-ups landed in `1bfe480e` and `a7b0be86`. See the [completed constructor-context plan](../completed/net-connection-websocket-arg-regression.md). |
| `test_cli_file_arg_precedes_package_script.cjs` | `ant t.js` ran the package script named `t.js` instead of the file. | `src/main.c` skips the script shortcut when the positional names an existing regular file. Committed in 87077b56. |
| `test_console_inspect_string_internals.cjs` | Stale. PR #90 raised `STR_SHORT_CONS_THRESHOLD` from 13 to 32, so the 13-char concat now copies flat by design. | Rope case uses two 16-char strings. Committed in 87077b56. |
| `test_repl_static_import.cjs` | Stale. Since 671d8071 the prompt is dim `❯`, a reset escape, then a space, so the pty driver's literal `❯ ` marker never matched and it killed the REPL at its deadline. | Driver matches the prompt with SGR escapes allowed. Committed in 87077b56. |
| `test_debug_error_trace.cjs` | Stale from birth. The `ANT_DEBUG=dump/errors:trace` channel and `[ant-debug:error]` marker never existed outside the commit that added the test. | Removed in 87077b56. |
| `test_ffi_wrappers.cjs`, `test_rpc.cjs` | Stale. ESM `import` syntax in `.cjs` files. | Renamed to `.mjs` in fe9904d6. |

## Unresolved at the sweep checkpoint, now fixed

Both were open when the totals above were recorded. Both are fixed in the
working tree as of 2026-09-07 evening; the fresh-sweep section below records
the rerun.

| Test | Cause | Fix |
| --- | --- | --- |
| `test_compile_native_addon.cjs` | **Regression from PR #96.** `require()` pre-creates the module object in `js_esm_import_sync_cstr_from_require` keyed on the resolver's virtual `/$ant/...` path. `esm_load_commonjs_module` then read `__dirname` and `__filename` from that pending object instead of from its `module_path` argument, which is the real materialized directory, and nothing rewrote them. Inside a materialized native package the fixture spawned its helper at the virtual path and got exit 127 with empty stderr, reported as "helper failed". `require.resolve` was unaffected because it already maps through `ant_bundle_materialized_path` (`loader.c:2504`). The earlier "pre-existing, fails on v14" verdict was wrong: the test and fixture were rewritten after v14, so v14 fails on assertions that did not exist when it was built. | Fixed at the source in `js_esm_import_sync_cstr_from_require` (`src/esm/loader.c`): the pre-created module object is now built from the registered module record's `resolved_path`, which is the real materialized file, while the require cache and module key keep the virtual path. That makes `filename`, `path`, and `paths` consistent from creation instead of patching them at load time. Six lines, uncommitted. Passes three runs in a row; spec suite and every `test_*require*`, `test_cjs_*`, `test_module_*`, `test_compile_*`, `test_esm_*`, `test_import_*` file pass on the rebuilt binary. The test silently skips when `build/ant-runtime` is missing, so build it with `meson compile -C build ant-runtime` before trusting a pass. |
| `test_eval.cjs` | Engine gap: sloppy-mode direct eval did not leak `var` or function declarations into the caller's scope. Node 26 and Bun 1.4.0 leak both. | Fixed in the working tree; the spec-faithful test from fe9904d6 passes. |

## Fresh sweep after the fixes

Run on 2026-09-07 evening against the working-tree build at `828a9b2f` with
the uncommitted loader fix for the native addon test. Same runner, 90s per
file, six in parallel. Seven test files were added between the checkpoint
and this run.

| Suite | Result |
| --- | --- |
| `tests/test_*` (576 files) | 572 pass, 0 fail, 3 exit non-zero by design, 1 expected timeout |
| Spec suite (`examples/spec/run.js --all`) | 4221 tests, 102 files, 0 failures |
| Module, require, compile, esm, import test families | all pass |

The only non-passing files are the three by-design exits and the GC stress
loop listed below.

## Exit non-zero by design, 3

These are not failures. They are sample sources whose whole point is the
non-zero exit, so any sweep that treats exit status as pass/fail will list
them. Do not fix or remove them; skip them when counting.

| Test | Why it exits non-zero |
| --- | --- |
| `test_highlight_long_strings.js` | Throws after three long string lines so the syntax highlighter can be eyeballed near the terminal edge. |
| `test_throw_stack.cjs` | Throws a string through three nested frames to show the stack trace format. |
| `test_with_strict.cjs` | Expects the SyntaxError for a `with` statement under `"use strict"`. |

## Expected timeout, 1

- `test_gc_stress10.js`. Not a failure. It is an open-ended TUI stress loop
  still rendering frames at the 90s cutoff with stable memory. Any per-file
  timeout will cut it off; that is the expected result.

## Differential runner mismatches

All 11 also mismatch on v14, so none are recent. v14 had 16; the five `path`
mismatches were fixed by the path port.

- `regexp` 0 to 4: empty regex `source` returns `""` instead of `"(?:)"`,
  `$<name>` replacement patterns are left literal, and replacing with lone
  surrogates yields U+FFFD.
- `property` 1 and 4: `JSON.stringify` drops keys containing a NUL byte.
  Property get, has, and descriptor agree with Node, so this is a JSON gap.
- `stream-shape` 1 to 4: covered by the stream property surface plan.

## CI note

The `build-platform` workflow only runs the cron and upgrade tests. Nothing
else under `tests/`, the JIT harness, or the spec suite runs in CI, which is
how the PR #95 and PR #96 regressions reached master green.

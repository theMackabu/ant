# Test Sweep 2026-09-07

Status: active
Last reviewed: 2026-09-07
Owner: theMackabu

Results of running every `tests/test_*` file, the JIT harness, the spec suite,
the Node differential runner, and `maid preflight`. The first pass ran against
`13e340d5`; the current numbers are from the working-tree build at `87077b56`
plus the uncommitted `new.target` native-frame change. The v14 column is the
`v14.0.ff84a70d.0` release binary from 2026-08-17. Note that v14 is only
useful for tests that existed unchanged at that tag; see the native addon row.

## Current totals

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
| `test_websocket_client_buffered_frames.cjs` | `new.target` lived in one global slot on `ant_t`, so the WebSocket constructor's target leaked into the native accept callback. `net_socket_create` built the accepted socket with the WebSocket prototype and the connection listener got an object with no `.on`. The `module.exports` data-property change exposed the leak by removing an incidental getter call that cleared the slot. | JS/JIT frames now carry invocation state, and native callbacks receive explicit `call_new_target` through `ant_params_t`. Native constructor frames only root targets for GC; the native accept callback explicitly selects the default Socket prototype. Uncommitted; adds `tests/test_new_target_frames.cjs`. See [net-connection-websocket-arg-regression.md](net-connection-websocket-arg-regression.md). |
| `test_cli_file_arg_precedes_package_script.cjs` | `ant t.js` ran the package script named `t.js` instead of the file. | `src/main.c` skips the script shortcut when the positional names an existing regular file. Committed in 87077b56. |
| `test_console_inspect_string_internals.cjs` | Stale. PR #90 raised `STR_SHORT_CONS_THRESHOLD` from 13 to 32, so the 13-char concat now copies flat by design. | Rope case uses two 16-char strings. Committed in 87077b56. |
| `test_repl_static_import.cjs` | Stale. Since 671d8071 the prompt is dim `❯`, a reset escape, then a space, so the pty driver's literal `❯ ` marker never matched and it killed the REPL at its deadline. | Driver matches the prompt with SGR escapes allowed. Committed in 87077b56. |
| `test_debug_error_trace.cjs` | Stale from birth. The `ANT_DEBUG=dump/errors:trace` channel and `[ant-debug:error]` marker never existed outside the commit that added the test. | Removed in 87077b56. |
| `test_ffi_wrappers.cjs`, `test_rpc.cjs` | Stale. ESM `import` syntax in `.cjs` files. | Renamed to `.mjs` in fe9904d6. |

## Still failing, 2

| Test | Verdict |
| --- | --- |
| `test_compile_native_addon.cjs` | **Regression from PR #96, unfixed.** Inside a materialized native package, `__dirname` and `__filename` are the virtual `/$ant/...` path while `require.resolve` correctly returns the extracted directory. The fixture spawns a helper at `path.resolve(__dirname, ...)`, gets exit 127, and throws "helper failed". Cause: PR #96 made `require()` pre-create the module object in `js_esm_import_sync_cstr_from_require` keyed on the resolver's virtual path; `esm_load_commonjs_module` then reads `path` and `filename` from that pending object instead of from its `module_path` argument, which is the real materialized path. Fix options: rewrite `id`, `path`, and `filename` on a pending module object when `module_path` differs, or map `resolved_path` through `ant_bundle_materialized_path` in the require path as `require.resolve` already does at `loader.c:2504`. The earlier "pre-existing, fails on v14" verdict was wrong: the test and fixture were rewritten after v14, so v14 fails on assertions that did not exist when it was built. |
| `test_eval.cjs` | **Engine gap.** Rewritten in fe9904d6 to assert spec behavior. Sloppy-mode direct eval does not leak `var` or function declarations into the caller's scope. Node 26 and Bun 1.4.0 pass the test; ant fails at the `var leaked = 42` assertion. Everything else in the test passes. |

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

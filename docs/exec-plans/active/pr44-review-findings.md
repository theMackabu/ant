# PR #44 Review Findings — Fix Plan

Status: complete (pending PGO re-verify of the warm-split micro)
Last reviewed: 2026-07-24
Owner: theMackabu

## Goal

Work through every actionable finding from the PR #44 reviews (CodeRabbit +
Macroscope, batches of 2026-07-11 through 2026-07-23), one at a time, with
verification per item. Every finding below was re-verified against HEAD
(48680128) before being listed — stale, wrong, duplicate, and
already-addressed comments are recorded at the bottom so they are not
re-litigated.

## Constraints

- JIT old-to-young write barriers are semantically required; no fix may
  remove, weaken, defer, or bypass them.
- User-facing error display UX must remain unchanged.
- Control-flow changes require a PGO regeneration before benchmarking
  (`ANT_PGO_IN_NIX_SHELL=1 ./meson/pgo/build.sh`).

## Task list

### Critical — crash

- [x] 1. `Buffer.write` segfaults on non-string input
  - `src/modules/buffer.c` (`js_buffer_write`, ~line 3307)
  - `js_getstr` returns NULL for non-`T_STR` values; the encoder dispatch
    dereferences it. `Buffer.alloc(4).write(123)` → SIGSEGV at 0x4
    (reproduced). Node throws `TypeError`.
  - Fix: reject non-string `args[0]` with a TypeError before dispatch
    (match Node's "argument must be a string").
  - Verify: write(123) / write({}) / write() all throw TypeError; existing
    buffer suites still pass.

### High — correctness

- [x] 2. `JS_NATIVE_CTOR` collides with `SV_CALL_HAS_EVAL_ENV`
  - `include/internal.h:65` (`1u << 5`) vs `include/silver/engine.h:418`
    (`1u << 5`), both tested against the same `closure->call_flags`
    (`engine.h:448` / `engine.h:456`).
  - A constructor closure carrying an eval env is misread as a native ctor
    and routed through `sv_call_native` with no receiver.
  - Fix: move `JS_NATIVE_CTOR` to a free bit; audit `call_flags` bit
    assignments for other overlaps while there.
  - Verify: repro with `new` on a function that closes over `eval`;
    spec suite.

- [x] 3. Use-after-free in string-call fast paths
  - RESOLUTION NOTE: fixed at the helper level (snapshot args before
    to_string_val) instead of gating the interpreter labels on a T_STR
    receiver — that covers all three entry points (interpreter labels, JIT
    jit_helper_call_string_builtin, plain builtins) and keeps the fast path
    for coercible receivers. Audit also found and fixed the SAME hazard in
    js_array_includes_call's generic path (args[1] read after a proxy
    length trap; from_arg now snapshotted at entry).
  - `src/silver/engine.c` `L_CALL_STRING_INDEXOF` / `L_CALL_STRING_SUBSTRING`
    (~line 1483).
  - `call_args` points into `vm->stack`; the helpers call
    `to_string_val(this_val)` first (`src/ant.c:12471`), which can run user
    JS (object receiver with custom `toString`) and realloc the stack, then
    read `args[0]` stale.
  - Fix: take the fast path only when `vtype(call_this) == T_STR`; otherwise
    fall through to `sv_vm_call`.
  - Verify: repro with `String.prototype.indexOf.call({toString(){...deep
    recursion to force stack growth...; return "x"}}, "x")`; spec suite.

- [x] 4. Stale `vs.obj_site` across JIT control-flow joins
  - RESOLUTION NOTE: conservative clear alongside the other speculative
    slot state at the single branch-target reset block. This also discards
    still-valid sites for literals whose construction spans a join
    (branchy field values), losing the DEFINE_FIELD site fastpath for
    fields after the branch — measured bench_jit_object_literal at parity
    with installed (~40ms both), so the cost is not observable. If a
    literal-heavy workload ever regresses, the precise fix is tracking
    obj_site in the label-merge entries.
  - `src/silver/swarm.c` branch-target reset block (~line 4410).
  - The join reset memsets `slot_type` / `known_func` / `has_const` but never
    `obj_site`, so a stale object-literal site pointer can survive a merge
    and let `jit_helper_define_field` overwrite the wrong site's
    `shared_shape`.
  - Fix: clear `vs.obj_site` (memset the array) alongside the other
    speculative per-slot state at every branch target.
  - Verify: targeted test with two predecessors storing different values in
    the same stack slot (one an object literal), then a DEFINE_FIELD after
    the join; JIT suites.

### Medium — correctness / spec divergence

- [x] 5. JIT put-field skips `regexp_note_property_write`
  - RESOLUTION NOTE: compile-time gate in the OP_PUT_FIELD case via a new
    shared `regexp_property_write_is_watched` predicate in
    include/modules/regex.h (single source of truth with the watcher).
    OP_DEFINE_FIELD is intentionally not gated: the interpreter's define
    path does not call the watcher either, so this is exact parity.
    Today's flag consumers also re-verify exec identity per call, so the
    bypass was defense-in-depth rather than an observed miscompile.
  - Interpreter put path calls it (`src/silver/ops/property.h:735`);
    `src/silver/swarm.c` has zero references, so a JIT-hot
    `RegExp.prototype.exec = ...` store skips regex fast-path invalidation.
  - Fix: cheapest is compile-time — refuse the put-field fastpath when the
    atom is one of the watched names (exec/replace/etc., mirror the
    interpreter's watch list) so those stores take the slow path.
  - Verify: JIT-warm loop that overwrites `exec` then calls a regex fast
    path; confirm the override is honored.

- [x] 6. matchAll empty-match advance ignores the `v` flag
  - RESOLUTION NOTE: the same unicode-only read existed at THREE advance
    sites — @@match, matchAll next, and @@replace (the review flagged only
    matchAll). All three now use a shared `regexp_full_unicode_flag`
    helper (u OR v, error-propagating). Verified exact Node parity on
    matchAll/match/replace with /gv and /gu over astral subjects.
  - `src/modules/regex.c:~1716` reads only `"unicode"`; `unicodeSets` is
    supported (`REGEXP_FLAG_UNICODE_SET`) but not consulted, so
    `/(?:)/gv` under-advances into surrogate pairs.
  - Fix: treat `unicodeSets` as full-unicode too (or use the iterator's
    captured full_unicode flag).
  - Verify: `[...'😀a'.matchAll(/(?:)/gv)]` produces the boundary before
    `a`; regex suites.

- [x] 7. `regexp_split_fast` skips `update_regexp_statics`
  - RESOLUTION NOTE: one update_regexp_statics call per successful
    separator match, placed before the empty-match advance so empty
    separator matches update statics like the exec-based slow path does.
    No RegExp.input static exists in the engine, so nothing else to
    mirror. Adds ~1 small-string alloc per separator; warm-split micro
    within cross-build noise (re-verify on fresh PGO).
  - `src/modules/regex.c:~2280` calls pcre2 directly; the slow path updates
    legacy statics via the exec path (`regex.c:1246`/`1334`), so
    `RegExp.$1` / `RegExp.lastMatch` diverge between paths after `split`.
  - Fix: call `update_regexp_statics` with the match data after each
    successful separator match in the fast loop.
  - Verify: `'a,b'.split(/(,)/)` then read `RegExp.$1` / `RegExp.lastMatch`;
    compare fast vs slow path (slow path forced via subclass).

- [x] 8. `Buffer.write` latin1/ascii writes UTF-8 bytes
  - RESOLUTION NOTE: the same wrong treatment covered the WHOLE latin1
    surface, not just write — Buffer.from memcpy'd UTF-8 bytes,
    byteLength returned UTF-8 length, and toString UTF-8-decoded. All
    four fixed as one contract: encode = one byte per UTF-16 code unit
    (unit & 0xff, shared buffer_encode_latin1_into with ascii fast
    path), byteLength = utf16_strlen, decode = byte -> U+00xx (ascii
    decode additionally & 0x7f). Byte-for-byte Node parity on 12 probes
    including astral surrogate masking.
  - `src/modules/buffer.c:~3276`: `ENC_LATIN1`/`ENC_ASCII` fall through to
    `buffer_encode_utf8_into`. Reproduced: `Buffer.alloc(2).write("é", 0, 2,
    "latin1")` writes `c3 a9` and returns 2; Node writes `e9`, returns 1.
  - Fix: dedicated latin1 path that decodes UTF-8 input to code points and
    writes one byte per code point ≤ 0xFF (Node semantics: code unit &
    0xff). NOTE: CodeRabbit's suggested patch (`dst[i] = str[i]`) is wrong —
    it still copies UTF-8 bytes verbatim.
  - Verify: latin1/ascii writes of non-ASCII match Node byte-for-byte;
    buffer suites.

- [x] 9. Replacer callback receives unclamped match index
  - RESOLUTION NOTE: full ToIntegerOrInfinity semantics (js_to_number,
    so string/object indices coerce per spec — the old code zeroed any
    non-number), truncate, clamp to str_utf16_len. Get errors on .index
    now propagate. Node parity on 1.5/-3/99/NaN/"2"/valueOf probes.
  - `src/modules/regex.c:2140-2162`: raw `position_units` (double from a
    custom exec's `.index`, e.g. 1.5 or out-of-range) is passed to the
    callback while slicing uses the clamped byte position.
  - Fix: ToIntegerOrInfinity + clamp to the subject's UTF-16 length before
    building `call_args`.
  - Verify: custom exec returning `{index: 1.5}` / huge index; callback sees
    the clamped integer Node would pass.

- [x] 10. `subtle.generateKey` accepts missing arguments
  - RESOLUTION NOTE: nargs < 3 now rejects with TypeError (message style
    matches importKey's). Found while verifying: the subtle wrapper
    double-reports impl errors (promise rejects AND the error surfaces
    as an uncaught top-level error) — pre-existing, reproduced with
    importKey on the installed binary; logged under follow-ups.
  - `src/modules/crypto.c:920-926`: nargs 1–2 silently default
    `extractable=false`, `usages=undefined`; WebIDL requires all three
    (TypeError).
  - Fix: `if (nargs < 3) return js_mkerr_typed(..., TypeError)`.
  - Verify: 1- and 2-arg calls reject with TypeError; webcrypto test.

- [x] 11. `CryptoKey.usages` aliases the caller's array
  - RESOLUTION NOTE: crypto_make_key_object snapshots the array
    (js_mkarr + per-element push), covering generateKey and importKey.
    Mutating the input array after creation no longer changes
    key.usages.
  - `src/modules/crypto.c:864`: the passed usages array is stored without
    copying; mutating it after creation mutates the key.
  - Fix: copy into a fresh array (js_mkarr + push each element) at creation.
  - Verify: mutate the input array post-generateKey; key.usages unchanged.

- [x] 12. `headers_data_copy` swallows OOM; `Response.redirect` error-path leak
  - RESOLUTION NOTE: list_append_raw now returns bool; headers_data_copy
    frees the partial list and returns NULL on any append failure
    (response.c's data_dup already handled NULL — the check was dead
    until now). Failure also propagates at the three other raw-append
    sites: headers_copy_from returns false, the set path returns the
    append result, headers-init returns an oom error. Redirect's
    ensure-headers failure path now frees href and clears the parsed
    url like the surrounding error exits. OOM paths verified by
    inspection; happy paths (redirect, clone, Headers copy
    independence) probed against Node.
  - `src/modules/headers.c:~304`: `list_append_raw` discards append failure,
    so cloning returns a silently partial copy on OOM.
  - `src/modules/response.c:~944`: early return on `response_ensure_headers`
    failure skips `free(href)` / `url_state_clear(&parsed)`.
  - Fix: propagate append failure (return NULL) from headers_data_copy and
    handle at callers; add the missing cleanup on the redirect error path.
  - Verify: code inspection + existing header/response suites (OOM paths are
    hard to exercise directly).

- [x] 13. Win32 drive-relative paths treated as absolute
  - RESOLUTION NOTE: path_absolutize now mirrors resolve(): fully
    qualified (path_is_absolute_style) returns as-is, drive-relative
    C:foo keeps its drive and joins the cwd (like resolve's
    drive_prefix carry), posix behavior unchanged. Also implemented
    Node's device-less-root quirk in relative() itself: '\a' -> '\b'
    returns the resolved target (no ..-walk) while C:\-rooted paths
    do walk. 14-case matrix (11 win32 + 3 posix) diffs clean against
    Node; 7 cases pinned in test_path_relative_resolve.cjs.
  - `src/modules/path.c:161` (`path_absolutize`): any
    `path_root_length > 0` returns the input unchanged, but win32 `C:foo`
    (root len 2) and `\foo` (root len 1) are not fully qualified, so
    `path.win32.relative` compares unresolved inputs.
  - Fix: check fully-qualified (style-aware) instead of root-length > 0 — or
    document the limitation if Node's own posix-host behavior is the
    reference. Decide against Node's actual `path.win32.relative` output on
    a posix host before coding.
  - Verify: `path.win32.relative('C:\\a', 'C:foo')` and `\foo` cases vs
    Node.

- [x] 14. Element-IC interns every string key (unbounded intern growth)
  - RESOLUTION NOTE: the review undersold it — the elem-IC site was one
    of EIGHT read-path inserters; the generic lookups (lkp, lkp_val,
    lkp_with_getter/setter, lkp_proto, lookup_prop_meta,
    js_get_ctor_proto) all interned every probed key, so the fallback
    path grew the table too. Added intern_string_existing (find-only)
    and switched all read paths; correctness is exact because shapes
    can only contain interned names, and writes still intern at
    property creation. Unique-key storm: 200,010 entries/9.3MB -> 10
    entries/416B. 100/100 spec files pass; bench_lkp delta proven to be
    scratch-vs-PGO build noise via stash A/B. Regression test
    test_intern_table_bounded.cjs (fails on the old binary).
  - `src/silver/ops/property.h:~555`: `sv_get_elem_ic` interns each string
    key on the IC path; intern table never evicts, so
    `obj[uniqueRuntimeString]` in a loop grows memory without bound.
  - Fix (design decision): bound it — e.g. only intern once the same IC site
    has seen a string key hit, or skip interning for keys not already
    interned (probe-only lookup), keeping the fallback path allocation-free.
  - Verify: loop over 1M unique keys; RSS stays bounded; elem IC benches
    unchanged.

### Low — hygiene, tests, docs

- [x] 15. `OPENSSL_cleanse` generated key buffers
  - RESOLUTION NOTE: both branches cleanse sizeof(buf) after
    crypto_make_key_object copies the material. The RAND_bytes-failure
    returns leave buf unwritten, so no cleanse needed there.
  - `src/modules/crypto.c:~933`: stack buffers holding fresh AES/HMAC key
    material are left uncleansed after `crypto_make_key_object` copies them.
  - Fix: `OPENSSL_cleanse(buf, len)` on both branches (including error
    returns after RAND_bytes succeeded).

- [x] 16. Protocol doc stress10 kill block orphans the ant child
  - RESOLUTION NOTE: both snippets now pkill the ant child by test name
    after the wrapper kill, matching the surrounding prose.
  - `docs/exec-plans/active/gc-and-server-benchmark-protocol.md:151-158` and
    `195-203`: the example kills only the `/usr/bin/time` wrapper PID while
    the surrounding text warns exactly against that.
  - Fix: make the snippets signal the ant child / process group (the pkill
    workaround the text already describes).

- [x] 17. Buffer registry test: assert absolute drift
  - `tests/test_buffer_registry_slots.cjs:~68`: `drift < 4MiB` passes on
    negative underflow; use `Math.abs(drift) < 4MiB`.

- [x] 18. WTF-8 exec test is vacuous on first-match failure
  - RESOLUTION NOTE: first match asserted explicitly (non-null, index 3,
    lastIndex 4) before the exhausted re-exec null check.
  - `tests/test_regex_utf16_positions.cjs:~60`:
    `eq(..., g.exec(wtf) && g.exec(...), null)` passes if the FIRST exec
    wrongly returns null. Assert the first match (and its index/lastIndex)
    explicitly before the null re-exec check.

- [x] 19. WebCrypto test: check first key material nonzero
  - RESOLUTION NOTE: also pinned items 10/11 here (arity rejections and
    the usages snapshot) since this file is their natural home.
  - `tests/test_webcrypto_generate_export.cjs:~32`: only `raw2` is checked
    for nonzero bytes; assert `raw` too.

## Not actionable (do not re-open)

- **Already addressed** (marked on the PR): obs-text header bytes +
  harness spawn stream events (`bd4718a`); case-insensitive
  `headers_data_append_if_missing` + put-field transition write barrier
  (`0f5e061`).
- **Stale**: "ensure capacity before shape swap" — HEAD already calls
  `js_obj_ensure_prop_capacity` before installing the new shape
  (`src/silver/ops/property.h:760`).
- **Wrong**: elysia2 logger hook names — `.request()` /
  `.afterHandle('global')` / `.error('global')` construct fine on the
  installed `v2.0.0-exp.38` (probed directly; benches served through it).
- **Deliberate divergence**: `'😀'.match(/x*/g)` → 2 empties vs Node's 3.
  WTF-8 byte storage has no mid-surrogate-pair position; whole-character
  advance is the engine convention. Documented in the test comment and the
  benchmark protocol decision log. (CodeRabbit's suggested "fix" would fail
  the suite.)
- **Duplicate**: the two path.c win32 comments are one issue (item 13).
- Discussion comments (walkthrough, review triggers, approvability
  verdict): housekeeping only.

## Validation status

- Every item above re-verified against HEAD 48680128 on 2026-07-23 before
  listing (segfault and latin1 reproduced on the no-JIT build; flag values,
  missing clears, and missing calls confirmed by reading HEAD).
- 2026-07-24: all 19 items fixed, each verified on a JIT scratch build
  (items 1-2 also on no-JIT) with Node cross-checks where applicable;
  full spec suite 100/100 after the intern change; six regression tests
  added to the harness. Perf-sensitive fixes (obj_site clear, watched
  put-field gate, intern find-only, split statics) measured at parity —
  re-verify the warm-split micro on the next fresh PGO build.

## Follow-ups (out of scope for this plan)

- Node rejects generateKey with empty keyUsages for secret keys
  (SyntaxError); ant accepts them — pre-existing divergence, the
  webcrypto test has always relied on it.
- Subtle-crypto wrapper double-reports impl errors: the returned promise
  rejects AND the same error is raised as an uncaught top-level error
  (pre-existing; reproduced with importKey on the installed binary).

- Pre-existing, both merge parents, untracked anywhere: byte-wise
  `"héllo".split("")` mojibake; `indexOf` NaN-position raw cast (latent,
  arm64 happens to produce the spec answer); `String.prototype.includes`
  position treated as byte offset (`"😀a".includes("a", 3)` → true, spec
  false). Candidate small PR after #44 lands.

# React Doctor String Indexing Investigation

Status: active
Last reviewed: 2026-06-25
Owner: theMackabu

## Goal

Get `./build/ant x --ant react-doctor docs/private/registry --no-lint --no-dead-code --no-score --blocking none --no-color -y` to complete without hanging or crashing, while keeping the fix in Ant's runtime semantics rather than patching React Doctor.

This investigation started as package compatibility work and then narrowed to a runtime hot-loop problem in string indexing.

## Current Findings

- React Doctor now gets past earlier compatibility failures for `fs.chown`, `BigInt.prototype.constructor`, `require("module")` shape, `events.errorMonitor`, and child-process stream behavior.
- A later crash was in PCRE2 JIT during package-tooling regex work. The current local tree disables PCRE2 JIT in `src/modules/regex.c` as a conservative workaround until the generated-code lifetime issue is isolated.
- After the PCRE workaround, the stripped React Doctor scan hangs during the synchronous security scan.
- Temporary instrumentation showed the hang reaches:
  - `checkSecurityScan`
  - file: `worker-configuration.d.ts`
  - rule: `clickjacking-redirect-risk`
  - specifically inside `stripCommentsPreservingPositions`, before the rule regex itself runs.
- The clickjacking regex is fast when tested directly against the same file. The slow path is the plugin's JS loop over a large string using repeated `content[index]` and `content.length`.
- `charCodeAt` was made fast with a cached ASCII fast path, but `content[index]` plus repeated `content.length` in the same loop still showed pathological behavior during this handoff.

## Important Local State

- The installed React Doctor package under `~/.ant/pkg/exec/react-doctor` has temporary `antdebug` instrumentation in:
  - `node_modules/react-doctor/dist/cli.js`
  - `node_modules/oxlint-plugin-react-doctor/dist/index.js`
- Restore the CLI bundle from `/tmp/react-doctor-cli.js.bak` before final validation if that backup is still present.
- Remove the plugin instrumentation manually or reinstall the package cache before final validation.
- Before resuming, check for stuck processes:

```sh
ps -axo pid,ppid,stat,etime,command | rg 'react-doctor|build/ant|\.ant/pkg/exec'
```

## Current Runtime WIP

The current local tree includes partial string fast-path changes:

- `include/internal.h`
  - added `ascii_char_cache[128]`
  - added `js_ascii_char_string`
- `src/gc/objects.c`
  - marks the ASCII character cache
- `src/ant.c`
  - added cached ASCII checks for string length/index/`charCodeAt`
- `src/silver/ops/property.h`
  - added cached ASCII checks for Silver string length/index
  - has an in-progress rooting change around string index materialization
- `src/silver/glue.c`
  - patched `jit_helper_get_length` to use cached ASCII metadata
- `include/silver/opcode.h`
  - currently marks `GET_LENGTH` as non-JIT-eligible as a temporary containment step

This is not ready to land as-is. The next pass should either finish the GC/rooting and JIT analysis cleanly or revert the half-measure `GET_LENGTH` JIT flag change.

## Repros

React Doctor stripped scan:

```sh
PATH=build:$PATH ./build/ant x --ant react-doctor docs/private/registry --no-lint --no-dead-code --no-score --blocking none --no-color -y
```

Direct regex sanity check:

```sh
./build/ant <<'EOF'
const fs = require('fs');
const s = fs.readFileSync('docs/private/registry/worker-configuration.d.ts', 'utf8');
const r = /\bredirect\s*\((?!\s*(?:await\s+)?[\w$]*(?:safe|valid|sanitiz|allowlist|whitelist)[\w$]*\s*\()[^)'"`\n]*\b(?:searchParams\.get|nextUrl\.searchParams|returnTo|callbackUrl|continue|next)\b|<iframe\b[\s\S]{0,700}\b(?:next=|continue=|redirect=|redirect_uri|userstoinvite|sharingaction|role=|\.\.)|frame-ancestors\s+(?:\*|'self'\s+\*)|X-Frame-Options["']?\s*:\s*["']?ALLOW/i;
console.time('test');
console.log(r.test(s));
console.timeEnd('test');
EOF
```

String-indexing repro:

```sh
./build/ant <<'EOF'
const fs = require('fs');
const s = fs.readFileSync('docs/private/registry/worker-configuration.d.ts', 'utf8');
let i = 0, n = 0;
console.time('while');
while (i < s.length) {
  if (s[i] === '*') n++;
  i += 1;
}
console.timeEnd('while');
console.log(n);
EOF
```

Control that was fast after the partial fix:

```sh
./build/ant <<'EOF'
const fs = require('fs');
const s = fs.readFileSync('docs/private/registry/worker-configuration.d.ts', 'utf8');
let n = 0;
for (let i = 0, len = s.length; i < len; i++) {
  if (s[i] === '*') n++;
}
console.log(n);
EOF
```

## Next Steps

1. Clean up temporary React Doctor instrumentation or preserve it intentionally while debugging.
2. Decide whether the runtime WIP should be kept or reverted to a smaller starting point.
3. Confirm whether the remaining slow loop is:
   - JIT OSR behavior for `GET_LENGTH` plus `GET_ELEM`
   - missing GC rooting after popping the source string from the VM stack
   - a stale generated-code path that bypasses the patched helpers
4. Avoid treating this as a React Doctor bug. The direct repro is an Ant runtime issue on large ASCII strings.
5. After the string-loop repro is fixed, rerun the stripped React Doctor scan, then run the full scan.

## Validation Status

- `meson compile -C build` passed after several intermediate runtime changes.
- Focused child-process tests passed before this string-indexing detour.
- The final runtime WIP was interrupted before validation.
- `maid preflight` needs to be rerun before any final handoff.


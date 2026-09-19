# TypeScript captured-variable cleanup

Status: active
Date: 2026-09-17
Owner: theMackabu

## Goal

Remove repeated scans of unrelated open captures during the TypeScript build
at `/Users/themackabu/Downloads/TypeScript`: `ant lib/tsc.js -b src`.
Use forced builds for comparisons and verify declaration/map output hashes.

## Verified finding and design

Interpreter frame and CLOSE_UPVAL cleanup currently traverse the whole VM open
capture list. Capture insertion is descending by slot address, but JIT bailout
rebasing mutates locations in place and generator resume prepends captures. Those
transitions do not preserve a global address order in mixed VM/JIT lists.

Preserve descending address order when locations change, then stop cleanup at
the first slot below the closing boundary. Retain the VM-stack membership check
so interpreter cleanup does not close a live JIT parent's captures. Share the
cleanup implementation between frame exits and CLOSE_UPVAL. VM stack growth
commits reserved storage and does not relocate the stack. No layout growth,
tiering changes or PGO regeneration are planned for this focused experiment.

## Validation

- Pin the existing main-checkout and installed binaries/profile before edits.
- Native coverage for mixed slot ranges, rebasing, generator resume ordering,
  retained outer captures, and closing write barriers.
- JS closure, exception, generator, OSR/bailout, eval and GC regressions.
- Full spec suite and repository checks before completion.
- Serial forced TypeScript A/B comparisons, declaration/map equivalence, and
  a separate native profile to confirm the scanning hotspot disappears.
- Preserve unrelated native-profile documentation from the preceding task.

Artifacts: `.cache/tsc-upvalue-cleanup-20260917/`. No commit requested.

## 2026-09-18 integration

Restored from stash `4165fdd2` into the exception-completion branch. Native order,
rebasing, resume, and write-barrier coverage and focused JS/GC regressions pass.
A fixed-work return-cleanup probe confirms bounded cleanup as outer captures
grow. Current integration results are in
[the completed follow-up](../completed/exception-performance-and-finally.md).
The TypeScript wall-time experiment above has not been rerun on this branch.

# UTF-16 Random-Access Index

Status: completed
Last reviewed: 2026-09-10
Owner: theMackabu

Historical landing summary from 2026-08-14. The validation below was not
rerun during this documentation cleanup.

Fixed `String.prototype.endsWith`-style positional ops that are O(n) per call
on non-ASCII strings: the single-entry scan-cursor cache stores the position at
the match point, so a target near the end resumes from ~0 whenever another
string evicted the entry between calls (2-string thrash = full rescan per
call). The landed fix is an 8-way per-string UTF-16 checkpoint index alongside
the existing one-entry scan cursor. The 4-way cursor described below was tested
and rejected because it regressed the sequential conversion gate.

## Landing result (2026-08-14)

The final implementation keeps the single scan cursor, because it is the best
representation for the dominant sequential single-string case. Long-gap
lookups additionally use an 8-way checkpoint index with these controls:

- consult the index only when the saved cursor is more than 64 UTF-16 units
  behind the target;
- admit a string on the first qualifying lookup and build its index only on the
  second, so one-off long strings do not pay for a full scan and allocation;
- check the most-recently-used index way first;
- invalidate cursor and index entries on string sweep, when their raw string
  pointers can actually become stale, rather than on every GC cycle;
- share the indexed-resume helper across `utf16_codepoint_at`,
  `utf16_index_to_byte_offset`, and `utf16_range_to_byte_range`; keep the
  equivalent local gate in `utf16_code_unit_at` to preserve its hot layout.

The attempted 4-way scan cursor was not retained. Matched-PGO prototypes made
the sequential gate 23-34% slower even after MRU refinements. Returning to one
cursor removed that structural regression while the independent checkpoint
index still fixes alternating random access.

Final interleaved PGO A/B medians used `/tmp/ant_utf16_base`
(`14.0.fe85adda.0`) as the pinned base and `./build/ant`
(`14.0.7fa53028.0`, profile rebuilt from the final source) as the candidate:

| Workload | Base | Candidate | Delta |
|---|---:|---:|---:|
| Sequential non-ASCII `Buffer.from(..., "ucs2")` | 545 ms | 491.5 ms | -9.8% |
| Two-string non-ASCII `endsWith` alternation | 855.5 ms | 8 ms | -99.1% (107x faster) |
| 500 one-off non-ASCII `endsWith` strings | 93 ms | 90 ms | -3.2% |
| Random non-ASCII `charCodeAt` | 330.5 ms | 333.5 ms | +0.9% |
| `tests/bench.js` total | 2953.5 ns | 2909.8 ns | -1.5% |

All workloads produced identical checksums. The broad benchmark's string and
RegExp rows were flat or modestly better. Validation passed the focused random
access, Buffer search, and string-accumulation tests; `maid preflight`; and the
full spec suite (3886/3886 tests across 100 files). The final binary and profile
were produced with `./meson/pgo/build.sh --force-no-nix`.

## Rejected Alternatives And Earlier Investigation

The July/August port combined a four-way scan cursor with the checkpoint index
and was reverted from the event-internals branch. Even with gap gating and
an MRU hint, sequential conversion measured 87-90ms against a true 58ms
baseline. Disabling the index did not remove that regression. The later
matched-PGO investigation isolated the cursor representation as the problem;
the single-cursor landing above supersedes the old unresolved checkpoint.

Outlining index seek into a noinline helper also measured about 10% slower in
that earlier port. The final design preserved the sequential hot path and
made checkpoint indexing conditional on repeated long-gap access. The module
import GC flake observed during the port reproduced on clean HEAD; its separate
resolution is recorded in [Module Import GC Flake](module-import-gc-flake.md).

## Original Evidence

The rejected implementation dump and intermediate samples remain in Git:

```sh
git show bc206f10a9ea73d3d91302eb208adf1479e4d22e:docs/exec-plans/completed/utf16-random-access-index.md
```

Use current source for implementation work. The archived four-way cursor is
not a drop-in reference or an outstanding landing task.

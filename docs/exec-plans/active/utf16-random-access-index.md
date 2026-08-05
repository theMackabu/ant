# UTF-16 Random-Access Index (carry-back for fable-perf-fixes)

Status: active
Last reviewed: 2026-08-01
Owner: theMackabu

Fixes `String.prototype.endsWith`-style positional ops that are O(n) per call
on non-ASCII strings: the single-entry scan-cursor cache stores the position at
the match point, so a target near the end resumes from ~0 whenever another
string evicted the entry between calls (2-string thrash = full rescan per
call). The fix — a 4-way cursor cache plus a per-string UTF-16 checkpoint index
— lives on the `fable-perf-fixes` branch and should land with it, not
separately. This plan records the improvements made (and the unresolved perf
problem found) during a validated-then-reverted port of that code on
`feat/unify-event-internals`, so they are not lost when fable lands.

Decision (2026-08): the fable-perf-fixes 4-way scan cache + u16 checkpoint index were
ported into feat/unify-event-internals, validated, then REVERTED from that branch to
keep its scope to event internals + issue fixes. The optimizations should land with
fable-perf-fixes itself. Three improvements were made during the port that the fable
branch does NOT have; apply them when landing it:

## 1. Gap-gate the index at call sites (recovers sequential-scan perf)
Building/consulting the index on every call regressed sequential access
(Buffer.from ucs2 loops). Only consult it when the cursor is far from the target:

    utf16_scan_cursor_resume_utf16(&cursor, target);
    if (target > cursor.utf16_pos && target - cursor.utf16_pos > (size_t)U16_IDX_CHUNK) {
      const u16_idx_entry_t *ix = u16_index_get(cursor.str, cursor.byte_len);
      if (ix) u16_index_seek(ix, &cursor, target);
    }

Apply at all four sites: utf16_code_unit_at, utf16_codepoint_at,
utf16_index_to_byte_offset, utf16_range_to_byte_range.

## 2. MRU way hint in utf16_scan_cache_find (1-compare hot path)
Sequential single-string workloads otherwise pay a 4-way linear scan twice per call
(resume + store):

    static _Thread_local uint32_t utf16_scan_cache_mru;
    // find: check ways[mru] first; on scan hit set mru = i
    // store: on victim allocation set mru = victim way
    // sync_epoch reset: mru = 0

## 3. Invalidate the u16 index on string SWEEPS, not every GC epoch
gc_get_epoch() bumps on every collection, discarding indexes for strings that
survived. A dedicated counter bumped in gc_strings_sweep() (g_strings_sweep_epoch in
src/gc/strings.c, getter declared in include/gc/strings.h) only invalidates when
string memory is actually reclaimed/reused. Measured neutral on benches but is the
correct lifetime; keep it if the plumbing is acceptable, else gc_get_epoch() is safe.

NOTE: do NOT outline the index-seek into a noinline helper — measured 10% WORSE
(95-102ms vs 87-90ms on Buffer.from ucs2 non-ascii 100KB) than the inline gated form.

## Validation notes from the port (all on feat/unify-event-internals, 2026-07/08)
- endsWith 2-string non-ascii thrash: 200ms -> 4ms (checkpoint index win)
- Buffer.from ucs2 sequential (non-ascii 100KB): true pre-port baseline 58ms; ported
  build 87-90ms even WITH gap-gate + MRU (+50%). A feature-disabled (#define'd out)
  build of the ported code also measured 86-90ms, so the residual cost is not the
  index/cache logic itself but something structural in the ported section (layout,
  find()-through-pointer resume path). UNRESOLVED - profile before landing fable's
  version; the sequential regression is real, not machine drift.
- strops/jq output diffs vs node: identical; spec --all clean
- A "modules import/exec error (TypeError: invalid object)" flake seen during
  validation reproduces on clean HEAD at the same rate (4/30 vs 4/18) - pre-existing
  GC bug in module import (mkprop on shape-less object), unrelated to this work.

## Exact section as it existed at revert time (drop-in reference)

```c
#define UTF16_SCAN_CACHE_WAYS 4
static _Thread_local utf16_scan_cache_t utf16_scan_caches[UTF16_SCAN_CACHE_WAYS];
static _Thread_local uint32_t utf16_scan_cache_victim;
static _Thread_local uint32_t utf16_scan_cache_mru;

static inline void utf16_scan_cache_sync_epoch(void) {
  uint64_t epoch = gc_get_epoch();
  if (utf16_scan_caches[0].epoch == epoch) return;
  for (int i = 0; i < UTF16_SCAN_CACHE_WAYS; i++)
    utf16_scan_caches[i] = (utf16_scan_cache_t){ .epoch = epoch };
  utf16_scan_cache_victim = 0;
  utf16_scan_cache_mru = 0;
}

static inline utf16_scan_cache_t *utf16_scan_cache_find(const char *str) {
  utf16_scan_cache_t *mru = &utf16_scan_caches[utf16_scan_cache_mru];
  if (mru->str == str) return mru;
  for (uint32_t i = 0; i < UTF16_SCAN_CACHE_WAYS; i++) {
    if (utf16_scan_caches[i].str == str) {
      utf16_scan_cache_mru = i;
      return &utf16_scan_caches[i];
    }
  }
  return NULL;
}

static inline void utf16_scan_cursor_init(
  utf16_scan_cursor_t *cursor,
  const char *str,
  size_t byte_len
) {
  utf16_scan_cache_sync_epoch();
  cursor->str = str;
  cursor->byte_len = byte_len;
  cursor->start = (const unsigned char *)str;
  cursor->end = cursor->start + byte_len;
  cursor->p = cursor->start;
  cursor->utf16_pos = 0;
}

static inline bool utf16_scan_cache_entry_ok(
  const utf16_scan_cache_t *e, const utf16_scan_cursor_t *cursor
) {
  return e && e->byte_pos <= cursor->byte_len;
}

static inline void utf16_scan_cursor_resume_utf16(
  utf16_scan_cursor_t *cursor,
  size_t target_utf16
) {
  utf16_scan_cache_t *e = utf16_scan_cache_find(cursor->str);
  if (!utf16_scan_cache_entry_ok(e, cursor)) return;
  if (target_utf16 < e->utf16_pos) return;
  cursor->p = cursor->start + e->byte_pos;
  cursor->utf16_pos = e->utf16_pos;
}

static inline void utf16_scan_cursor_resume_byte(
  utf16_scan_cursor_t *cursor,
  size_t target_byte
) {
  utf16_scan_cache_t *e = utf16_scan_cache_find(cursor->str);
  if (!utf16_scan_cache_entry_ok(e, cursor)) return;
  if (target_byte < e->byte_pos) return;
  cursor->p = cursor->start + e->byte_pos;
  cursor->utf16_pos = e->utf16_pos;
}

static inline void utf16_scan_cursor_store(const utf16_scan_cursor_t *cursor) {
  utf16_scan_cache_t *e = utf16_scan_cache_find(cursor->str);
  if (!e) {
    e = &utf16_scan_caches[utf16_scan_cache_victim];
    utf16_scan_cache_mru = utf16_scan_cache_victim;
    utf16_scan_cache_victim = (utf16_scan_cache_victim + 1u) % UTF16_SCAN_CACHE_WAYS;
  }
  e->str = cursor->str;
  e->byte_len = cursor->byte_len;
  e->byte_pos = (size_t)(cursor->p - cursor->start);
  e->utf16_pos = cursor->utf16_pos;
}

#define U16_IDX_CHUNK 64
#define U16_IDX_WAYS  8
#define U16_IDX_MIN_BYTES 256

typedef struct {
  uint32_t byte_off;
  uint32_t u16_pos;
} u16_idx_point_t;

typedef struct {
  const char *str;
  size_t byte_len;
  u16_idx_point_t *points;
  size_t n_points;
} u16_idx_entry_t;

static _Thread_local struct {
  uint64_t epoch;
  uint32_t victim;
  u16_idx_entry_t e[U16_IDX_WAYS];
} u16_idx;

static size_t u16_index_decode_len(const unsigned char *p, const unsigned char *end, size_t *units_out);

static void u16_index_sync_epoch(void) {
  uint64_t ep = gc_strings_sweep_epoch();
  if (u16_idx.epoch == ep) return;
  for (int i = 0; i < U16_IDX_WAYS; i++) {
    free(u16_idx.e[i].points);
    u16_idx.e[i] = (u16_idx_entry_t){0};
  }
  u16_idx.victim = 0;
  u16_idx.epoch = ep;
}

static const u16_idx_entry_t *u16_index_get(const char *str, size_t byte_len) {
  if (byte_len < U16_IDX_MIN_BYTES || byte_len > UINT32_MAX) return NULL;
  u16_index_sync_epoch();
  for (int i = 0; i < U16_IDX_WAYS; i++)
    if (u16_idx.e[i].str == str && u16_idx.e[i].byte_len == byte_len)
      return &u16_idx.e[i];

  size_t max_points = byte_len / U16_IDX_CHUNK + 2;
  u16_idx_point_t *pts = malloc(max_points * sizeof(*pts));
  if (!pts) return NULL;

  const unsigned char *strt = (const unsigned char *)str;
  const unsigned char *end = strt + byte_len;
  const unsigned char *p = strt;
  size_t u16_pos = 0, n = 0;
  pts[n++] = (u16_idx_point_t){0, 0};

  while (p < end) {
    size_t units;
    size_t slen = u16_index_decode_len(p, end, &units);
    if (u16_pos + units > n * (size_t)U16_IDX_CHUNK && n < max_points) {
      pts[n++] = (u16_idx_point_t){(uint32_t)(p - strt), (uint32_t)u16_pos};
    }
    u16_pos += units;
    p += slen;
  }

  u16_idx_entry_t *e = &u16_idx.e[u16_idx.victim];
  u16_idx.victim = (u16_idx.victim + 1u) % U16_IDX_WAYS;
  free(e->points);
  e->str = str;
  e->byte_len = byte_len;
  e->points = pts;
  e->n_points = n;
  return e;
}

static inline void u16_index_seek(
  const u16_idx_entry_t *ix, utf16_scan_cursor_t *cursor, size_t target_u16
) {
  size_t chunk = target_u16 / U16_IDX_CHUNK;
  if (chunk >= ix->n_points) chunk = ix->n_points - 1;
  while (chunk > 0 && ix->points[chunk].u16_pos > target_u16) chunk--;
  if (ix->points[chunk].u16_pos > cursor->utf16_pos) {
    cursor->p = cursor->start + ix->points[chunk].byte_off;
    cursor->utf16_pos = ix->points[chunk].u16_pos;
  }
}

```

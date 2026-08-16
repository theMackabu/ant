# NUL Property Keys and Array Includes Correctness

Status: active
Last reviewed: 2026-08-01
Owner: theMackabu

Two unrelated correctness gaps that surfaced while validating PR #66 review findings.
Both are **pre-existing** — verified byte-identical on a master build (`707b9981`), so
neither is a regression from the propref, events or stream work. They share a document
because both were deferred from that review pass for the same reason: the honest fix is
a storage-layer change, not a spot patch at the site where the symptom shows.

## Part 1: property keys with an embedded NUL are truncated

### Symptom

```js
const k = 'a' + String.fromCharCode(0) + 'b';   // 'a\0b', length 3
const o = {};
o[k] = 1;
o.other = 2;
```

| observation                | ant             | node                    |
| -------------------------- | --------------- | ----------------------- |
| `Object.keys(o)`           | `["a","other"]` | `["a\u0000b","other"]`  |
| `Object.keys(o)[0].length` | `1`             | `3`                     |
| `o[k]`                     | `undefined`     | `1`                     |
| `Object.hasOwn(o, k)`      | `true`          | `true`                  |
| `JSON.stringify(o)`        | `{"a":1,...}`   | `{"a\u0000b":1,...}`    |
| `new Map([[k,1]]).get(k)`  | `1`             | `1`                     |

The `hasOwn true` / `o[k] undefined` split is the tell: some paths carry the real length
and some recompute it with `strlen`, so the same key exists and does not exist depending
on which path asks. `Map` is unaffected because map entries store their own keys.

### Root cause

The intern table itself is binary-clean — `intern_string(str, len)` in `src/ant.c`
memcpys `len` bytes, stores `entry->len`, and dedups with `len` + `memcmp`. The problem
is everything downstream of it:

- `ant_shape_prop_t` (`include/shapes.h`) stores a key as a bare `const char *interned`
  with **no length**, and there is no API to recover the interned length from the pointer.
- Every consumer therefore recomputes `strlen(prop->key.interned)`: enumeration
  (`js_own_property_keys`), JSON stringify's fast object path, descriptor handling, and
  the `js_get`-style lookups that accept a NUL-terminated C string.

CodeRabbit pinned this on `src/modules/json.c:708`, but json.c is a symptom — no length
is available for it to use. Fixing the JSON site alone is not possible.

### Fix shape

Carry the length alongside interned keys, in one of two ways:

1. **Length-from-pointer.** The interned entry header (`interned_string_t`) sits
   immediately before `entry->str` in the same allocation, so
   `((interned_string_t *)ptr) - 1` recovers `len` from any interned pointer in O(1).
   Add `size_t intern_length(const char *interned)` and replace every
   `strlen(prop->key.interned)` with it. No struct layout changes, no shape growth.
   Requires the invariant that shape keys are **always** interned pointers — audit
   before relying on it.
2. **Store the length in the shape.** Widen `ant_shape_prop_t` with a `uint32_t key_len`.
   Simpler to reason about, but grows every shape entry and touches shape
   construction/transition code.

Option 1 is preferred: it is minimally invasive and the header-adjacency invariant
already holds by construction (`entry->str = (char *)(entry + 1)`).

Either way, the C-string-keyed public lookups (`js_get(js, obj, "literal")`) stay
NUL-terminated by contract; only the length-carrying internal paths change.

### Task list

1. Audit that every `ant_shape_prop_t.key.interned` pointer really is an
   `intern_string` result (i.e. nothing sticks a foreign `const char *` into a shape).
2. Add `intern_length(const char *interned)` next to `intern_string` in `src/ant.c`,
   exposed via the header that declares `intern_string`.
3. Replace `strlen(key.interned)` at each consumer: `js_own_property_keys`, JSON fast
   object path (`json_write_object_fast`), descriptor paths, and the property lookup
   helpers that currently strlen before interning the probe.
4. Find the get-path site that loses the length (observed: `o[k]` misses while
   `hasOwn(o, k)` hits — the get side is strlen'ing somewhere before interning).
5. Test: the table above as a `.cjs` regression test, asserting every row matches node,
   plus a JSON round-trip (`JSON.parse(JSON.stringify(o))` preserves the key).

## Part 2: `Array.prototype.includes` misses non-dense elements

### Symptom

```js
const a = [];
Object.defineProperty(a, '0', { value: 'A', configurable: true, enumerable: true });
Object.defineProperty(a, '1', { value: undefined, configurable: true, enumerable: true });
Object.defineProperty(a, '2', { value: 'B', configurable: true, enumerable: true });
```

| observation            | ant     | node   |
| ---------------------- | ------- | ------ |
| `a[0]`                 | `'A'`   | `'A'`  |
| `Object.keys(a)`       | `0,1,2` | `0,1,2`|
| `a.indexOf('A')`       | `0`     | `0`    |
| `a.includes('A')`      | `false` | `true` |
| `a.includes(undefined)`| `false` | `true` |
| `[...a]`               | works   | works  |

`includes` is the only broken consumer — `indexOf`, indexing, spread and enumeration all
see the elements. The elements live in shape properties (or the descriptor table), not
the dense buffer, because they were created by `defineProperty` on an initially empty
array.

### Root cause (partially established)

`array_includes_array_slow` (`src/ant.c` ~10520) takes a fast path when
`array_includes_can_skip_hole_lookups()` returns true, and inside that path returns
`false` outright when `array_may_have_dense_elements()` is false. For this array shape
one of those predicates answers wrongly — the array has no dense elements but does have
index properties, and the fast path concludes "nothing to find".

**A one-line gate is already known not to work**: adding
`array_may_have_dense_elements(arr)` to the `skip_hole_lookups` condition was tried and
did not change the result, so the misprediction is deeper than that condition — likely in
`array_includes_object_may_have_indexed_props(js, arr, /*include_dense*/ false)`, which
walks the shape looking for index keys and is expected to return true here but evidently
does not. Instrument that walk first; do not re-try the gate.

`indexOf` takes a different path and is correct — use it as the reference for where
element reads should come from.

### Task list

1. Instrument `array_includes_object_may_have_indexed_props` on the repro: does the
   shape walk see the `'0'`/`'1'`/`'2'` keys? (Check the `i >= ptr->prop_count` skip and
   the `ANT_SHAPE_KEY_STRING` filter against how `defineProperty` stores index keys —
   they may live in the descriptor table, not the shape.)
2. Fix the predicate (or the slow-path fallthrough) so shape/descriptor-held index
   properties are scanned. Follow `indexOf`'s read path for the element loads.
3. Re-run the accessor interaction case: after the `defineProperty`-over-dense
   materialization fix (commit `6876e2c8`), `[1,2,3]` with a getter on `'1'` works; the
   initially-empty-array case in the table above is what still fails.
4. Test: extend `tests/test_accessor_undefined_result.cjs` or add a sibling covering
   `includes`/`indexOf` agreement on dense, shape-only, and mixed arrays.

## Decision log

- 2026-08-01: Both verified **pre-existing** on master `707b9981` — identical outputs,
  so neither blocks PR #66.
- 2026-08-01: NUL-key fix deliberately not attempted inside the PR #66 review pass.
  json.c (where it was reported) cannot be fixed in isolation, and the real change spans
  shapes, enumeration and lookup paths.
- 2026-08-01: `includes` gate on `array_may_have_dense_elements` tried and reverted —
  measured no behaviour change, so the wrong answer originates elsewhere (see Part 2
  task 1).

## Validation status

Repro tables above were captured against node v26 and ant at `6876e2c8`.

Part 2 FIXED (2026-08-01): the failure was not in the predicates but in
`array_includes_dense_fast` trusting `may_have_holes` — the flag is never set by the
defineProperty path, so the fast scan read raw `T_EMPTY` dense slots and returned false
while the elements lived in the shape. The no-holes branch was removed; an empty slot now
always bails to the slow lookup path. Whole repro table verified identical to node;
bench_includes_dense/sparse at parity. Part 1 FIXED (2026-08-01): intern_length() added via header adjacency, 21 strlen
consumers migrated, and the dynamic-get path converted to length-carrying
(js_try_get_len / js_getprop_fallback_len; the silver shim lost its NUL-terminating
copy). Full repro table identical to node; covered by tests/test_nul_property_keys.cjs.
Both parts of this plan are complete; move to completed/ on next doc pass.

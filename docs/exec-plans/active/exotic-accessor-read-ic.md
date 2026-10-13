# Exotic Receiver and Accessor Read IC

Status: active
Last reviewed: 2026-10-12
Owner: theMackabu

## Problem

Typed arrays stopped carrying hidden own `buffer` and `BYTES_PER_ELEMENT`
properties, and ArrayBuffer/DataView instances stopped carrying hidden
`byteLength`/`byteOffset`, so those reads now resolve through prototypes. The
get IC refused both exotic receivers and accessor properties, so every such
read took the generic lookup (~45 ns per getter). Buffer method lookups paid
the same cost because a Buffer is an exotic receiver.

## Design

- Stage 0. `ant_exotic_ops_t` gains `keys` and `heap`. Typed arrays share one
  static table marked `ANT_EXOTIC_KEYS_NUMERIC`; `js_set_exotic_ops` installs
  it. `obj_ensure_exotic_ops` copies a static table into a heap table (keys
  reset to `ANY`) before any public hook setter writes. GC frees and memory
  accounting only touch heap tables. Mapped `arguments` no longer clears its
  static table pointer in its finalizer.
- Stage B. `ant_key_may_be_numeric` (internal.h) is the single prefilter: every
  `NUMERIC` hook declines keys it rejects. The C get IC admits an exotic
  receiver when its ops are `NUMERIC` and the atom fails the prefilter, checked
  at hit time on the exotic branch only. Exotic receivers never fill `MISSING`.
  The prefilter is exact for identifier-shaped atoms: names starting with `I`
  or `N` pass only as `Infinity`/`NaN`. The compiler stores the result on each
  atom (`sv_atom_t.maybe_numeric`), and the JIT passes atom pointers to its
  field helpers, so the cache and the JIT never inspect key text; only the
  typed array hooks, which see arbitrary keys, run the prefilter. In the JIT, snapshot, poly and mega
  paths still reject exotic receivers; typed arrays use the generic and
  own-slot paths, which add an exotic-flag guard only when the atom passes the
  prefilter, or fall back to the C lookup.
- Stage A. `SV_GF_IC_OWN_GETTER` and `SV_GF_IC_PROTO_GETTER` cache accessor
  hits. The getter is read from the shape entry at hit time; nothing new is
  retained. Fill happens before the call. No-effect lookups pass no getter
  slot and keep missing on accessors. The JIT compares kinds exactly, so the
  new kinds route to its helper; `sv_prop_get_field_ic` calls the getter via
  `js_call_getter`, which invokes native getters directly.
- Stage D. `length` reads are cached for `NUMERIC` exotic plain objects in C.

## Rejected

- Hidden own data properties on instances: fast, but observable through
  `getOwnPropertyNames`, spread and descriptors.
- Fill-time-only numeric checks: an entry filled by an ordinary object of the
  same shape would be hit by a typed array, so the atom is checked per hit.
- A JIT admission guard (exotic flag, ops pointer, `keys`) on snapshot, poly
  and mega paths: about 10% faster Buffer method calls, but +1% instructions on
  newt-prelude from the extra block at every ordinary site.

## Validation

- `tests/test_typedarray_named_read_ic.cjs`, `tests/test_accessor_ic.cjs`, and
  typed array/DataView/Buffer receivers in `tests/test_ic_read_differential.cjs`.
- The suspected `ta.NaN` JIT hit on a shared root shape did not reproduce
  before these changes; the named-read test guards it.

## Open

- Stage E (slot-reading intrinsics for `buffer`/`byteLength`) if accessor reads
  must reach own-data-property speed.

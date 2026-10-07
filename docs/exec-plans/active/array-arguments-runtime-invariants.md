# Array and Arguments Runtime Invariants

Status: active
Last reviewed: 2026-10-07
Owner: theMackabu

## Goal

Make the `Array.prototype` builtins and the `arguments` object follow the
spec (frozen, sealed and read-only-length arrays, generic array-likes, species,
prototype holes) while keeping the dense fast paths at least as fast as before.
This work changed invariants that the interpreter, the builtins and the JIT
all rely on, across `6f37e4b5`, `5ce71f87`, `ff765143`, `a6078a6f`,
`e5ac3bfb`, `bda9e95b`, `4c3c5803`, `e18fc303`, `c13ba358` and `4f15a028`.

## Scope

`src/ant.c` (array builtins, `arguments` creation), `src/descriptors.c`,
`src/gc/objects.c` (storage cache), `include/object.h`, `include/isolate.h`,
and the JIT guards in `src/jit/values.c` and `src/jit/emit_properties.c`.

## Invariants

### Arguments objects

- An `arguments` object is a `kTypeArray` with `flags.arguments_object` set
  (`ANT_OBJECT_FLAG_ARGUMENTS`) and `Object.prototype` as its proto. Its
  `length` is an ordinary data property, so it can be overwritten, deleted or
  redefined.
- Arguments objects are cloned from a per-strictness template. Their in-object
  slots are fixed: `ANT_ARGUMENTS_SLOT_LENGTH`, `ANT_ARGUMENTS_SLOT_CALLEE` and
  `ANT_ARGUMENTS_SLOT_ITERATOR` (`include/object.h`). `arguments_template`
  asserts the template's shape matches. The JIT reads `length` from
  `ANT_ARGUMENTS_SLOT_LENGTH` and tests the flag through
  `ANT_OBJECT_FLAGS_HIGH_BYTE`. `static_assert`s pin the flag to the high
  byte, with only the RegExp brand above it, so the JIT's byte compare stays
  valid.
- `array_length_obj_ptr` excludes arguments objects. Code that wants
  "an array whose `length` is the element count" must use it rather than
  `array_obj_ptr`.

### Prototype chain

- `array_proto_chain_plain(js, arr)` is true when `arr`'s proto is
  `Array.prototype`, `Array.prototype` has no elements, and the chain
  `Array.prototype -> Object.prototype -> null` has no exotic objects or index
  keys. The answer is cached per IC epoch in `js->array_chain_plain_epoch`.
  `array_chain_revalidate` sets `guards_absence` on both prototypes, so any
  property addition there bumps the epoch. Elements stored on
  `Array.prototype` don't change its shape, so the cache cannot cover them;
  that is why `Array.prototype`'s `length` is checked on every call.
- The JIT append guard checks the same conditions inline: the receiver's
  EXT/SEALED/FROZEN/EXOTIC flags, `proto == Array.prototype`, the epoch, and
  `Array.prototype` length.
- `array_hole_reads_proto` decides whether a hole read must walk the chain.

### Species and fresh results

- `js->array_species_protector_invalid` is set when `Symbol.species` is
  defined on `Array`. While it is clear, an array whose proto is
  `Array.prototype` creates results with the intrinsic `Array`
  (`array_constructor_from_receiver`).
- `array_species_create(js, receiver, len, &intrinsic)` reports whether the
  result is a fresh intrinsic array. Fresh results are filled with
  `array_result_set` (direct store); others go through
  `array_result_set_checked` (CreateDataPropertyOrThrow).
- `flags.may_have_holes` is set only when a hole can exist. `map`, `slice` and
  `splice` call `array_mark_packed` on fully written
  results instead of leaving them holey.

### Mutability and length

- In-place fast paths are gated by one flags mask. `pop` and `shift` use
  `array_mutable_dense` (fast, dense length fits, not frozen, sealed, exotic or
  arguments); `splice` uses its own mask (extensible, not frozen, sealed or
  exotic). A read-only `length`
  is always frozen or exotic, so it fails these masks. Everything else falls to
  the generic paths.
- A read-only `length` is represented as frozen, or exotic plus a registry
  descriptor (`js_array_make_length_readonly`, `arr_length_readonly_desc`).
  `array_length_readonly` tests the flags inline and calls the cold
  `array_length_readonly_slow` only for exotic arrays.
- Generic mutators (`array_pop_generic`, `array_shift_generic`,
  `array_splice_generic` and the generic `push`) follow spec order and throw
  through `array_set_index_throw`, `array_delete_index_throw`,
  `array_set_length_throw` and `array_move_range_throw`, which use
  `js_setprop_throw` / `js_setprop_index_throw` (strict-mode Set).
- String wrappers expose own `length` and index keys
  (`js_try_get_string_own_exotic`); array mutators throw on them
  (`array_string_wrapper_length`).
- Array-likes: `array_like_length_checked` propagates ToLength errors, and
  index reads use full Get / HasProperty.

### Storage cache

- Backing stores with a power-of-two capacity of at most 32
  (`GC_ARRAY_STORAGE_CLASSES = 6`) are returned to a per-isolate free list
  (`js->array_storage`) when an array dies. The next pointer lives in slot 0
  of the freed buffer.
- `gc_array_storage_trim` runs after each major GC. It keeps as many buffers
  per class as were taken since the previous major GC and frees the rest, so
  idle retention stays bounded.

## Decisions

- Epoch cache instead of re-walking the chain on every append: the walk was
  the dominant cost of `push` once the semantics were fixed.
- Flags tested inline with cold helpers for read-only length: the first
  version (one out-of-line predicate) made `js_setprop` and `pop` codegen
  worse; the inline mask brought `pop`/`shift` 14% below the pre-change
  baseline.
- `fresh`/intrinsic flag on species results instead of a per-store check in
  `map`: the per-store check showed up as a regression.
- Hoisting the fill loop in `alloc_array_with_proto_capacity` lets it lower to
  a `memset`, which recovered the exact-capacity allocation regression.

## Validation

- Tests: `tests/test_array_builtin_fast_paths.cjs` (sections including
  `speciesresult`, `packedresults`, `arraylikes`, `protochain`),
  `tests/test_array_readonly_length.cjs`, `tests/test_array_storage_reuse.cjs`,
  `tests/test_arguments_object_semantics.cjs`,
  `tests/test_arguments_length_only.cjs`. Expected values were generated with
  Node.
- Measured by instruction count, no-PGO builds:
  - JIT append guard on the epoch cache: `push` -11%, append -14%, hole
    fill -19% per 64 stores.
  - storage cache: `[]` 22.1 -> 14.6 ns.
  - `slice`: 3,931 -> 1,162.
  - `pop`/`shift` mask fast paths: -14%.
  - array helpers against `origin/master`: 1.4-7.5x faster.

## Follow-ups

- The per-store `Array.prototype` length check in the append guard could be
  folded into the epoch if element writes on `Array.prototype` bumped it.
- `splice` on proxies goes through the generic path; it was not specifically
  measured.
- `reverse`, `copyWithin`, `fill` and `unshift` on array-likes and frozen
  arrays have not been audited against Node.
- Assigning to an index of a String wrapper has not been verified against Node.

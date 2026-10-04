# Polymorphic Property Reads and Object Literal Allocation

Status: completed (merged as `be1aa867`, #121; follow-ups through 2026-10-04)
Last reviewed: 2026-10-04
Owner: theMackabu

The detailed working log (per-stage measurements, review rounds) is in this
file at `be1aa867`.

## Outcome

Named property reads, constructor calls and object literals no longer leave
compiled code on the common paths that bench-v8 exposed. With PGO builds
trained on their own code, against master `9d61d90d`: bench-v8 RayTrace
+13%, EarleyBoyer +3%, Richards +2.5%, the rest within noise; fixed-work
RayTrace -9% time and -15% instructions. GC fixtures and apps level or faster.

## What landed

- **Polymorphic reads** (`sv_gf_poly_t`): a read site keeps up to 8 more own
  or prototype cases, validated like its own case; a hit swaps the case into
  the site's own slot, so the inline check sees the common case. More than 16
  new cases within one epoch makes the site megamorphic. Compiled code loops
  over the cases inline at sites already polymorphic when compiled, in
  functions up to 512 bytecode bytes (`JIT_GFP_MAX_CODE_LEN`).
- **Megamorphic reads** (`sv_gf_mega_cache_t`): an isolate-wide cache keyed by
  shape, prototype and key, 4096 primary and 1024 secondary entries, probed
  inline; a function whose site went megamorphic after compiling recompiles
  once (at most twice per function).
- **Polymorphic stores** (`sv_pf_poly_t`): existing-property and add cases,
  with an inline write barrier for references stored into old objects. Add
  cases are keyed by the receiver's prototype (`sv_add_proto_key`): its value
  when old (one compare inline), its identity tagged odd when young (checked
  in C, rekeyed by value once promoted).
- **Allocation**: inline object literal stores (functions up to 1024 bytecode
  bytes), inline shape transitions, a per-function cache of the constructor's
  `prototype` slot, and direct calls of compiled constructors from compiled
  `new` (`jit_emit_new_direct`, functions up to 1024 bytecode bytes,
  `JIT_NEW_DIRECT_MAX_CODE_LEN`).
- **Strict mode in compiled code**: store and delete helpers take the
  compiled function's mode, since compiled frames push no VM frame (a bug
  that predated this work).

## Key decisions

- **C re-validates, the JIT trusts the epoch.** On a cached hit the C lookup
  re-reads the holder's current property metadata; the compiled loop and
  probe load the slot after the shape, prototype and epoch checks. Anything
  that changes metadata in place must therefore bump the IC epoch.
  `tests/test_ic_read_differential.cjs` compares both against an uncached
  lookup; removing the epoch check from the compiled loop makes it fail.
- **Size gates** keep compile cost bounded: inline poly loops over 512 bytes,
  inline literal stores and direct `new` over 1024 bytes fall back to
  helpers. Direct `new` in a 1,500-site function peaked at 1,992 MB against
  842 MB without it; 1024 bytes holds about 90 sites, and bench-v8 and Newt
  ran the same instructions with the cap.
- **Majors release what epochs retire**: after each major the megamorphic
  cache (`sv_gf_mega_clear`) and the per-site polymorphic cases
  (`sv_ic_polys_release_shapes`) drop their shape references, since the
  epoch bump makes them miss anyway; Newt Prelude ran identical instructions.
  Single-case IC shapes stay: they share the reference registry with JIT
  snapshots whose addresses compiled code embeds.
- **Identities stay unique**: when the prototype identity counter wraps, every
  live object's identity is cleared (`sv_ic_identities_reset`, `572ec066`).

## Rejected

- An unrolled 8-way inline probe at every polymorphic site (`rayTrace`
  compile 102 -> 378 ms, up to +120 MB).
- A helper call at every read site, and checking only the cases present at
  compile time (lost the gains).
- Hits that did not promote (DeltaBlue +26% instructions).
- Recompiling for merely polymorphic sites; inline literal stores in all
  functions (Newt tokenizer +33 ms, +24 MB).
- Comparing prototype identities inline for adds (RayTrace +2.7%
  instructions); recording only old prototypes (slow adds until the first
  minor).

## Validation

`test_poly_field_ic`, `test_poly_field_store`, `test_ic_read_differential`,
`test_jit_new_direct`, `test_jit_define_slot`, `test_new_prototype_cache`,
`test_jit_store_barrier`, `test_jit_strict_mode` (JIT, `--jitless`, node);
native `ic-identity-wrap` and `ic-poly-shape-release`. Each guard test was
checked against a mutant that removes its guard.

## Limitations and follow-ups

- The cached-read check is still written separately in C and in the JIT
  emitters; the differential test is the guard against drift.
- Fixed-work DeltaBlue and Crypto ran 3-5% more cycles than master under PGO
  with level instructions (layout); not investigated.
- The tracked PGO profile predates this work and needs regenerating.

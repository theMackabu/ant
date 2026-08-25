# Hash Seed Randomization

Status: active
Last reviewed: 2026-07-31
Owner: theMackabu

## Goal

Decide whether `hash_key` needs a per-process random secret, and if so, resolve the
conflict with the one place its output is persisted.

## The exposure

`include/hash.h` uses rapidhash's published default secrets:

```c
static const uint64_t ant_hash_secret[8] = {
  0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL, ...
};
```

Fixed secrets make rapidhash and wyhash vulnerable to *seed-independent* collisions: because
the constants are known, an attacker can construct inputs whose mixing state cancels out,
producing unlimited colliding keys without needing to observe the process. The published
technique sets bytes near the end of the input to the secret XORed with the length.

  <https://liams.website/articles/seed-independent-collisions-on-wyhash-and-rapidhash>

`hash_key` feeds the string intern table, which every property name goes through. An
attacker who controls key names -- a JSON request body, query parameters, user-supplied
object keys -- can force every key into one bucket and turn property access into a linear
scan. That is the classic HashDoS shape, and for a server runtime it is reachable from
ordinary request handling.

## What this is not

**Not a regression, and not caused by adopting rapidhash.** The previous FNV-based
`hash_key` was more trivially attackable: it had no avalanche at all in the low bits, so
colliding keys could be found by inspection rather than by algebra. Anything that fixes this
had to happen regardless of which hash is in place. The rapidhash port made the hash
*better* on every axis; it did not introduce the exposure.

The measured severity of the old weakness is on record: prefix-sharing keys reached an
average probe length of 710 and a maximum chain of 10,000 at 200k keys. Randomizing the
secret is about the adversarial case, not the accidental one -- the accidental one is
already fixed.

## The blocker

`hash_key` output is **persisted**. `esm_get_cache_path` (`src/esm/remote.c`) names remote
module cache entries after it:

```c
snprintf(suffix, sizeof(suffix), "remote/%016llx", (unsigned long long)hash);
```

and the same value is written into `metadata.bin`. A per-process random secret would give a
different filename on every run: every cached remote module would miss, be refetched, and
the old entries would accumulate as unreachable files.

So randomization cannot simply be switched on. The two uses have to be separated first.

## Task list

1. **Split the persisted use from the in-memory one.** Give `esm_get_cache_path` and the
   `metadata.bin` writer their own explicitly-stable hash, documented as on-disk format and
   never to be tuned. This is worth doing on its own merits even if randomization is
   declined -- right now any future change to `hash_key` silently orphans a user's cache.
   An earlier attempt at exactly this was reverted as unnecessary duplication; it is not,
   and this is the reason.
2. **Audit the other `hash_key` callers** for persistence or cross-process assumptions:
   `descriptors.c`, `silver/compiler.c`, and the intern table itself. In-memory only is the
   expectation; confirm it.
3. **Decide the seed source.** Per-process from the OS CSPRNG at isolate init is the usual
   answer. Note the bootstrap snapshot machinery (`meson/builtins`) may bake tables into the binary --
   check whether a snapshot restores intern-table state, because a snapshot taken under one
   seed cannot be restored under another.
4. **Measure.** The seed becomes a runtime value rather than a compile-time constant, so the
   compiler can no longer fold it. The standalone harness that produced the current numbers
   (1.3-2.4x faster than the old hash, avalanche bias 0.0302) should be re-run against a
   runtime-seeded variant before committing.
5. **Then decide.** If step 4 shows a material cost, the alternative is bounding the damage
   instead of preventing it: cap intern-table chain length and rehash with a new seed when a
   bucket exceeds it. That keeps the fast path unchanged and only pays under attack.

## Decision log

- Not treated as a release blocker. The exposure predates the rapidhash port and the port
  strictly improved matters; treating it as newly-introduced would misstate the history.
- Step 1 is worth landing independently of the rest. The `hash_key`-names-cache-files
  coupling is a latent trap for any future tuning, separate from any security argument.

## Validation status

- Exposure not demonstrated against ant. No collision-generating input has been constructed
  for this specific secret set and table implementation; the claim rests on the published
  technique applying to the default secrets, which ant uses unmodified.
- Constructing an actual colliding key set against ant's intern table would be the honest
  way to size this, and has not been done.

## Follow-ups

- `include/hash.h` already carries a comment noting that changing it orphans remote cache
  entries. If step 1 lands, that comment moves to the new stable hash and `hash.h` loses the
  constraint entirely.

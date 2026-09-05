# WebCrypto secret-key compatibility

Status: implementation pending validation
Owner: theMackabu

## Scope

Add AES-GCM/HMAC `generateKey` and raw `exportKey` in `src/modules/crypto.c`.
Imported keys retain native extractability and usages. PR #44 and `/tmp/ant`
are historical references, not patches to transplant.

## Decisions

- Normalize supported algorithms, integer lengths, and iterable usages at the
  WebCrypto boundary; do not accept OpenSSL-only hash aliases for HMAC keys.
- Keep native permissions separate from mutable JavaScript metadata.
- Use one owned key allocation for import or generation. Snapshot imported
  BufferSource bytes before algorithm getters; cleanse native storage on failure
  and finalization. Raw exports are independent copies.
- Share promise settlement and explicitly root the promise across conversion
  callbacks. Consume pending exceptions when rejecting, preserving the rejection
  reason instead of taking and rethrowing it through JavaScript accessors.
- Keep PBKDF2 import non-extractable. No new algorithms, formats, or wrapping
  operations are included.
- Use the existing `js_iter` without changes to shared iterator behavior.
  Full sequence getter semantics, cached `next`, error typing, and exception
  precedence during iterator closing are deferred, with a TODO at the call site.
  Regression coverage does not claim support for those deferred edge cases.

Behavior reference: [Web Cryptography Level 2](https://www.w3.org/TR/webcrypto/).

## Validation status

The user's rebuilt binary exposed an inherited-`next` lookup failure during the
benchmark pilot. Crypto now uses the existing shared iterator helper instead of
its own loop. This follow-up has not been rebuilt or executed; tests and preflight
remain pending approval. The pilot produced no valid A/B performance comparison.

After approval: run `maid preflight`, build the configured tree, then run
`tests/test_webcrypto_generate_export.cjs` and existing crypto-focused coverage.
This work is not yet verified to compile or pass tests.

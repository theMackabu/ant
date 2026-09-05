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

Behavior reference: [Web Cryptography Level 2](https://www.w3.org/TR/webcrypto/).

## Validation status

Source review and regression cases added; no builds, tests, probes, or preflight
have been run for this cleanup. The user requires approval before execution.

After approval: run `maid preflight`, build the configured tree, then run
`tests/test_webcrypto_generate_export.cjs` and existing crypto-focused coverage.
This work is not yet verified to compile or pass tests.

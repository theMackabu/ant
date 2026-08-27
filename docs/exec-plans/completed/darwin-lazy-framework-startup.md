# Darwin Lazy Framework Startup

Status: completed
Last reviewed: 2026-08-26
Owner: theMackabu

## Goal

Remove fixed macOS process-startup cost from platform frameworks that Ant only
needs for sandbox virtual machines or HTTPS system-trust checks.

## Scope

- Preserve Hypervisor-backed sandbox behavior and entitlements.
- Preserve macOS system trust validation and invalid-certificate rejection.
- Keep the tlsuv change in its tracked Meson patch-directory overlay.
- Compare pinned PGO/LTO binaries with serial, non-overlapping runs.

## Decisions

- Resolve Hypervisor.framework entrypoints once when the first sandbox session
  is created. Ordinary Ant processes do not link or load the framework.
- The final Security and CoreFoundation imports came from tlsuv's BoringSSL
  system-trust callback. The compiled Apple keychain object was dead-stripped
  and did not contribute imports to the Ant executable.
- Resolve the eight trust functions and the CoreFoundation array callbacks once
  when the first system-trust check runs. Keep the frameworks loaded for the
  process lifetime.
- The resulting Ant executable directly links only libSystem and libc++.

## Performance

The baseline was `/tmp/ant-security-lazy/ant-security-eager`, SHA-256
`46224c4f0cdc184178bb1a857481a5ed4fed8493d85a4e6877f32bf3e1acf489`.
The candidate was `/tmp/ant-security-lazy/ant-security-lazy`, SHA-256
`a059f15a4ca705f54dd56ebf614d3d1882cf28c997f6ed2fbd5b816fc57e165d`.
Both were release PGO/LTO builds.

Three interleaved AB/BA rounds used `hyperfine -N`, 15 warmups, and 100 runs.
The mean of the three round medians was:

- Empty execution: 5.01 ms to 3.78 ms, a 24.5% reduction.
- Hono cold start: 7.46 ms to 6.05 ms, an 18.9% reduction.

## Validation

- Reconfigured and compiled the release PGO/LTO build.
- Confirmed with Mach-O and dyld inspection that ordinary execution loads none
  of Hypervisor, Security, or CoreFoundation, while sandbox and HTTPS paths
  load their required frameworks on demand.
- Confirmed a valid HTTPS request returns 200 and an expired certificate is
  rejected.
- Passed the tlsuv BoringSSL smoke test, WebSocket cancellation test, sandbox
  message test, startup-time regression test, and test manifest.
- Passed all 3,972 specification tests and `maid preflight`.

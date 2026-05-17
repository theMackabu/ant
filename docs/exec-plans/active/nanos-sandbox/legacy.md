# Nanos Sandbox: Legacy Things To Remove And Rewrite

Status: active
Last reviewed: 2026-05-16
Owner: theMackabu

## Request Transport Cleanup

Remove or avoid these transport paths as the default model:

- request files
- stdin probing
- newline-delimited JSON
- TCP control channels
- temp-image argument rewrites
- per-script image patching

The daemon should consume requests only from an explicit backend transport.

JSON payloads are acceptable during bring-up, but JSON should not become the
permanent ABI. The long-term transport should be a compact custom serializable
binary stream with typed frames.

## Console Cleanup

Normal sandbox stdout/stderr should stop flowing through the emulated machine
console. PL011/console should remain attached only for:

- boot logs
- kernel panics
- daemon startup failures
- low-level diagnostics

Normal program output should use the daemon protocol frames.

## Debug Flag Cleanup

`ANT_SANDBOX_VM_TRACE` is a temporary bringup aid. It produces raw
MMIO/queue-level noise and should be removed or hidden once the backend is more
stable.

Replace it with a user-facing `ant sandbox --verbose` mode that feels more like
macOS verbose boot: high-signal boot/device/protocol milestones without raw
device spam.

## Image Layout Cleanup

The direct `ops build` MBR image is not the final shipped artifact. It includes
a mostly empty fixed Nanos/kernel partition that costs about 12 MiB.

The final sandbox image should remain the shrunk raw-root TFS image paired with
the matching patched Nanos kernel.

Avoid local output variants such as stable plus versioned copies in `nanos/out`.
Keep one local filename per arch and put version isolation in the runtime cache.

## ops Quirk Cleanup

The local image builder still works around ops behavior while composing the
image. Any workaround that depends on ops filename arch detection or transient
ops state should stay isolated to the build script and should not leak into the
runtime cache or host CLI.

Runtime lookup should not scan old ops paths. It should use only:

- the versioned Ant sandbox cache
- explicit `ANT_SANDBOX_IMAGE`
- explicit `ANT_SANDBOX_KERNEL`

## Network Cleanup

Any drop-complete-only virtio-net behavior is a bringup shim, not the final
network backend. Replace it with a real host network backend before calling
network support done.

Also verify the doc and code agree on the current state: local smoke output may
still show `NET: no network interface found` unless network emulation is enabled
or completed.

## Mount Path Cleanup

`/workspace` is the MVP guest path, not necessarily the final mount ABI.

Arbitrary guest paths such as `Sandbox({ mount: ".:/project" })` should not be
pretended to work until the daemon or Nanos mount layer has a real design for
aliases, dynamic mounts, or stable mount roots.

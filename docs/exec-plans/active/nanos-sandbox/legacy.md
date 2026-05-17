# Nanos Sandbox: Legacy Things To Remove And Rewrite

Status: active
Last reviewed: 2026-05-17
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

## Darwin Backend Stubs And Bringup Shims

`src/sandbox/backends/darwin/` is a working bringup backend, but several
pieces are intentionally minimal and should not be treated as finished APIs:

- Timer delivery is better but not final. The backend now raises the correct
  redistributor PPI and uses `cntvct_el0` for deadline math, but the patched
  Nanos `CNTFRQ_EL0` fallback is still bringup scaffolding.
- The MSI path is modern virtio PCI MSI-X now, but the machine still exposes a
  GICv2M-compatible MSI frame rather than a full ITS. Keep that contract honest:
  use the V2M SPI window deliberately, or replace it with a real ITS model and a
  matching Nanos patch.
- Virtio-net now has opt-in vmnet shared-mode plumbing behind
  `ANT_SANDBOX_VM_NET=1`, but the local ad-hoc binary cannot currently start it:
  adding `com.apple.vm.networking` to the local entitlements makes macOS kill
  the executable before `main`, while omitting it makes `vmnet_start_interface`
  fail with status `1001`. Treat network as blocked on the signing/helper
  decision, not done.
- Virtio-vsock is only a first request pipe. It sends one newline-terminated
  JSON request after the guest connects, ignores the event queue, and advances
  flow-control counters for guest RW packets without parsing stdout/stderr,
  result, error, or exit frames.
- Virtio-9p is a small read-mostly server for the current workspace. It handles
  attach, walk, getattr, lopen, read, readdir, statfs, and clunk, while other
  9p messages return `ENOSYS`. Writes, mutation, stronger path policy, dynamic
  mount tags, and arbitrary guest mount aliases are still future work.
- Virtio-blk is modern virtio PCI, but it is still only enough for the raw root
  image. It uses a tiny fixed queue, direct `pread`/`pwrite`, sparse config
  handling, and should be reviewed before relying on flush/discard/write-zero
  behavior.
- PCI config space is handcrafted for the current modern virtio devices. It
  exposes the required capabilities and MSI-X tables, but it is not a full PCI
  root-complex model and still swallows unsupported config/PIO accesses.
- PL011/UART is an output/debug path, not the final sandbox stdio transport.
  It should become boot log and panic plumbing once daemon frames own normal
  stdout/stderr.
- RTC and miscellaneous MMIO devices are minimal compatibility shims. RTC reads
  host wall time and ignores writes; unknown devices should become explicit
  models or explicit failures as the machine contract stabilizes.
- Shutdown and error mapping are incomplete. PSCI/semihosting exits can stop
  the VM, but the host does not yet reliably distinguish script exceptions,
  guest daemon errors, kernel panics, transport errors, and clean exits.
- Environment flags such as `ANT_SANDBOX_VM_TRACE`,
  `ANT_SANDBOX_VM_TRACE_9P_PATHS`, `ANT_SANDBOX_VM_TRACE_9P_READDIR`,
  `ANT_SANDBOX_VM_TIMEOUT_MS`, and `ANT_SANDBOX_VM_NET` are bringup controls.
  Keep or replace them deliberately before exposing a stable CLI.
- The backend is Apple Silicon only. The non-aarch64 Darwin path is a hard
  `ENOSYS` stub.

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

The old drop-complete-only virtio-net behavior has been replaced with vmnet
shared-mode queue plumbing, but network support is not done until a signed or
otherwise unentitled backend can actually start on user machines.

Also verify the doc and code agree on the current state: local smoke output may
still show `NET: no network interface found` unless network emulation is enabled
or completed. `ANT_SANDBOX_VM_NET=1` currently fails early without the vmnet
entitlement.

## Mount Path Cleanup

`/workspace` is the MVP guest path, not necessarily the final mount ABI.

Arbitrary guest paths such as `Sandbox({ mount: ".:/project" })` should not be
pretended to work until the daemon or Nanos mount layer has a real design for
aliases, dynamic mounts, or stable mount roots.

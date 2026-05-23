# Nanos Sandbox: Legacy Things To Remove And Rewrite

Status: active
Last reviewed: 2026-05-23
Owner: theMackabu

## Darwin Backend Stubs And Bringup Shims

`src/sandbox/backends/darwin/` is a working bringup backend, but several
pieces are intentionally minimal and should not be treated as finished APIs:

- Timer delivery is better but not final. The backend now raises the correct
  redistributor PPI and uses `cntvct_el0` for deadline math, but the patched
  Nanos `CNTFRQ_EL0` fallback is still bringup scaffolding.
- Virtio-9p now supports explicit writable mounts, but its path policy and
  mutation surface should continue to be reviewed as more workflows depend on
  host-backed writes.
- Virtio-blk is modern virtio PCI, but it is still only enough for the raw root
  image. It uses a tiny fixed queue, direct `pread`/`pwrite`, sparse config
  handling, and should be reviewed before relying on flush/discard/write-zero
  behavior.
- PCI config space is handcrafted for the current modern virtio devices. It
  exposes the required capabilities and MSI-X tables, but it is not a full PCI
  root-complex model and still swallows unsupported config/PIO accesses.
- RTC and miscellaneous MMIO devices are minimal compatibility shims. RTC reads
  host wall time and ignores writes; unknown devices should become explicit
  models or explicit failures as the machine contract stabilizes.
- Shutdown is still minimal. PSCI/semihosting exits and explicit timeout policy
  can stop the VM, but API-level cancellation, forced teardown, and graceful
  long-running session lifecycle still need a deliberate design.
- The backend is Apple Silicon only. The non-aarch64 Darwin path is a hard
  `ENOSYS` stub.

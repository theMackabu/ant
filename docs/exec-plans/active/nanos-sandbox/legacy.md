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
- Shutdown is still minimal. PSCI/semihosting exits can stop the VM, but
  cancellation, forced teardown, and long-running lifecycle policy still need a
  deliberate design.
- Internal/debug environment flags such as `ANT_SANDBOX_VM_TRACE_9P_PATHS` and
  `ANT_SANDBOX_VM_TRACE_9P_READDIR` are bringup controls. Keep them hidden or
  replace them deliberately before exposing a stable CLI.
- Remove the scoped 9p trace flags after module-loading and directory-walk
  performance is fixed; they are temporary probes for the current loading perf
  work, not a stable diagnostics API.
- The backend is Apple Silicon only. The non-aarch64 Darwin path is a hard
  `ENOSYS` stub.

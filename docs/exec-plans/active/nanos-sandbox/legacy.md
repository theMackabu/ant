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
- PCI config space now has explicit modern virtio config handling for the
  current device set, including BAR sizing, command masking, MSI-X control
  masking, and absent-device reads. It is still not a full PCI root-complex
  model, and unsupported PIO accesses are still minimal compatibility shims.
- RTC now has an explicit PL031 subset for the registers Nanos uses, and
  unsupported RTC/unknown MMIO accesses fail with diagnostics instead of
  silently returning zero. Unsupported PCI PIO accesses are still minimal
  compatibility shims.
- Shutdown is still minimal. PSCI/semihosting exits and explicit timeout policy
  can stop the VM, but API-level cancellation, forced teardown, and graceful
  long-running session lifecycle still need a deliberate design.
- The backend is Apple Silicon only. The non-aarch64 Darwin path is a hard
  `ENOSYS` stub.

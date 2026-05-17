# Nanos Sandbox: Things Done

Status: active
Last reviewed: 2026-05-17
Owner: theMackabu

## Goal Shape

The sandbox feature is aimed at running JavaScript inside a hardware-isolated
Nanos unikernel using one generic Ant image per Ant version. The image is not
rebuilt or patched for each script.

The long-term module shape is:

```js
import { Sandbox } from "ant:sandbox";

const sb = new Sandbox({ mount: ".:/workspace" });
await sb.run("script.js");
await sb.eval("export default 1 + 1");
await sb.close();
```

The CLI shape is:

```sh
ant sandbox script.js
```

## Generic Image

The generic Nanos image boots Ant in daemon mode:

```sh
/ant --sandbox-daemon
```

The host-side `ant sandbox` command is responsible for:

- finding or downloading the cached image and kernel
- launching a Nanos-compatible backend
- passing requests through the backend transport
- streaming stdout, stderr, structured results, and exit status back to the
  host process

The dynamic part is the host runner plus the guest daemon protocol.

## Cache Layout

The local builder and host CLI use an Ant-version-isolated sandbox cache:

- If legacy `~/.ant` exists: `~/.ant/sandbox/<ant-version>/`
- Otherwise: `$XDG_CACHE_HOME/ant/sandbox/<ant-version>/` or
  `~/.cache/ant/sandbox/<ant-version>/`
- `ant-sandbox-<arch>.img`
- `ant-kernel-<arch>.img`

`ant sandbox <script>` reads from that versioned cache unless
`ANT_SANDBOX_IMAGE` or `ANT_SANDBOX_KERNEL` explicitly points at an override.

Local build scratch state stays inside the repository:

- `nanos/.cache/nanos/` for the patched Nanos checkout and kernel build
- `nanos/.cache/ops-config.*` for transient ops state while composing the image

Local build outputs stay boring and singular:

- `nanos/out/<arch>/ant-sandbox-<arch>.img`
- `nanos/out/<arch>/ant-kernel-<arch>.img`

Do not add parallel stable/versioned local image names. Version isolation
belongs in the runtime cache directory, not in `nanos/out`.

## Local Image Build

`nanos/build-sandbox.sh` builds the Ant musl binary, builds a patched Nanos
kernel, composes the sandbox image, shrinks it, and copies the resulting image
and kernel into the versioned sandbox cache.

The script also supports reusing an existing Ant musl binary while rebuilding
only Nanos/image pieces:

```sh
./nanos/build-sandbox.sh --skip-docker
```

Local ARM image facts observed on 2026-05-16:

- Direct `ops build` output is a raw MBR image with two Nanos TFS partitions.
- Direct `ops build` output is about 24 MB.
- Partition 1 is a fixed Nanos/kernel area of about 12 MiB and mostly zeros.
- Partition 2 contains the Ant app payload of about 11.3 MiB.
- The sandbox build shrinks the final shipped image to the raw root TFS
  partition and relies on the paired patched Nanos kernel to mount that raw TFS
  as `/`.
- Embedded Ant ELF is already stripped; the remaining bulk is real `.text`,
  `.rodata`, and unwind data.

## CI Image Build

CI builds the patched kernel with `.github/workflows/build-nanos-kernel.yml`.
That workflow is manually runnable, applies `nanos/patches/*.patch` to a fresh
Nanos checkout, reads `ant.version` from `meson/ant.version`, and uploads both
stable and versioned kernel artifacts.

The reusable Nanos image workflow shrinks its `ops build` output before upload,
so consumers receive the smaller raw-root image instead of the padded MBR image.

## Current Nanos Patches

Current local Nanos patches are saved under `nanos/patches/` so they can be
replayed in local builds, CI, or a fork later.

Known patch themes:

- keep aarch64 user pointers below Ant's 47-bit NaN-boxing ceiling
- read architected timer frequency from devicetree when `CNTFRQ_EL0` is zero
- refresh stale 9p readdir children
- fix `writev` handling
- make stack growth probing report `ENOMEM` correctly
- increase process stack size for Ant
- allow a raw TFS image to boot as the root filesystem

The low-user-VA patch is intended for the Ant sandbox image, not as a casual
global Nanos default. It keeps aarch64 user pointers below Ant's 47-bit
NaN-boxing ceiling while still leaving 128 TiB of user VA, but it also reduces
ASLR/address-space width for other workloads.

## macOS Backend Progress

The first macOS backend lives under `src/sandbox/backends/darwin.c` and uses
Hypervisor.framework directly.

References cloned for this work:

- `/tmp/qemu`: QEMU `virt` machine reference for GICv2M, PCI, virtio, MSI/MSI-X,
  and device-model behavior
- `/tmp/hypervisor-framework`: small Rust wrapper showing Apple Silicon VM,
  memory map, and vCPU lifecycle
- `/tmp/libkrun`: production ARM64 HVF reference for GIC/FDT/virtio-mmio style
  wiring
- `/tmp/xhyve` and `/tmp/hyperkit`: useful historical HVF references, mostly
  Intel-era and less directly useful for Apple Silicon

The backend currently creates the VM, maps guest RAM, creates the GIC/vCPU,
loads the cached Nanos kernel, provides a boot FDT, and emulates enough
UART/RTC/PCI/modern virtio surface to boot the cached image from the versioned
sandbox cache.

Known working pieces:

- cached kernel/image lookup
- Nanos guest output
- modern virtio-blk reads from the cached image
- slot 0 PCI enumeration fix
- local Darwin ad-hoc signing with `meson/ant.entitlements`
- basic `ant sandbox examples/demo/welcome.js` execution with the shrunk raw-root
  image and paired patched kernel

The slot 0 PCI enumeration bug was slot 0 reporting header type `0xff`, which
made Nanos take the multi-host-controller path and skip normal bus 0 probing.
Slot 0 now reports a single-controller header while remaining absent.

HVF does not allow the backend to set `CNTFRQ_EL0`, so the generated FDT
advertises the host counter through `/timer/clock-frequency` and the local Nanos
patch reads that value when the architectural register is zero.

## HVF GIC, Timer, And MSI-X Cleanup

The Darwin backend now requires Hypervisor.framework GIC MSI support during VM
creation and routes sandbox virtio devices through modern virtio PCI MSI-X
instead of legacy INTx/SPI completion interrupts.

Completed cleanup:

- use Hypervisor.framework's legal SPI interrupt range for MSI setup instead of
  trying to claim LPI-style interrupt ID `8192`
- align the provisional GICv2M MSI frame with QEMU virt's `0x08020000` frame and
  SPI base `48`
- add a tiny V2M-style MSI frame model that can read `TYPER`/`IIDR` and route
  `SETSPI_NS` writes through `hv_gic_send_msi`
- patch local Nanos so GICv3 without ITS allocates MSI vectors from the V2M SPI
  window instead of LPI IDs
- expose virtio-blk, virtio-net, virtio-9p, and virtio-vsock as modern virtio
  PCI devices with common/notify/ISR/device capabilities
- expose MSI-X tables for all sandbox virtio devices and deliver queue
  completions through `hv_gic_send_msi`
- remove the legacy virtio PCI register path (`QUEUE_PFN`, shared ISR, and
  per-device INTx/SPI completion interrupts)
- fix the local GIC redistributor register type from 16 bits to 32 bits so
  `GICR_ISPENDR0` is not truncated
- compare vtimer deadlines against `cntvct_el0`, not `mach_absolute_time()`
- assert the HVF IRQ line when raising the virtual timer PPI, matching the
  reference shape in libkrun
- remove the `ANT_SANDBOX_VM_WAKE_MS` polling thread now that the allowed
  sandbox smoke tests do not need periodic host-side wakeups

Validated after this cleanup:

```sh
./build/ant sandbox examples/demo/advanced.js
./build/ant sandbox examples/demo/wasm.js
./build/ant sandbox examples/demo/pi.js
```

## Virtio-Net vmnet Groundwork

The Darwin backend has opt-in virtio-net host plumbing behind:

```sh
ANT_SANDBOX_VM_NET=1 ant sandbox script.js
```

Implemented groundwork:

- link the Darwin backend against `vmnet.framework`
- keep vmnet off by default so normal sandbox runs still boot without a network
  device
- start a vmnet shared-mode interface when `ANT_SANDBOX_VM_NET=1`
- expose the vmnet-provided MAC through modern virtio-net config space
- pass guest TX Ethernet frames to `vmnet_write()`
- queue host RX packets from the vmnet event callback
- preserve guest RX descriptors until a real packet is available instead of
  consuming buffers and dropping future RX delivery
- wake the vCPU when vmnet packets arrive so the guest RX ring can be drained
- stop the vmnet interface during backend teardown

Validation after adding this groundwork:

```sh
./build/ant sandbox examples/demo/advanced.js
./build/ant sandbox examples/demo/wasm.js
./build/ant sandbox examples/demo/pi.js
```

Known blocker: local ad-hoc signing cannot simply add
`com.apple.vm.networking`. With that entitlement present, macOS kills the binary
before `main`; without it, `ANT_SANDBOX_VM_NET=1` reaches vmnet and fails with
status `1001`.

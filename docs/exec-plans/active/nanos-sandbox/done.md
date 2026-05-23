# Nanos Sandbox: Things Done

Status: active
Last reviewed: 2026-05-23
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

## Initial `ant:sandbox` Module

`ant:sandbox` now exports a native `Sandbox` constructor:

```js
import { Sandbox } from "ant:sandbox";

const sb = new Sandbox({ mount: ".:/workspace" });
await sb.eval("export default 1 + 1");
await sb.run("examples/demo/pi.js");
await sb.close();
```

The module shares the host-side sandbox launch parser with the CLI, including
cached image/kernel resolution, repeatable read-only mounts, explicit writable
mounts, forwarded ports, guest cwd, verbose mode, and TTY/color capability bits.
`run()` and `eval()` use the custom binary frame protocol and return promises
that resolve with the guest exit code/result or reject when the VM/request
fails.

`examples/demo/sandbox.js` is the public smoke example for this API. It exercises
both `eval` and `run` through the module.

## Persistent `ant:sandbox` Sessions

`Sandbox` instances now keep one VM session alive across multiple requests:

```js
import { Sandbox } from "ant:sandbox";

const sb = new Sandbox({ mount: ".:/workspace" });
console.log(await sb.eval("export default 1 + 1"));
await sb.run("examples/demo/pi.js");
await sb.close();
```

The host VM layer exposes create/execute/destroy session operations. The Darwin
backend creates the HVF VM once, keeps the vCPU/device model alive, sends one
request frame per `run()`/`eval()` call, and waits for that call's
result/error/exit frames before returning to the caller.

The guest daemon now installs framed stdout/stderr once and then loops over
request frames on the same virtio-vsock stream. `close()` sends a typed close
frame so the daemon can acknowledge and exit cleanly before the host tears down
the VM.

Current caveat: the JavaScript API remains synchronous internally, so a
long-running guest request occupies the VM until it exits. Parallel request
queueing and cancellation are still future API work, but repeated sequential
requests no longer reboot Nanos.

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
- support dynamic virtio-9p mount tags with caller-selected guest paths
- merge dynamic 9p mountpoints with boot manifest mountpoints so later boot
  config does not erase mounts learned from virtio device tags

The low-user-VA patch is intended for the Ant sandbox image, not as a casual
global Nanos default. It keeps aarch64 user pointers below Ant's 47-bit
NaN-boxing ceiling while still leaving 128 TiB of user VA, but it also reduces
ASLR/address-space width for other workloads.

## macOS Backend Progress

The first macOS backend lives under `src/sandbox/backends/darwin/` and uses
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

## Darwin Backend Source Split

The macOS backend was split out of the single 3k-line `darwin.c` into focused
translation units under `src/sandbox/backends/darwin/`.

Current layout:

- `backend.c`: top-level backend entrypoint, VM lifecycle, and vCPU run loop
- `common.c`: byte helpers, guest memory access, file checks, and kernel loading
- `fdt.c`: generated Nanos boot device tree
- `gic.c`: Hypervisor.framework GIC setup, timer handling, and MSI setup
- `mmio.c`: top-level MMIO dispatch and exception advancement
- `pci.c`: PCI ECAM/config-space model
- `virtio.c`: shared modern virtio PCI/MSI-X/common-config handling
- `virtio_blk.c`, `virtio_net.c`, `virtio_vsock.c`, `virtio_9p.c`: device
  implementations
- `uart.c`: PL011 output buffering
- `backend.h`: Darwin backend-private machine types and cross-file prototypes

Validation after the split:

```sh
meson compile -C build
./build/ant sandbox examples/demo/advanced.js
./build/ant sandbox examples/demo/wasm.js
./build/ant sandbox examples/demo/pi.js
```

## Workspace Mounts And Dynamic Guest Paths

The CLI now mounts the host current working directory read-only at `/workspace`
by default:

```sh
ant sandbox script.js
```

Default behavior:

- host cwd is exposed through virtio-9p
- guest cwd is `/workspace`
- the guest daemon runs the requested script from that cwd

The CLI also accepts repeatable explicit read-only mounts:

```sh
ant sandbox --mount .:/workspace script.js
ant sandbox --mount .:/project script.js
ant sandbox --mount .:/workspace --mount ./fixtures:/fixtures script.js
```

The host backend exposes one virtio-9p device per mount. Each device tag carries
the Nanos attach ID plus the requested guest mount path, for example:

```txt
0:/workspace:ro
1:/fixtures:ro
```

The local Nanos patch parses those tags, records the caller-selected mount path,
creates the mountpoint inside the raw TFS root, and mounts the corresponding
virtio-9p volume there. Nanos boot config can still provide ordinary manifest
mounts; `storage_set_mountpoints()` now merges new mountpoints into the existing
table so dynamic virtio-tag mounts are not lost later in boot.

The host 9p server now tracks arbitrary guest fid numbers with a dynamic fid map
instead of assuming the guest uses small dense fid IDs.

Writable mounts are still a separate piece of work. The syntax is reserved:

```sh
ant sandbox --write tmp:/tmp script.js
ant sandbox --write ./out:/out script.js
```

but the current 9p server remains read-mostly and should not be documented as a
stable writable mount ABI yet.

Validation after dynamic mount work:

```sh
meson compile -C build
./nanos/build-sandbox.sh --skip-docker
./build/ant sandbox examples/demo/advanced.js
./build/ant sandbox examples/demo/wasm.js
./build/ant sandbox examples/demo/pi.js
./build/ant sandbox --mount .:/project examples/demo/pi.js
./build/ant sandbox examples/npm/smoke
```

## Binary Request Transport

The first guest request over virtio-vsock no longer uses newline-delimited JSON.
The host now sends a compact binary request frame and the guest daemon parses
that frame directly.

Current frame shape:

```txt
header:
  magic[4] = "ANTF"
  version  = 1
  type     = run | eval
  flags    = 0
  length   = payload bytes

run payload:
  u32 capabilities
  u16 tty_rows
  u16 tty_cols
  string cwd
  string entry
  u32 argc
  string argv[argc]
  optional u32 forward_count
  optional u16 forward_guest_port[forward_count]
```

Strings are `u32` length-prefixed byte sequences. The forwarded guest port list
is present only when one or more forwards were requested; it lets the daemon
reject sandbox server binds that were not explicitly published with `--forward`.

## Framed Daemon Output

The daemon protocol now uses the same virtio-vsock byte stream for host-bound
frames:

- `stdout`
- `stderr`
- `result`
- `error`
- `exit`

The guest installs a sandbox output writer after the request is received and
the daemon has successfully entered the requested cwd. Normal console and
`process.stdout`/`process.stderr` writes become daemon frames. The Darwin host
parses the stream, writes stdout/stderr frames to the host terminal, and exits
the VM with the daemon's exit code when the `exit` frame arrives.

PL011 is no longer live user output. The Darwin host captures a bounded dynamic
diagnostic tail, suppresses routine kernel console chatter, and only reports the
captured tail as `SandboxKernelPanic` when it detects kernel panic/fault/assertion
output before a daemon `exit` frame.

## Structured Result And Error Frames

`result` and `error` frames are now typed protocol payloads instead of display
strings.

Result payloads carry a compact value tag for primitive values:

- `undefined`
- `null`
- booleans
- numbers
- strings
- display-only fallback

They also carry a display string so CLI-style consumers can still render a
human-readable value without knowing how to turn every future value kind into a
host object.

Error payloads carry structured fields for name, message, stack, and display
text. Host consumers such as `ant:sandbox` can reject with a real error-shaped
value, while the CLI path can keep printing the display form.

## TTY And Color Capability

The host now advertises terminal capabilities in the request frame:

- stdout TTY
- stderr TTY
- force color
- strip ANSI/color
- terminal rows and columns

The guest applies those capabilities before module initialization, so
`process.stdout.isTTY`, `process.stderr.isTTY`, console coloring, and
`getWindowSize()` reflect the host-facing sandbox transport instead of the
guest's PL011 device. The Darwin host also strips ANSI from received daemon
frames when the request capability asks for stripping; otherwise it preserves
frame bytes, and terminal-attached sandbox runs force color.

`ant:sandbox` exposes the policy deliberately:

```js
new Sandbox({
  tty: true,
  ttyRows: 40,
  ttyCols: 100,
  color: "preserve" // "auto", "force", "strip", or "preserve"
});
```

The default `auto` policy follows the host terminal. `force` asks the guest to
emit colored output, `strip` removes ANSI/color output at the host boundary, and
`preserve` leaves bytes alone without forcing guest color.

## Verbose Mode

`ant sandbox --verbose script.js` is the user-facing sandbox progress mode. It
prints high-signal backend and protocol milestones such as backend selection,
mount/forward setup, VM creation, GIC setup, memory mapping, kernel/FDT load,
vCPU creation, guest start, daemon connection, request send, and daemon exit
code. Verbose mode also streams the Nanos PL011 boot console to stderr; normal
mode keeps PL011 hidden except for captured kernel panic diagnostics.
Host verbose milestones use a `[seconds.usec] ant: ...` prefix so they sit
next to Nanos' own `[seconds.usec] ...` console output.

Raw MMIO and virtqueue tracing was removed from the public sandbox path.
Targeted bringup probes should be added deliberately instead of reviving a
catch-all raw device trace flag.

## Native NAT Forward Cleanup

Inbound host port forwards allocate a unique synthetic guest-visible peer port
for each accepted host connection. This keeps browser retries and parallel
connections from colliding on the same guest TCP tuple.

Sandboxed TCP server binds are checked against the forwarded guest port list
from the request frame. Both `node:net` servers and default-export HTTP servers
fail immediately with a `--forward` hint when they try to listen on an
unforwarded TCP port, instead of silently creating a guest-local server that the
host cannot reach.

The default VM timeout is now a 10s boot/request-delivery timeout. Once the daemon
connects and accepts the request, long-running scripts keep running until they
exit or the user stops the sandbox. The hidden `ANT_SANDBOX_VM_TIMEOUT_MS`
bringup override still wins as a full-run timeout for now, but it should
eventually be replaced by an explicit CLI/config timeout.

## Error Mapping

Sandbox VM execution now reports a classified result alongside the raw return
code. The host distinguishes:

- guest script exits
- backend unavailable
- configuration errors
- boot/request timeout
- kernel panic
- daemon protocol errors
- transport errors
- generic VM failures

The CLI treats guest exits as script failures and returns the guest exit code
without adding an infrastructure error. `ant:sandbox` keeps guest-thrown errors
as their original structured JS errors, while VM/protocol/kernel failures reject
with named no-stack errors such as `SandboxProtocolError`,
`SandboxKernelPanic`, or `SandboxVMError`.

## Native NAT Network Backend

The Darwin backend now uses an unentitled native NAT backend for sandbox
networking instead of Apple's `vmnet.framework`. This avoids sudo, helper
processes, and the `com.apple.vm.networking` entitlement problem for local
builds.

The virtio-net device now has real RX/TX behavior for the supported sandbox
network model:

- preserve guest RX buffers until packets are available
- handle ARP, DHCP, DNS, ICMP, and TCP enough for Ant fetch/server workflows
- translate generic guest UDP flows to host UDP sockets, with DNS/DHCP still
  handled as built-in control-plane services
- translate guest outbound TCP connections to host sockets
- expose a stable guest network at `10.0.2.15`
- support explicit host forwards with `ant sandbox --forward <port|host:guest>`
- allocate a unique synthetic guest-visible peer port per accepted host
  connection so retries and parallel connections do not collide
- wake the vCPU when NAT packets arrive so the guest RX ring can be drained

The old vmnet direction is deliberately removed from the plan: it needs special
signing/entitlements or privileged setup on macOS, while native NAT works in
ordinary local builds.

Validation after native NAT networking:

```sh
./build/ant sandbox examples/demo/fetch.js
./build/ant sandbox --forward 3000 examples/npm/elysia
./build/ant sandbox examples/demo/advanced.js
./build/ant sandbox examples/demo/wasm.js
./build/ant sandbox examples/demo/pi.js
```

## Native NAT Refactor

The native NAT implementation was split out of the virtio queue code so each
layer has a narrower job:

- `virtio_net.c` owns virtqueue TX/RX plumbing
- `net_packets.c` owns Ethernet, ARP, DHCP, DNS, ICMP, IPv4, TCP packet helpers
- `nat.c` owns socket translation, forwards, NAT session state, and RX enqueue

TCP connection lifecycle is now explicit state instead of several loosely
coupled booleans. NAT sessions live in a growable table of stable session
objects rather than a fixed `tcp[256]` array. Forward listeners and poll maps
are allocated from the requested sandbox configuration instead of living in
fixed global tables. Per-session guest-to-host and host-to-guest byte buffers
grow on demand and are capped by named safety limits instead of being embedded
as always-allocated 64 KiB arrays.

The verbose path reports high-signal network counters when the backend stops:
guest packets, host packets, dropped RX packets, opened TCP sessions, opened UDP
sessions, and NAT session slots.

`tests/test_sandbox_net_packets.c` covers packet byte-order and checksum
helpers, DHCP responses, DNS responses, outbound TCP SYN/SYN-ACK/ACK plus
FIN/RST handling, host forward accept delivery, and generic UDP NAT round trips.

## Virtio-vsock Event Queue And Credits

The Darwin virtio-vsock device now treats the socket connection as a real
credit-based stream instead of a one-shot packet shove:

- host-to-guest request frames are chunked to fit the guest receive buffers
- the host tracks guest `buf_alloc` and `fwd_cnt` before sending more stream
  data
- guest `RW` packets advance host receive credit and get `CREDIT_UPDATE`
  replies
- guest `CREDIT_UPDATE` and `CREDIT_REQUEST` packets can resume a partially
  sent host request
- `REQUEST`, `RESPONSE`, `RST`, and `SHUTDOWN` now reset or echo connection
  state deliberately

The host also wires virtio-vsock queue 2 instead of treating it as unused.
Nanos patch `0014-virtio-vsock-event-queue.patch` posts event buffers on the
guest event queue and rearms the queue after each event completion, including
the transport-reset event used by the virtio-vsock ABI.

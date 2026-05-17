# Nanos Sandbox

Status: active
Last reviewed: 2026-05-16
Owner: theMackabu

## Goal

Build an `ant sandbox` feature that runs JavaScript inside a hardware-isolated
Nanos unikernel using a prebuilt generic Ant image. The image should be shipped
once per Ant version and reused by consumers without rebuilding for each script.

The long-term module form is:

```js
import { Sandbox } from "ant:sandbox";

const sb = new Sandbox();
const result = await sb.eval(`
  export default 1 + 1;
`);
await sb.close();
```

Persistent sandbox instances should also support mounted project files:

```js
import { Sandbox } from "ant:sandbox";

const sb = new Sandbox({ mount: ".:/workspace" });
await sb.run("script.js");
await sb.eval("export default 1 + 1");
await sb.close();
```

The CLI form is:

```sh
ant sandbox script.js
```

## Core Architecture

The prebuilt Nanos image must remain generic. It should contain Ant and boot
into a dedicated guest mode, not a user-specific script:

```sh
/ant --sandbox-daemon
```

The host-side `ant sandbox` command is responsible for:

1. Finding or downloading a cached `ant-sandbox-<arch>.img` plus Nanos kernel.
2. Launching a Nanos-compatible backend with that image and kernel.
3. Passing requests to the guest Ant daemon through a backend-provided transport.
4. Streaming stdout, stderr, structured results, and exit status back to the
   host process.

The dynamic part is the host runner plus the guest daemon protocol. The image is
not rebuilt or patched for each script.

## Mount Model

Use the project directory as the first implementation model because it makes
local imports and file reads work naturally.

Default:

```sh
ant sandbox script.js
```

Behavior:

- Mount the current working directory read-only at `/workspace`.
- Set the guest cwd to `/workspace`.
- Run `script.js` from the guest daemon.

Explicit read-only mounts:

```sh
ant sandbox --mount .:/workspace script.js
ant sandbox --mount ./fixtures:/fixtures script.js
```

Explicit writable mounts:

```sh
ant sandbox --write tmp:/tmp script.js
ant sandbox --write ./out:/out script.js
```

`tmp:/tmp` means the host creates a temporary writable directory and exposes it
at `/tmp`. Host project directories should be read-only by default; writable
host paths must be explicit.

Example request:

```json
{
  "mode": "run",
  "cwd": "/workspace",
  "entry": "script.js",
  "argv": [],
  "mounts": [
    { "host": ".", "guest": "/workspace", "readonly": true },
    { "host": "tmp", "guest": "/tmp", "readonly": false }
  ]
}
```

## Protocol

Start with a one-shot protocol:

1. Host launches the VM.
2. Guest daemon reads one request.
3. Guest executes it.
4. Guest returns result and exits the VM.

For the first working native VMM path, use NDJSON over virtio-vsock for the
request/control channel. The guest daemon connects to host CID `2`, port
`1024`, reads one newline-delimited JSON request, executes it, and exits. 9p
remains the workspace file transport.

For `Sandbox` instances, evolve to a persistent JSON-RPC-like protocol over the
same stream transport:

- `eval`
- `run`
- `close`
- stdout/stderr frames
- result/error frames

For pure `eval`, virtio-serial alone can carry the source and result. For
`script.js` with local imports/assets, use the mount model above.

## Backend Contract

Each backend should expose the same Nanos-compatible machine shape instead of
using platform-specific guest conventions:

- load the cached Nanos kernel
- attach the cached Ant/Nanos disk image as virtio-blk
- provide console output in a Nanos-compatible way
- provide one request transport for JSON frames
- optionally provide a Nanos-compatible file mount for `/workspace`

The current common denominators are:

- request: virtio-vsock NDJSON, with 9p request-file fallback only as a bringup
  aid
- files: virtio-9p for the default read-only cwd mount, or an attached input
  volume when a backend cannot provide 9p yet
- output: guest console/stdout

Avoid image patching, temp-image argument rewrites, TCP control channels, host
request files, and stdin probing as the default model. The daemon should only
consume requests from an explicit backend transport.

The Darwin backend uses Hypervisor.framework directly. It must boot the cached
Nanos kernel and disk image itself and expose only devices that the generic
Nanos image already understands.

## Cache Layout

The local builder and host CLI use the Ant sandbox cache:

- If legacy `~/.ant` exists: `~/.ant/sandbox/`
- Otherwise: `$XDG_CACHE_HOME/ant/sandbox/` or `~/.cache/ant/sandbox/`
- `ant-sandbox-<arch>.img`
- `nanos-kernel-<arch>.img`

The image manifest must include this generic mount ID:

- `%0:/workspace:ro`

Current local filenames:

- `ant-sandbox-arm64.img`
- `nanos-kernel-arm64.img`

The default `ant sandbox script.js` mount is image manifest `%0:/workspace`
paired with a host 9p mount tag of `0`. This matches Nanos's virtio-9p attach
ID handling (`virtfs0`) and keeps the image generic: the host supplies whatever
current working directory should back `/workspace` at launch time.

`/workspace` is the MVP guest path, not necessarily the final mount ABI.
`Sandbox({ mount: ".:/workspace" })` works with the current baked image model,
but arbitrary guest paths such as `Sandbox({ mount: ".:/project" })` need more
design. Because Nanos consumes mount points from the image manifest, the host
cannot currently invent a new guest mount path at VM launch by changing only the
request JSON. Possible directions:

- Bake stable mount roots such as `/mnt/ant/0`, `/mnt/ant/1`, ... and map
  requested guest paths through aliases/symlinks or daemon-side path rewriting.
- Keep `/workspace` as the real mount and treat custom guest names as aliases
  once the guest daemon can create or resolve them safely.
- Patch or upstream Nanos support for dynamically attaching virtio-9p mounts at
  caller-selected guest paths, if that proves practical.

`nanos/build-sandbox.sh` copies the freshly built image and matching Ops/Nanos
kernel into this cache. `ant sandbox <script>` reads from the same cache and can
seed it from `nanos/out/<arch>/ant-sandbox.img` plus `~/.ops/.../kernel.img`
during local development.

## Native VMM Notes

Local ARM image facts observed on 2026-05-16:

- `ant-sandbox.img` is a raw MBR image.
- It contains two Nanos TFS partitions.
- Raw size is about 24 MB.
- Partition 1 is a fixed Nanos/kernel area of about 12 MiB and mostly zeros.
- Partition 2 contains the Ant app payload of about 11.3 MiB.
- Embedded Ant ELF is already stripped; the bulk is real `.text`, `.rodata`,
  and unwind data.

The sandbox runner should be a small platform-native VMM:

- Linux: KVM
- macOS: Hypervisor.framework
- Windows: WHPX / Hyper-V APIs

The minimum runner needs:

- guest memory allocation
- Nanos-compatible kernel/image loading
- vCPU setup/loop
- virtio block for the image
- virtio serial for protocol frames
- eventually virtio-9p or an equivalent shared filesystem/mount mechanism

The VMM should expose the same Nanos-compatible machine model on every backend
instead of binding the image to one host API's preferred device set.

## macOS Hypervisor.framework Notes

The first macOS backend lives under `src/sandbox/backends/darwin.c` and uses
Hypervisor.framework directly. References cloned for this work:

- `/tmp/hypervisor-framework`: small Rust wrapper showing Apple Silicon VM,
  memory map, and vCPU lifecycle.
- `/tmp/libkrun`: production ARM64 HVF reference for GIC/FDT/virtio-mmio style
  wiring.
- `/tmp/xhyve` and `/tmp/hyperkit`: useful historical HVF references, mostly
  Intel-era and less directly useful for Apple Silicon.

The backend currently uses Hypervisor.framework directly to create the VM, map
guest RAM, create the GIC/vCPU, load the cached Nanos kernel, provide a boot
FDT, and emulate enough UART/RTC/PCI/legacy virtio-blk surface to start booting
the cached image from `~/.ant/sandbox`. The current local run reaches Nanos
guest output and services virtio-blk reads from the cached image. The PCI
enumeration bug was slot 0 reporting header type
`0xff`, which made Nanos take the multi-host-controller path and skip normal bus
0 probing; slot 0 now reports a single-controller header while remaining absent.
The backend also exposes a minimal legacy virtio-net PCI device at slot 2 with
`VIRTIO_NET_F_MAC`, RX/TX queues, and drop-complete TX handling. Nanos now
attaches `en1` and assigns an IPv6 link-local address, proving network device
discovery works. The next debugging layer is timer wakeup plus the
request/workspace transport.
Meson now ad-hoc signs local Darwin builds with `meson/ant.entitlements` so
Hypervisor.framework allows `hv_vm_create`.

Still missing from the macOS backend:

- reliable timer and device interrupt delivery through Hypervisor.framework's GIC
- a real host network backend for virtio-net RX/TX instead of drop-complete TX
- virtio-9p or another Nanos-compatible attached input volume for `/workspace`
- virtio-vsock request/control transport
- structured stdout/stderr/result framing back to the host CLI

## Open Questions

- Should no-arg Ant inside Nanos automatically enter sandbox daemon mode, or
  should the image manifest explicitly pass `--sandbox-daemon`?
- Should the first prototype use 9p for mounted cwd, or package a small init
  request into a separate block/initrd-style device?
- What is the exact stdout/stderr/result framing format?
- How should exit codes and thrown exceptions map back to the host CLI?
- What minimal APIs should `ant:sandbox` expose in the first module version?

## Next Steps

1. Done: add hidden CLI mode `ant --sandbox-daemon` with one-shot `run` and
   ESM-shaped `eval` request handling once a backend transport supplies JSON.
2. Done: teach local and CI Nanos image builds to boot Ant with
   `--sandbox-daemon` while keeping the image generic.
3. In progress: implement the Darwin Hypervisor.framework backend with Nanos
   kernel loading, console output, storage, and transport devices.
4. In progress: add basic `ant sandbox script.js` cache resolution and default
   read-only cwd mount config.
5. Add explicit writable temp mount support.
6. Promote the protocol into `ant:sandbox` with persistent VM support.

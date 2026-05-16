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

1. Finding or downloading a cached `nanos-ant-<version>-<arch>.img`.
2. Launching a hypervisor runner with that image.
3. Passing requests to the guest Ant daemon.
4. Streaming stdout, stderr, structured results, and exit status back to the
   host process.

The dynamic part is the host runner plus the guest daemon protocol. The image is
not rebuilt for each script.

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

For `Sandbox` instances, evolve to a persistent JSON-RPC-like protocol over
virtio-serial:

- `eval`
- `run`
- `close`
- stdout/stderr frames
- result/error frames

For pure `eval`, virtio-serial alone can carry the source and result. For
`script.js` with local imports/assets, use the mount model above.

## QEMU Proof Notes

Local ARM image facts observed on 2026-05-16:

- `ant-sandbox.img` is a raw MBR image.
- It contains two Nanos TFS partitions.
- Raw size is about 24 MB.
- Partition 1 is a fixed Nanos/kernel area of about 12 MiB and mostly zeros.
- Partition 2 contains the Ant app payload of about 11.3 MiB.
- Embedded Ant ELF is already stripped; the bulk is real `.text`, `.rodata`,
  and unwind data.

Direct QEMU boot notes:

- `qemu-system-aarch64 -kernel ~/.ops/0.1.55-arm/kernel.img` rejected the Nanos
  ELF with "incompatible architecture" in local QEMU 10.2.1.
- UEFI boot saw the disk but dropped to the shell and reported an x64 image type
  during probing.
- QEMU generic loader worked:

```sh
qemu-system-aarch64 \
  -machine virt,gic-version=2,highmem=off \
  -m 1G \
  -cpu host \
  -accel hvf \
  -device loader,file="$HOME/.ops/0.1.55-arm/kernel.img",cpu-num=0 \
  -drive if=none,id=hd0,format=raw,file=ant-sandbox.img \
  -device virtio-blk-pci,drive=hd0 \
  -semihosting \
  -device virtio-rng-pci \
  -netdev user,id=n0 \
  -device virtio-net,netdev=n0 \
  -display none \
  -serial stdio \
  -no-reboot
```

Observed output:

```text
[0.002068] en1: assigned 10.0.2.15
```

That proves the image can boot under QEMU without Ops. It does not yet prove the
guest can receive and execute arbitrary host scripts; that requires the daemon
mode and request channel.

## Future VMM Direction

QEMU is the right proof tool, but the long-term differentiator is a small
platform-native runner:

- Linux: KVM
- macOS: Hypervisor.framework or Virtualization.framework
- Windows: WHPX / Hyper-V APIs

The minimum runner needs:

- guest memory allocation
- Nanos kernel loading
- vCPU setup/loop
- virtio block for the image
- virtio serial for protocol frames
- eventually virtio-9p or an equivalent shared filesystem/mount mechanism

Do not start by building the VMM. First prove `ant --sandbox-daemon` and the
host/guest protocol with QEMU.

## Open Questions

- Should no-arg Ant inside Nanos automatically enter sandbox daemon mode, or
  should the image manifest explicitly pass `--sandbox-daemon`?
- Should the first prototype use 9p for mounted cwd, or package a small init
  request into a separate block/initrd-style device?
- What is the exact stdout/stderr/result framing format?
- How should exit codes and thrown exceptions map back to the host CLI?
- What minimal APIs should `ant:sandbox` expose in the first module version?

## Next Steps

1. Add hidden CLI mode: `ant --sandbox-daemon`.
2. Teach the Nanos image build to boot Ant in daemon mode while staying generic.
3. Prototype host `ant sandbox --eval '1 + 1'` using QEMU and virtio-serial.
4. Add read-only cwd mount support for `ant sandbox script.js`.
5. Add explicit writable temp mount support.
6. Promote the protocol into `ant:sandbox` with persistent VM support.

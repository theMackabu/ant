# Nanos Sandbox: Things To Add

Status: active
Last reviewed: 2026-05-16
Owner: theMackabu

## Reliable HVF Delivery

Make timer and device interrupt delivery reliable through Hypervisor.framework's
GIC.

This is the next backend priority before chasing higher-level features. Device
completion, timer interrupts, and guest scheduling need to be boring before the
transport layer can be trusted.

## Real Network Backend

Add a proper network backend for virtio-net RX/TX instead of a discovery-only or
drop-complete implementation.

The goal is a Nanos-compatible device that can eventually support guest fetch,
package access, and controlled sandbox networking.

## Workspace Mounts

Implement the read-only current-working-directory mount:

```sh
ant sandbox script.js
```

Default behavior:

- mount host cwd read-only at `/workspace`
- set guest cwd to `/workspace`
- run `script.js` from the guest daemon

Add explicit read-only mounts:

```sh
ant sandbox --mount .:/workspace script.js
ant sandbox --mount ./fixtures:/fixtures script.js
```

Add explicit writable mounts:

```sh
ant sandbox --write tmp:/tmp script.js
ant sandbox --write ./out:/out script.js
```

`tmp:/tmp` means the host creates a temporary writable directory and exposes it
at `/tmp`. Host project directories should be read-only by default; writable
host paths must be explicit.

## Mount Path Design

The image manifest currently needs this generic mount ID:

- `%0:/workspace:ro`

This pairs with a host 9p mount tag of `0`, matching Nanos' virtio-9p attach ID
handling (`virtfs0`). That keeps the image generic: the host supplies whatever
cwd should back `/workspace` at launch time.

For arbitrary guest paths, choose one of these directions:

- bake stable mount roots such as `/mnt/ant/0`, `/mnt/ant/1`, ... and map
  requested guest paths through aliases/symlinks or daemon-side path rewriting
- keep `/workspace` as the real mount and treat custom guest names as aliases
  once the guest daemon can create or resolve them safely
- patch or upstream Nanos support for dynamically attaching virtio-9p mounts at
  caller-selected guest paths

## Framed Protocol

Add length-prefixed frames over a Nanos-compatible transport, preferably
virtio-vsock.

Initial frame types:

```json
{"type":"run","cwd":"/workspace","entry":"script.js","argv":[]}
{"type":"eval","cwd":"/workspace","source":"export default 1 + 1"}
{"type":"stdout","data":"hello\n"}
{"type":"stderr","data":"warning\n"}
{"type":"result","value":2}
{"type":"error","name":"TypeError","message":"...","stack":"..."}
{"type":"exit","code":0}
```

This starts with JSON only as a bringup payload. Replace it with a custom binary
serializable stream before treating the protocol as stable.

## stdout/stderr And TTY

Route normal guest stdout/stderr through daemon frames instead of PL011.

TTY/color behavior should become an explicit sandbox protocol capability. The
current PL011 path makes guest `process.stdout.isTTY` false, so ordinary
`console.log` output does not use colors even though some error-reporting paths
still print ANSI styling directly.

Once stdout/stderr are daemon frames, the host should decide whether to:

- advertise TTY/color support
- preserve ANSI
- strip ANSI
- force color behavior for terminal-attached sandbox runs

## `ant:sandbox`

Promote the protocol into an `ant:sandbox` module with persistent VM support:

```js
import { Sandbox } from "ant:sandbox";

const sb = new Sandbox({ mount: ".:/workspace" });
await sb.run("script.js");
await sb.eval("export default 1 + 1");
await sb.close();
```

Persistent sandbox instances should support:

- `eval`
- `run`
- `close`
- stdout/stderr frames
- result/error/exit frames

## CLI Polish

Add a user-facing verbose mode:

```sh
ant sandbox --verbose script.js
```

The mode should show high-signal boot/device/protocol milestones without raw
MMIO or virtqueue spam.

## Cross-Platform Backends

The sandbox runner should eventually be a small platform-native VMM:

- Linux: KVM
- macOS: Hypervisor.framework
- Windows: WHPX / Hyper-V APIs

Each backend should expose the same Nanos-compatible machine model instead of
binding the image to one host API's preferred device set.

Minimum backend pieces:

- guest memory allocation
- Nanos-compatible kernel/image loading
- vCPU setup/loop
- virtio block for the image
- protocol transport
- eventually virtio-9p or an equivalent shared filesystem mechanism

## Error Mapping

Define how guest exits map back to the host CLI:

- normal exit code
- thrown exception
- unhandled rejection
- daemon protocol error
- VM boot failure
- kernel panic
- transport failure

The host should distinguish user script failure from sandbox infrastructure
failure.

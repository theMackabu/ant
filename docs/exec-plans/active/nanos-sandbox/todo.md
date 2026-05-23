# Nanos Sandbox: Things To Add

Status: active
Last reviewed: 2026-05-23
Owner: theMackabu

## Writable Mounts

Implement explicit writable mounts on top of the dynamic mount path work:

```sh
ant sandbox --write tmp:/tmp script.js
ant sandbox --write ./out:/out script.js
```

`tmp:/tmp` should create a temporary host directory and expose it at `/tmp`.
Host project directories should stay read-only by default; writable host paths
must be explicit.

The current host 9p server is read-mostly. Writable mount support needs real 9p
mutation handlers, path policy, and a clear cleanup story for temporary writable
directories.

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

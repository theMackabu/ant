# Nanos Sandbox: Things To Add

Status: active
Last reviewed: 2026-05-23
Owner: theMackabu

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

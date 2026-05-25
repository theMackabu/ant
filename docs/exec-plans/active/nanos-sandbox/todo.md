# Nanos Sandbox: Things To Add

Status: active
Last reviewed: 2026-05-23
Owner: theMackabu

## Cross-Platform Backends

The sandbox runner should eventually be a small platform-native VMM:

- Linux: KVM
- macOS: Hypervisor.framework
- Windows: WHPX / Hyper-V APIs

# Todo

1. **virtio-fs**
   This is the big one. It would replace a lot of the 9p pain: fewer path-walk quirks, better file I/O semantics, better performance ceiling, cleaner writable mounts. It may be more work than 9p, but it attacks a real product problem.

2. **A cleaner block/storage path**
   If writable images, package caches, or persistent sandboxes matter, virtio-blk needs flush/discard/write-zeroes reviewed properly. Could also eventually add a separate writable scratch disk instead of leaning only on 9p writes.

3. **virtio-balloon**
   Less urgent, but good if long-running persistent sandboxes become normal and you want memory pressure behavior to be less crude.

4. **virtio-console**
   Maybe, but only if we want a more formal low-level diagnostic channel. Normal stdout/stderr should stay on the framed daemon protocol, so console is not as exciting.

5. **virtio-gpu**
   Fun, but probably a distraction unless you want graphical sandbox apps someday.

6. **More real net features**
   Not a new guest driver exactly, but UDP NAT beyond DNS/DHCP, IPv6, better TCP state, and maybe packet capture/debugging would make the current virtio-net path feel much more production.

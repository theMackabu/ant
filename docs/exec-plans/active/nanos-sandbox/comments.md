# Nanos Sandbox: Comments

Status: active
Last reviewed: 2026-05-23
Owner: theMackabu

## Core Architecture

The prebuilt Nanos image must remain generic. It should contain Ant and boot
into a dedicated guest mode, not a user-specific script:

```sh
/ant --sandbox-daemon
```

The dynamic part is the host runner plus the guest daemon protocol. The image is
not rebuilt or patched for each script.

## Backend Contract

Each backend should expose the same Nanos-compatible machine shape instead of
using platform-specific guest conventions:

- load the cached Nanos kernel
- attach the cached Ant/Nanos disk image as virtio-blk
- provide console output in a Nanos-compatible way for debug/failure output
- provide one request/result/stdout/stderr transport
- optionally provide a Nanos-compatible file mount for `/workspace`

The current common denominators are:

- request/results/stdout/stderr: length-prefixed frames, likely virtio-vsock
- files: virtio-9p for read-only host mounts, or an attached input volume when
  a backend cannot provide 9p yet
- debug output: guest console/PL011

The Darwin backend uses Hypervisor.framework directly. It must boot the cached
Nanos kernel and disk image itself and expose only devices that the generic
Nanos image already understands.

## Protocol Comments

Start with a one-shot protocol:

1. Host launches the VM.
2. Guest daemon connects to the host sandbox service.
3. Guest executes the request.
4. Guest returns result and exits the VM.

For pure `eval`, the framed transport carries the source and result. For
`script.js` with local imports/assets, use the mount model.

For persistent `Sandbox` instances, evolve to a request/response protocol over
the same binary stream transport.

## Mount Comments

Use the project directory as the default implementation model because it makes
local imports and file reads work naturally. The CLI now supports repeatable
read-only mounts where each host path maps to a real guest mountpoint.

Examples:

```sh
ant sandbox script.js
ant sandbox --mount .:/project script.js
ant sandbox --mount .:/workspace --mount ./fixtures:/fixtures script.js
```

The host backend gives each mount its own virtio-9p device and encodes the
attach ID plus guest path in the 9p tag, for example `0:/workspace:ro`. The
patched Nanos side creates the target mountpoint and merges dynamic tag mounts
with any boot manifest mount table.

The request transport uses a binary frame with length-prefixed strings. Mounts
are established by the backend as real virtio-9p devices; the daemon request
only needs the guest cwd, entry/source, and argv.

Writable mounts are still future work. The planned CLI shape is explicit:

```sh
ant sandbox --write tmp:/tmp script.js
ant sandbox --write ./out:/out script.js
```

## Native VMM Comments

The VMM should expose the same Nanos-compatible machine model on every backend
instead of binding the image to one host API's preferred device set.

This keeps the shipped artifact stable:

```txt
nanos-ant-<version>-<arch>.img
  /ant --sandbox-daemon
```

Each host backend adapts the platform API to devices Nanos already understands.

## Open Questions

- Should no-arg Ant inside Nanos automatically enter sandbox daemon mode, or
  should the image manifest explicitly pass `--sandbox-daemon`?
- Should the first prototype use 9p for mounted cwd, or package a small init
  request into a separate block/initrd-style device?
- Should structured result frames eventually support richer object transfer, or
  should `ant:sandbox` keep primitive results and require explicit stdout/files
  for larger data?
- How should exit codes, thrown exceptions, guest daemon errors, and VM/backend
  failures map into distinct host error classes?
- Should persistent `ant:sandbox` add async request queueing/cancellation, or
  keep one in-flight request per VM until the first stable API pass?

## Tech Debt Notes

There is an unrelated Ant bug where `JSON.stringify(this)` can throw a
localStorage warning/error if `--localstorage-file` or `localStorage.setFile`
were not provided valid paths. Track separately from sandbox.

The docs should keep calling out when a workaround is Nanos-specific,
ops-specific, or HVF-specific so it does not accidentally become a permanent
API decision.

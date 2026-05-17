# Nanos Sandbox: Comments

Status: active
Last reviewed: 2026-05-16
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
- files: virtio-9p for the default read-only cwd mount, or an attached input
  volume when a backend cannot provide 9p yet
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

Use the project directory as the first implementation model because it makes
local imports and file reads work naturally.

Example request shape during bringup:

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
- What is the exact stdout/stderr/result framing format?
- How should exit codes and thrown exceptions map back to the host CLI?
- What minimal APIs should `ant:sandbox` expose in the first module version?

## Tech Debt Notes

There is an unrelated Ant bug where `JSON.stringify(this)` can throw a
localStorage warning/error if `--localstorage-file` or `localStorage.setFile`
were not provided valid paths. Track separately from sandbox.

The docs should keep calling out when a workaround is Nanos-specific,
ops-specific, or HVF-specific so it does not accidentally become a permanent
API decision.

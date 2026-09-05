# Private primordial capture

Status: active
Last reviewed: 2026-09-04
Owner: theMackabu

## Scope

Replace JS bootstrap capture and the sixth CommonJS parameter with one ordinary
internal module. No public capture API, arbitrary-name registry, registration
hooks, compiler hints, or opcode changes.

## Design

- `include/primordial_list.h` declares the supported captures in a re-includable
  definition table, used to generate IDs and capture metadata.
- Before JS bootstrap, original values are copied into 20 per-runtime slots:
  19 exported values plus the original Function.prototype.call for uncurrying.
- GC marks these slots even if public properties are overwritten or deleted.
- `ant:internal/primordials` constructs and freezes one null-prototype object
  on first import. Uncurried methods use the existing bound-function machinery.
- CommonJS returns to its normal five parameters; path and its helpers import
  the internal module normally. The internal specifier is not a supported public
  API and does not expose a general capture/lookup facility.

Startup capture is a fixed number of existing property lookups and stores, not
a traversal of the builtin graph. Slot storage is 160 bytes per runtime on this
64-bit build; this is not a claim about total process memory. Wrapper allocation
is deferred until first import. Failure leaves the cached object unpublished.

## Verification and profiling

The release/PGO/LTO build passes, as do preflight, focused primordial/alias/cwd
tests, 100 seeded Node path differential probes, and the full spec suite
(4,136 assertions across 102 files, no failures).

Compared against a freshly built baseline from `99264fff`, using the same
configured options. Serial ABBA, five warmup blocks and 30 measured blocks
per scenario (60 fresh processes per binary):

| Median | Baseline JS capture | Native fixed captures |
| --- | ---: | ---: |
| Startup wall time | 5.894 ms | 5.872 ms |
| Startup plus path wall time | 6.726 ms | 6.675 ms |
| First path import, inside JS | 0.587 ms | 0.607 ms |
| Startup peak RSS | 6,406,144 B | 6,373,376 B |
| Path peak RSS | 7,438,336 B | 7,438,336 B |

Wall times include the timing launcher, runtime initialization, a memory query,
output, and process exit. Filesystem caches are warm; these are fresh-process,
not cold-disk measurements. First-import cost increased about 20 microseconds;
total startup medians are essentially unchanged. In-process RSS after path
import increased 48 KiB, while median peak RSS was unchanged. RSS is coarse;
Ant's process.memoryUsage heap fields in this build mirror RSS and are not
independent allocation measurements. Executable size increased 144 bytes.

Both builds use the same existing PGO inputs, which emit stale-profile warnings;
this is an artifact comparison, not independently retrained PGO attribution.
It shows no substantial overhead on this host, not a universal guarantee.

Artifacts: `/tmp/ant-primordials.duLHZR` contains `profile.mjs`, raw samples and
hashes in `profile.json`, validation logs, and the preserved baseline executable.
Baseline SHA256: `aef6a60c17ce2fce13b6bcf3c4e7d0d3b3d5719a452159d9ee3c4ea39e68f153`.
Candidate SHA256: `0420913651444b55975efe2092fef41051315a1a80822595aadfb17f0d2aef8c`.

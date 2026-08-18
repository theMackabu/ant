# Ant Cron API

Status: completed
Last reviewed: 2026-08-18
Owner: theMackabu

## Goal

Add `Ant.cron` with the same public overloads and runtime behavior as Bun 1.4's
cron API:

- `Ant.cron.parse(expression, relativeDate?, options?)`;
- `Ant.cron(schedule, handler, options?)` for in-process jobs;
- `Ant.cron(path, schedule, title)` for persistent OS jobs; and
- `Ant.cron.remove(title)`.

Reference: `https://bun.com/docs/runtime/cron.md`, read on 2026-08-17.

## Constraints

- Five-field expressions use local time by default and accept an IANA zone via
  `{ tz }` for parsing and in-process jobs.
- Reuse Ant's Temporal provider for IANA data and DST disambiguation.
- In-process handlers must not overlap. The next occurrence is selected only
  after a returned Promise settles.
- Active jobs must keep their handler and handle alive through GC. `unref()`
  must release event-loop liveness without cancelling the job.
- Persistent registration must replace jobs with the same validated title and
  must never edit a real scheduler during tests.
- The persistent runner imports the module and invokes its default export's
  `scheduled(controller)` method, awaiting asynchronous completion through the
  normal event loop.

## Task List

- [x] Implement expression parsing, matching, normalization, and next-time
      search with nicknames, names, ranges, lists, steps, and POSIX DOM/DOW OR.
- [x] Implement `CronJob`, Promise-aware rescheduling, disposal, ref/unref, GC
      marking, and `Ant.cron` registration.
- [x] Implement Linux crontab, macOS launchd, and Windows Task Scheduler
      registration/removal.
- [x] Generate native launchd calendar intervals and Windows CalendarTrigger
      XML, including S4U user context and the 48-trigger limit.
- [x] Move persistent scheduler I/O and child-process waits off the JavaScript
      thread, and make scheduler failures observable through the returned
      Promise.
- [x] Harden Linux crontab updates against failed reads and concurrent Ant
      updates.
- [x] Add the internal scheduled-module CLI path and controller object.
- [x] Add public TypeScript declarations and focused tests.
- [x] Add a safe runnable demo, spec coverage, and the focused cron tests to
      the repository harness.
- [x] Run preflight, build, focused runtime tests, and Bun differential probes.

## Decisions

- Store parsed fields as bitsets and preserve whether the minute, hour,
  day-of-month, and day-of-week fields were literal wildcards. The wildcard
  metadata is required for POSIX day matching and fall-back DST behavior.
- Search both absolute minutes and local civil minutes. Absolute scanning
  preserves duplicated fall-back minutes; civil scanning uses Temporal's
  compatible disambiguation to shift missing spring-forward times by the gap.
- Search matching civil days, hours, and minutes directly instead of scanning
  every minute for up to eight years. Limit absolute-minute scans to the window
  around a time-zone offset transition.
- Keep native CronJob state outside the JS heap, root active handles through
  GC, and reschedule only after returned Promises settle.
- Await the persistent module's returned Promise and propagate asynchronous
  rejection through the scheduler process exit status.
- Run persistent registration and removal through `uv_queue_work`; only settle
  their JavaScript Promises after the OS operation completes. Serialize those
  requests so same-process scheduler updates cannot lose one another.
- Use launchd `StartCalendarInterval` entries and Windows Task Scheduler XML
  rather than launching every minute and filtering inside Ant.
- Bound launchd interval expansion, replace LaunchAgent files transactionally,
  and group Windows calendar fields according to Task Scheduler's native XML
  model.
- Exercise persistent registration with an isolated HOME and a fake
  `launchctl`, including same-title replacement and removal, so validation
  cannot modify the user's real scheduler.

## Validation Log

- 2026-08-17: initial and final `maid preflight` passed.
- 2026-08-17: `meson compile -C build` passed.
- 2026-08-17: `./build/ant tests/test_ant_cron.cjs` passed.
- 2026-08-17: `./build/ant tests/test_ant_cron_cli.cjs` passed.
- 2026-08-17: `./build/ant tests/test_ant_cron_os.cjs` passed with fake
  `launchctl`; no real scheduler state was changed.
- 2026-08-17: Linux and Windows registration tests passed their host guards on
  macOS; their platform branches were not executed on this host.
- 2026-08-17: MinGW C23 syntax checks passed for the Windows cron scheduler and
  Windows system-time-zone mapping.
- 2026-08-17: `./build/ant examples/spec/run.js --all` passed: 3,944 tests in
  100 files, with zero failures.
- 2026-08-17: `maid knowledge`, `maid structure`, and `maid validate_changes`
  passed.
- 2026-08-17: Bun 1.4 differential probes matched parsing, names, nicknames,
  steps, impossible dates, overload errors, IANA zones, and spring/fall DST
  behavior.
- 2026-08-17: warm `Ant.cron.parse("@yearly", 0, { tz: "UTC" })` measured
  about 0.0022 ms per call over 10,000 iterations; the minute-by-minute scan
  regression is removed.
- 2026-08-17: the harness passed the cron demo, the 18-assertion cron spec,
  and all five focused cron regression targets.
- 2026-08-17: the expanded spec suite passed 3,962 tests in 101 files with
  zero failures.
- 2026-08-18: fixed the reviewed spring-forward collapse, epoch-zero and Date
  boundary handling, throwing time-zone getters, and the Linux unterminated
  crontab overflow; focused Bun differential assertions pass.
- 2026-08-18: added semantic Windows repetition/grouping, transactional and
  bounded macOS registration, serialized scheduler work, isolate teardown,
  O(1) stopped-job access, and native-platform CI execution.
- 2026-08-18: `maid preflight`, Meson reconfigure/build, all five focused cron
  tests, the cron demo/spec harness targets, and the 3,962-test full spec suite
  passed. The Windows branch also passed a MinGW C23 syntax check.
- 2026-08-18: the deterministic timer fixture fired two asynchronous handlers
  with a maximum concurrency of one. A 10,000-job local sweep measured about
  0.22 ms for `unref`, 0.65 ms for `stop`, and 0.22 ms for stopped `ref`.

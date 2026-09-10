# Separate Runtime Artifacts

Status: completed; CI artifact production and API rollout were not verified here
Last reviewed: 2026-09-10
Owner: theMackabu

Historical implementation record extracted from the execution-plan index.
The validation below was recorded during implementation, not rerun during
the documentation cleanup.

## Outcome And Decisions

The platform workflow uploads `ant-runtime-<target>` separately from
`ant-<target>`. Runtime artifact names derive from the configured Ant artifact
name so musl target naming stays consistent.

`maid download` skips runtime artifacts by default. The API resolves their
own artifact IDs and extracts `ant-runtime` or `ant-runtime.exe`. Runtime
resolution preserves run selection and release fallback, including ZIP
extraction for runtime release assets. Version metadata comes from the
matching `version-ant-<target>` artifact.

Default downloads also skip bench-v8 scores. `maid download -- --all`
downloads every artifact, including runtime, version, and score ZIPs.
Artifact enumeration follows all API pages.

## Recorded Validation And Rollout Limit

`cd docs/api && bun test` passed 18 tests covering all targets, explicit and
latest runs, extraction, release fallback, and download filtering. Native
build/reconfigure and spec runs did not validate this packaging-only change.

The original rollout requirement was for CI to produce the new artifacts
before deploying the updated API. This archive does not establish whether
that later rollout occurred; verify artifact availability for deployment work.

## Original Record

The full source note is recoverable with:

```sh
git show bc206f10a9ea73d3d91302eb208adf1479e4d22e:docs/exec-plans/index.md
```

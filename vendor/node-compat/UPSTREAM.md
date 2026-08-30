# Upstream provenance

This directory vendors the direct-execution Node.js compatibility tests curated
by Bun.

- Repository: <https://github.com/oven-sh/bun>
- Bun commit: `ed950b88ab2ec6b58bccdfe7d310731b8ca13c4d`
- Imported path: `test/js/node/test`
- Imported directories: `parallel`, `common`, and `fixtures`
- Imported: 2026-08-29

The tests originated in the Node.js repository and include Bun's compatibility
adaptations. `NODE-LICENSE` is the Node.js license captured from Node commit
`045ff95365c8a65f8abc0a33efb055b6d9d93c03`; `BUN-LICENSE.md` is Bun's license
at the pinned Bun commit.

Do not edit imported test files for Ant. Keep Ant-specific selection,
expectations, and process orchestration under `tests/node-compat/`. Update this
snapshot by replacing the three imported directories from a new pinned Bun
commit and recording the new identity here.

`package.json` is Ant-owned harness metadata, not an upstream file. It marks
the imported `.js` tests as CommonJS, matching how Node and Bun execute them.

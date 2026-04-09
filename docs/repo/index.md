# Repo Knowledge Index

Status: active
Last reviewed: 2026-04-09
Owner: theMackabu

This directory is the durable, versioned knowledge base for Ant's repository
workflow. Start with the smallest document that answers the task at hand.

## Core References

- Agent entrypoint: [../../AGENTS.md](../../AGENTS.md)
- Architecture map: [../../ARCHITECTURE.md](../../ARCHITECTURE.md)
- Build instructions: [../../BUILDING.md](../../BUILDING.md)
- Contribution guide: [../../CONTRIBUTING.md](../../CONTRIBUTING.md)
- Test selection guide: [testing.md](testing.md)
- Execution plans and tech debt: [../exec-plans/index.md](../exec-plans/index.md)

## How To Use This Knowledge Base

- Keep stable reference material here instead of burying it in chat history or
  scratch notes.
- Use execution plans for work that spans multiple commits, decisions, or
  checkpoints.
- Keep `AGENTS.md` short and link into this directory rather than expanding it.
- Run `maid knowledge` after updating these docs so stale links or missing
  metadata fail fast.
- Run `maid structure` to guard changed-file boundaries.
- Run `maid validate_changes` to route the current diff to the smallest safe
  validation set.

## When To Add A New Doc

- Add a new document when a rule, subsystem map, or workflow is reused across
  tasks and would otherwise be repeated in prompts or review comments.
- Prefer one focused file per topic over a single large manual.

# Execution Plans

Status: active
Last reviewed: 2026-04-09
Owner: theMackabu

Use this directory for durable, versioned plans when work spans multiple
decisions, checkpoints, or follow-up changes.

## Layout

- Active plans: [active/README.md](active/README.md)
- Completed plans: [completed/README.md](completed/README.md)
- Technical debt tracker: [tech-debt.md](tech-debt.md)

## When To Create A Plan

- The task spans multiple subsystems.
- The work will happen across multiple commits or pull requests.
- Validation has meaningful risk, tradeoffs, or deferred follow-ups.
- Future contributors will need the reasoning, not just the final diff.

## Plan Expectations

- State the problem, constraints, and intended outcome up front.
- Keep a short decision log as the work evolves.
- Record validation status and unresolved risks.
- Move finished plans into `completed/` once the work is done.

`todo/` can still hold scratch notes, but durable execution history belongs in
this directory.

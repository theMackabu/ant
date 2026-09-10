# Execution Plans

Status: active
Last reviewed: 2026-09-10
Owner: theMackabu

Use individual plans for work spanning multiple decisions, checkpoints, or
follow-up changes. Keep this index for navigation and lifecycle guidance.

## Find A Plan

- Work in progress: [active/README.md](active/README.md)
- Historical outcomes: [completed/README.md](completed/README.md)
- Unscheduled follow-ups: [tech-debt.md](tech-debt.md)

Open the smallest relevant plan. Completed plans are historical evidence;
read them for a specific decision or regression, not as routine startup
context. Search current code and reference docs first when establishing
present behavior, and narrow archive searches to the relevant topic.

## When To Create A Plan

- Work spans multiple subsystems, commits, or pull requests.
- Validation has meaningful risk, tradeoffs, or deferred follow-ups.
- Future contributors will need reasoning beyond the final diff.

## Plan Lifecycle

- While active, state the problem, constraints, intended outcome, decisions,
  validation status, and unresolved risks. Update the current checkpoint
  instead of appending repetitive session reports.
- On completion, retain a concise outcome, key decisions and rejected
  alternatives, final validation with its limits, and follow-up references.
- Promote still-current invariants into `docs/repo/` or `ARCHITECTURE.md`;
  put unfinished work in a linked active plan or the debt tracker.
- Remove obsolete session instructions, superseded checkpoints, temporary
  command transcripts, and copied code. Record a Git revision when trimming
  substantial history so the original evidence remains recoverable.
- Move the summary into `completed/` and update both directory indexes.
  Prune redundant or superseded plans only after preserving unique reasoning
  and fixing inbound links; age alone is not a reason to delete a plan.

`todo/` is scratch space. Durable plans and execution history belong here;
current reference material belongs in [docs/repo/](../repo/index.md).

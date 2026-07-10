---
name: execute-roadmap
description: >-
  Continue the OpenLiveReplay broadcast-perfection execution loop. Invoke to
  advance the roadmap from wherever it left off: sync, reconcile directives,
  compute the frontier, drive ready initiatives to shipped through /workflows
  (implement test-first in isolated worktrees + independent review + guarded
  self-merge), always keeping docs/broadcast-plan/program-state.json current and
  the audit green. Use when asked to "run/continue/advance the loop", "execute the
  roadmap", "work the next initiative(s)", or "drive the broadcast plan".
---

# Execute the broadcast-perfection roadmap (one iteration)

This skill is the executable form of
[`docs/broadcast-plan/operating-loop.md`](../../../docs/broadcast-plan/operating-loop.md).
One invocation advances the roadmap by **one iteration** — it selects up to the WIP
cap of ready work, drives each initiative to `shipped` via `/workflows`, and records
progress. It is **idempotent and resumable**: it reads the live state and picks up
wherever the last run stopped, so you can invoke it at any point to continue.

**Never** hand-edit `program-state.json`; always transition through
`tools/roadmap/state.py` (it rolls back any transition that would fail the audit).
**Never** merge except through `tools/roadmap/merge_guard.sh`. Read
`docs/broadcast-plan/directives.md` first — a human `Stop`/`Revert` overrides
everything below.

## 0. Preconditions & environment check

```sh
git fetch origin
python tools/roadmap/audit.py            # must be clean; if not, FIX THE ROADMAP FIRST (see §6)
python tools/roadmap/state.py frontier   # activePhase, readyInWindow, inProgress, allComplete
```

Detect the environment once and record it in your run report:
- **Build-capable** (Qt + ffmpeg/ffprobe/srt on PATH) and **gh-authenticated** →
  run the full loop including implement/CI/merge.
- **Otherwise** (e.g. a docs-only box) → do the always-safe parts (sync, directives,
  frontier, re-planning, state hygiene) and **stop before implementation**, reporting
  exactly which initiatives are ready and that they need a build-capable runner. Do
  not fake build/test/merge evidence.

## 1. Reconcile directives

Read `docs/broadcast-plan/directives.md` top-to-bottom. Obey the newest applicable
entry; append a `Resolution` line to each open one. A `Stop <scope>` removes that
scope from selection this iteration; a `Revert <pr>` is actioned first (open a
`git revert` PR through the normal gate — revert-first is always in-policy).

## 2. Compute the frontier & select work

From `python tools/roadmap/state.py frontier`:
- If `allComplete: true` → the program is done; report and stop.
- If `readyInWindow` is empty but work remains → **phase-exit re-plan** (§5), then
  recompute.
- Otherwise pick up to **3 total in-flight** (WIP cap) from `readyInWindow`, with
  **disjoint write scopes**: compare each candidate's impl `Files` list (in
  `docs/broadcast-plan/impl/phase-N-*.md`) against every in-flight PR and every other
  candidate — never select two that touch the same files. Honour the **single
  advance slot** (`readyOutOfWindow` is off-limits; at most one pick may be from the
  active phase + 1). Skip anything under a directive `Stop`.

For each selected initiative, open its impl-plan section and confirm the `Files`
list and slices still match the live code (re-slice if the code moved).

## 3. Drive implementation via /workflows

Use the **Workflow tool** with the template
[`references/implement-initiative.js`](references/implement-initiative.js). Pass the
selected initiatives (id, title, size, `Files`, slices, acceptance criteria,
impl-plan path) as `args`. The workflow, per the model routing
(Opus for XL/XXL & safety-critical and for reviews; default tier for S/M):

- **Implement** each initiative in an **isolated worktree** (`isolation: 'worktree'`)
  so disjoint initiatives run in parallel without collision. Test-first per slice
  (RED → GREEN → refactor), one commit per slice inside the declared `Files`; runs
  the local gate; **pushes its branch** and opens a **draft PR**; returns the branch,
  PR, files-touched and gate results.
- **Independently review** each resulting diff in a **fresh context** (a different
  agent than the implementer; a security pass for auth/ingest/I-O/rendering), returns
  a verdict + findings.

Before/at launch, transition each initiative:
`python tools/roadmap/state.py set <id> --status implementing --pr <draft-pr-url>`
(the audit requires a `pr` for `implementing`).

Keep the state-changing git/gh side effects (label, merge, worktree cleanup) in the
**main loop**, not inside subagents — see §4. Do NOT invoke the Workflow tool unless
this is a build-capable, opted-in run (workflows spawn many agents).

## 4. Self-merge (guarded) & record state

For each initiative whose review has **zero unresolved Critical/Important findings**:

1. Verify diff scope: `gh pr diff <pr>` touches only the declared `Files`.
2. Wait for the required **"CI gate"** to go green: `gh pr checks <pr>` / `gh run watch`.
3. The independent reviewer (not the implementer) applies the artifact:
   `gh pr edit <pr> --add-label independent-reviewed`.
4. Merge through the guard **only**: `bash tools/roadmap/merge_guard.sh <pr>`
   (it re-checks CI-green + the label, then `gh pr merge --merge --delete-branch`).
   Raw `gh pr merge` / `--admin` / `--no-verify` / force-push are denied by
   `.claude/settings.json`.
5. Record: `python tools/roadmap/state.py set <id> --status shipped --pr <pr> --evidence <ci-run-or-measurement-url>`
   then remove the worktree.

If a review finds Critical/Important issues, either iterate (Codex→Opus escalates
after two failed reviews) or **revert-first** and set the state back
(`--status approved`), then re-plan.

## 5. Phase-exit re-plan (only when `readyInWindow` is empty)

Run the active phase's exit criteria (in `phase-N-*.md`). Then re-audit and re-slice
the **next** phase file against the live codebase and flip its impl plan from
`<!-- draft -->` to full task stacks + PR-slice tables for its XL/XXL initiatives.
Run `python tools/roadmap/audit.py` to confirm still-green, then recompute the
frontier and continue by default (no human gate unless a directive says so).

## 6. If the audit is red

The roadmap itself is malformed — fix that before any execution. Common fixes:
add a missing registry row/definition, resolve a bad dependency, add an `impl/`
task heading, or run `python tools/roadmap/state.py sync` after adding an initiative
(it preserves progress). Re-run `python tools/roadmap/audit.py` until clean.

## 7. Report & continue

End every invocation with: environment mode; directives handled; what advanced
(id → new status, with PR/evidence links); the new `readyInWindow`; and the next
initiative to pick up. Invoke this skill again to continue — it resumes from the
updated state.

## Guardrails (always)

- Update state **only** via `state.py`; keep the audit green at all times.
- Merge **only** via `merge_guard.sh`; the independent review label is applied by a
  different agent than the implementer.
- **Never without a directive:** production deploys/migrations, spending, external
  publishing, deleting evidence, or overriding a failed safety/security gate.
- Reverting a flagged slice is always in-policy and needs no directive.

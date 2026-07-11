# Operating loop — the runbook

How the autonomous execution model actually runs. The [index](./README.md) is the
"what"; this is the "how". It is specific to this repository's CI, hooks, branch
protection and merge method (all verified — see the index "Repository facts").

**Run it:** invoke the **`execute-roadmap`** skill
([`.claude/skills/execute-roadmap/`](../../.claude/skills/execute-roadmap/SKILL.md)) —
the executable form of this runbook. One invocation advances the roadmap by one
iteration and resumes from the live state, so you can call it at any point to
continue. It transitions state only through
[`tools/roadmap/state.py`](../../tools/roadmap/state.py) (which rolls back any
transition that would fail the audit) and drives implementation through `/workflows`.

## Roles

- **Agents** execute non-stop: they compute the frontier, select ready work, plan,
  implement test-first in isolated worktrees, self-merge behind the gate, record
  state, and continue. They never wait for the human between merged results.
- **The human** evaluates merged results asynchronously and steers **by exception**
  through [`directives.md`](./directives.md). Silence means continue.
- **External-gate owners** handle only the enumerated human-bound items (usability
  studies, hardware/interop labs, deploys, spending) — see the evidence ladder.

## The loop

1. **Sync.** `git fetch origin`; base new work on `origin/main`.
2. **Reconcile directives.** Read [`directives.md`](./directives.md) top-to-bottom.
   Obey the newest applicable entry; append a `Resolution` line to each open one.
   A `Stop` in scope removes that scope from selection; a `Revert` is actioned
   immediately (revert-first is always in-policy).
3. **Audit + frontier.** `python tools/roadmap/audit.py --frontier`. A non-zero
   exit means the roadmap is malformed — fix that before anything else. On success
   it prints `{activePhase, ready, readyInWindow, readyOutOfWindow, inProgress,
   blocked, allComplete}`. `ready` is every initiative whose dependencies are all
   `shipped` (approval does not gate readiness); `readyInWindow` restricts that to
   the active phase **plus one advance-slot phase** — select only from there.
4. **Select work.** From `readyInWindow`, respect: **WIP cap 3**; **disjoint write
   scopes** (compare the candidate's impl `Files` list against in-flight PRs — never
   claim overlapping files); at most **one advance slot** from the next phase
   (`readyOutOfWindow` is off-limits). Open a **draft PR first** so a `pr` link
   exists, then set the initiative's state to `implementing` (rule 12 requires a
   `pr` for `implementing`; `evidence` is required only at `measuring`/`shipped`).
5. **Plan per initiative.** Open the phase impl plan. If the plan is a draft
   (later phase pulled via the advance slot), first re-audit and slice it against
   the live code. Confirm the `Files` list still matches reality.
6. **Implement per slice, test-first, in an isolated worktree.**
   `git worktree add .claude/worktrees/<branch> -b <branch> origin/main` (prefix
   `agent/` or `codex/` per the router). For each PR slice: **RED** (write the
   failing test named in the acceptance criteria) → **GREEN** (make it pass) →
   **REFACTOR** → commit (one logical commit per slice; keep the diff inside the
   declared Files list). Slices — not initiatives — are the PR unit.
7. **Self-merge** (protocol below). On merge, set the state entry to `shipped` with
   the `pr` and an `evidence` link (CI run / measurement) by **editing the entry
   directly** — `--emit-state` preserves existing status/pr/evidence, so it never
   wipes a shipped entry (rule 12 rejects `shipped` without both links).
8. **Continue.** Recompute the frontier and take the next `readyInWindow` item.

**`readyInWindow` empty ⇒ advance the phase.** When no in-window item is ready or
in progress but work remains (`allComplete` is false), the active phase's reachable
work is done: run its exit criteria, then re-audit / re-slice / re-estimate the next
phase file against the live codebase and flip its impl plan from `<!-- draft -->` to
full (which promotes its items into the window). Proceed by default — no human gate
unless a directive says otherwise. Only `allComplete: true` means the program is
finished.

## Self-merge protocol (adapted to this repo)

Branch protection requires exactly one check — **"CI gate"** — with
`enforce_admins` on, no required human review, and repo auto-merge **disabled**. So:

1. **Local gate green.** Run the applicable [`.githooks/pre-push`](../../.githooks/pre-push)
   gate for the change (docs-only changes take its fast path and still run
   `python tools/roadmap/audit.py`). Never `--no-verify`.
2. **Push + open PR** to `origin/main`. Authenticate via gh's credential helper so
   the hook runs (see CLAUDE.md).
3. **CI gate green.** Poll `gh pr checks <pr>` / `gh run watch` until the required
   **"CI gate"** context is green. Do not use `--admin` (admins are enforced too).
4. **Diff-scope verification.** `gh pr diff` touches **only** the initiative's
   declared `Files` list. A stray file fails the check — fix or split.
5. **Independent review in a fresh context.** A *different* agent than the
   implementer reviews the PR from a clean context (the implementer never reviews
   its own work). For any change to **auth, ingest/parsing, I/O, or rendering
   surfaces**, add a security-focused review pass. With **zero unresolved Critical
   or Important findings**, the reviewer applies the **`independent-reviewed`**
   label — the enforced artifact. The implementer must not self-apply it.
6. **Rollback recorded.** The PR body names the revert (the merged slice is
   revertible; note it in the state `evidence`).
7. **Merge — through the guard only.** `bash tools/roadmap/merge_guard.sh <pr>`.
   The guard refuses to merge unless the required **"CI gate"** is green **and** the
   `independent-reviewed` label is present, then does `gh pr merge --merge
   --delete-branch`. Raw `gh pr merge` is denied to loop agents
   ([`.claude/settings.json`](../../.claude/settings.json)); never `--admin` /
   `--no-verify`.
8. **Remove the worktree** (`git worktree remove --force …`) and record `shipped`.

### Enforcement backstops (what actually blocks a bad merge)

The self-merge safety rests on layered enforcement, strongest first:

- **GitHub branch protection (verified, hard):** the required **"CI gate"** context
  (which includes the always-run `docs-audit`), `enforce_admins` **on** (so
  `--admin` cannot bypass the gate), and `allow_force_pushes` **off** (no force-push
  to `main`). A red gate — including a malformed roadmap — cannot merge, period.
- **The merge guard (loop-side):** enforces the one thing GitHub cannot — that an
  independent review happened — plus the permission `deny` list that blocks raw
  merge / `--admin` / `--no-verify` / force-push / `gh api` writes.
- **Do NOT enable "required approvals" branch protection on this solo repo — it
  deadlocks the loop.** GitHub forbids a PR author from approving their own PR, and
  `enforce_admins` is on, so a single-identity maintainer could never satisfy the
  check and *nothing* could merge (not even via the UI). Independent-review
  enforcement here is therefore the **merge guard** (it refuses to merge without the
  `independent-reviewed` label) layered on the GitHub backstops above — never a
  required-review toggle. Making review enforceable by a party *other than the loop*
  would require a **bot / GitHub-App reviewer** wired to the merge guard (a separate,
  opt-in second identity) — that is the only form of "required review" that does not
  deadlock a single-identity repo.

## Revert-first

Reverting a flagged slice is **always in-policy and needs no directive**. If a
merged slice causes a regression, a soak failure, or a review escalation, revert it
first (a clean `git revert` PR through the same gate), then re-plan. A `Revert`
directive is a request to do exactly this.

## Never without a directive

Do **not**, absent an explicit [`directives.md`](./directives.md) entry:

- deploy to production, run a data/schema migration, or cut a release;
- spend money or provision paid infrastructure;
- publish to any external service (package registries, app stores, social, the NDI
  runtime distribution, a public artifact);
- delete evidence (CI logs, measurement traces, soak artifacts, state history);
- override a failed safety or security gate (sanitizer, fuzz crash, control-plane
  auth, the CI gate) or merge with `--admin`.

Everything else — coding, testing, refactoring, self-merging behind a green gate,
reverting — proceeds by default.

## Model routing

Size labels (S/M/L/XL/XXL) are the default trigger.

| Work | Model |
|------|-------|
| Advisory / hardest design challenges / phase-exit + re-planning reviews | **Fable 5** |
| Loop orchestration; independent reviews; hard coding (XL/XXL, or safety-critical: auth, ingest, threading, GPU, I/O) | **Opus** |
| Average & bulk coding (S/M) | **Codex** (via `/codex`) — never reviewed by Codex itself |

**Escalation:** Codex → Opus after **two** failed reviews on the same slice; Opus →
Fable 5 for a design consult when the approach is contested. **De-escalate** back to
Codex once the design is pinned. Record any escalation and the decision in the impl
plan or a short ADR under `docs/superpowers/specs/`.

## WIP, advance slot & write scope

- **WIP cap = 3** concurrent `implementing`/`measuring` initiatives.
- **Advance slot:** at most one in-flight item may come from the phase after the
  active phase, and only if its dependencies are shipped.
- **Disjoint write scopes:** two concurrent initiatives must not list overlapping
  `Files`. When the impl `Files` lists intersect, serialise them.

## Evidence ladder (human-bound validation never deadlocks a phase)

- **Directional** evidence (a quick internal read, a synthetic measurement) gates
  *iteration* — agents produce it.
- **Expert** evidence (a trained operator or a hardware-lab bench check) gates a
  *phase exit* — an external-gate owner produces it; the loop keeps working other
  ready initiatives while it is pending.
- **Full** evidence (a formal usability study, a certification run on real gear)
  gates the *final release*.

External evidence gates are marked as such in the impl plans with a **named owner**
and are never written as agent-executable checkbox steps. A pending external gate
blocks only the initiatives that depend on it, never the whole phase.

---

[Index](./README.md) · [Directives](./directives.md) · [Program state](./program-state.json)

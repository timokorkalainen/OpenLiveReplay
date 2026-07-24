# Agent Instructions

Follow [CLAUDE.md](CLAUDE.md) for the canonical repository workflow, public-repo
constraints, branch policy, pre-push policy, and PR expectations.

## OpenAI Codex scope-control gates

This section applies only when the acting agent is **OpenAI Codex**. It does not
change the Claude workflow in `CLAUDE.md`. These are hard stops for every
Codex-authored change, including one-off tasks, roadmap work, and delegated
subagent work. "Stop" means make no further implementation edits until the user
explicitly approves the expanded scope.

Before implementation, Codex must establish a scope contract containing:

- the requested outcome, allowed files or globs, and explicit non-goals;
- an estimated budget for changed lines, files, and non-merge commits;
- whether any new test infrastructure or developer tooling is permitted.

Unless the user approves different limits, stop before exceeding **1,000 changed
lines (additions plus deletions)**, **20 files**, or **10 non-merge commits**. For
roadmap work, the initiative and its declared `Files` must exist and the roadmap
audit must pass before implementation starts. Missing or undefined roadmap scope
is a stop condition, never permission to proceed.

Approval must name the affected budget or prohibited tool explicitly. General
requests such as "be thorough", "spare no expense", "take no shortcuts", or
"make it better" do not authorize scope expansion. Delegation does not reset or
partition these limits; Codex remains responsible for the aggregate branch.

Codex must measure the branch against its starting base SHA after every five
commits and before each readiness claim. Report additions and deletions grouped
as production, tests, tooling, and documentation. Stop and ask for approval when:

- any budget is reached or the work grows beyond twice its estimate;
- test, tooling, fixture, or support-code growth exceeds production-code growth;
- the diff introduces a parser, preprocessor, compiler wrapper, cache,
  calibration system, process supervisor, test runner, policy engine, static
  analyzer, or other custom verification framework;
- two consecutive commits primarily repair newly added support machinery rather
  than the requested product behavior.

Prefer deletion and existing language, compiler, CMake/CTest, and repository
facilities over new infrastructure. Custom verification machinery requires
advance user approval and must be isolated in a separate PR unless the user
explicitly approves including it.

Every added test must be registered in a local or CI command that actually runs
it. Codex must list that command and verify collection/execution counts. Running
one selected smoke method does not justify an otherwise unexecuted test module
or framework. Do not add dormant test suites.

An independent-review claim requires a durable GitHub review/comment or tracked
report tied to the exact reviewed commit SHA. The artifact must cover correctness,
scope, proportionality, maintainability, dead code, and whether added tests
actually execute. Do not apply an `independent-reviewed` label or report
"Critical 0 / Important 0 / Minor 0" without linking that artifact.

Before describing a Codex-authored PR as ready, its body must disclose:

- changed-line totals grouped as production, tests, tooling, and documentation;
- file and non-merge commit counts;
- all custom infrastructure and why existing facilities were insufficient;
- the exact test commands and any tests not executed;
- every approved scope or budget deviation.

Passing CI proves the checked behavior, not that the change is appropriately
scoped. Codex must not claim "production-ready", recommend merge, or continue
ambiguous work when any gate above lacks evidence. Ambiguity is a stop condition:
report it and ask the user.

For long-running, agent-driven development, this repo runs a machine-checkable
roadmap + non-stop operating loop. Start at
[docs/broadcast-plan/README.md](docs/broadcast-plan/README.md) (the "what"),
[docs/broadcast-plan/operating-loop.md](docs/broadcast-plan/operating-loop.md)
(the "how"), and steer via
[docs/broadcast-plan/directives.md](docs/broadcast-plan/directives.md). The
roadmap is enforced by `python tools/roadmap/audit.py` (a required CI job); run
`python tools/roadmap/audit.py --frontier` to see the ready work.

Build and run references:

- [Build and Run](docs/build-and-run.md) covers VS Code, terminal desktop,
  tests, and iOS device builds.
- [.vscode/launch.json](.vscode/launch.json) contains the committed VS Code
  debug configurations.
- [.vscode/tasks.json](.vscode/tasks.json) contains the committed VS Code build
  and test tasks.

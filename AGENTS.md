# Agent Instructions

Follow [CLAUDE.md](CLAUDE.md) for the canonical repository workflow, public-repo
constraints, branch policy, pre-push policy, and PR expectations.

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

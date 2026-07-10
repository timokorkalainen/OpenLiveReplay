#!/usr/bin/env python3
"""Atomic program-state.json transitions for the execution loop.

Every mutation re-runs the roadmap audit, so the state file is never left in an
inconsistent shape (an illegal transition -- e.g. `shipped` without evidence, or a
status ahead of an unshipped dependency -- writes but then reports the audit failure
with a non-zero exit). Used by the `execute-roadmap` skill so state updates are
mechanical and always validated.

  python tools/roadmap/state.py frontier                 # ready set (fails if roadmap malformed)
  python tools/roadmap/state.py show [<id>]              # whole frontier, or one entry
  python tools/roadmap/state.py set <id> --status implementing --pr URL
  python tools/roadmap/state.py set <id> --status shipped --pr URL --evidence URL
  python tools/roadmap/state.py sync                     # add new initiatives, preserve progress
"""
from __future__ import annotations
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audit  # noqa: E402


def _state_path(pd: Path) -> Path:
    return pd / "program-state.json"


def _commit_or_rollback(pd: Path, new_text: str, msg: str) -> int:
    """Write new_text, re-run the audit, and ROLL BACK if it fails -- so the state
    file is never left in an inconsistent shape by an illegal transition."""
    path = _state_path(pd)
    old_text = path.read_text(encoding="utf-8") if path.exists() else None
    path.write_text(new_text, encoding="utf-8")
    probs, _, _ = audit.run_rules(pd)
    if probs:
        if old_text is not None:
            path.write_text(old_text, encoding="utf-8")   # roll back
        print(f"{msg}: REJECTED -- this transition is not legal ({len(probs)} audit "
              f"problem(s)); state left unchanged:", file=sys.stderr)
        for p in sorted(str(x) for x in probs):
            print("  " + p, file=sys.stderr)
        return 1
    print(f"{msg}: OK (audit clean)")
    return 0


def cmd_set(args) -> int:
    pd = args.plan_dir.resolve()
    state = json.loads(_state_path(pd).read_text(encoding="utf-8"))
    if args.id not in state:
        print(f"unknown initiative '{args.id}' -- not in program-state.json "
              f"(run `state.py sync` after adding it to the registry)", file=sys.stderr)
        return 2
    entry = state[args.id]
    if not isinstance(entry, dict):
        entry = {"phase": None, "status": "discovery", "pr": None, "evidence": None}
        state[args.id] = entry
    if args.status:
        if args.status not in audit.STATUSES:
            print(f"invalid status '{args.status}' (need one of {sorted(audit.STATUSES)})",
                  file=sys.stderr)
            return 2
        entry["status"] = args.status
    if args.pr is not None:
        entry["pr"] = args.pr or None
    if args.evidence is not None:
        entry["evidence"] = args.evidence or None
    return _commit_or_rollback(pd, json.dumps(state, indent=2) + "\n",
                               f"set {args.id} -> status={entry.get('status')}")


def cmd_sync(args) -> int:
    pd = args.plan_dir.resolve()
    _, inits, _ = audit.run_rules(pd)
    state = audit.emit_state(pd, inits)   # preserves existing progress, adds new as discovery
    return _commit_or_rollback(pd, json.dumps(state, indent=2) + "\n",
                               f"synced {len(state)} initiatives")


def cmd_show(args) -> int:
    pd = args.plan_dir.resolve()
    if args.id:
        state = json.loads(_state_path(pd).read_text(encoding="utf-8"))
        print(json.dumps(state.get(args.id, {"error": "unknown id"}), indent=2))
        return 0
    return cmd_frontier(args)


def cmd_frontier(args) -> int:
    pd = args.plan_dir.resolve()
    probs, inits, _ = audit.run_rules(pd)
    if probs:
        print(f"roadmap audit FAILED ({len(probs)} problem(s)) -- fix the roadmap "
              f"before driving the loop:", file=sys.stderr)
        for p in sorted(str(x) for x in probs):
            print("  " + p, file=sys.stderr)
        return 1
    print(json.dumps(audit.compute_frontier(pd, inits), indent=2))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Atomic, audited program-state transitions.")
    default_plan = Path(__file__).resolve().parents[2] / "docs" / "broadcast-plan"
    ap.add_argument("--plan-dir", type=Path, default=default_plan)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("set", help="update an initiative's status/pr/evidence")
    s.add_argument("id")
    s.add_argument("--status")
    s.add_argument("--pr")
    s.add_argument("--evidence")
    s.set_defaults(fn=cmd_set)
    sh = sub.add_parser("show", help="print the frontier, or one entry with <id>")
    sh.add_argument("id", nargs="?")
    sh.set_defaults(fn=cmd_show)
    sy = sub.add_parser("sync", help="regenerate state preserving progress (add new initiatives)")
    sy.set_defaults(fn=cmd_sync)
    fr = sub.add_parser("frontier", help="print the ready set (JSON)")
    fr.set_defaults(fn=cmd_frontier)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())

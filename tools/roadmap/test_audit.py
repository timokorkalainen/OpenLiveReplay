#!/usr/bin/env python3
"""Per-rule tests for tools/roadmap/audit.py.

Each test starts from a minimal VALID roadmap fixture, then injects exactly one
defect and asserts the matching rule fires. The `test_valid_fixture_is_clean`
test is the linchpin: it proves the audit has no false positives on a well-formed
plan. Run with:  python -m unittest -v   (from tools/roadmap/)
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audit  # noqa: E402

INDEX = """# Demo Plan

## Quality gates

### QG-DEMO-ONE — demo latency
Target: end-to-end under 10 ms.

## Initiative registry

| ID          | Phase | Size | Summary       |
|-------------|-------|------|---------------|
| `aa-root`   | 1     | M    | the root task |
| `bb-big`    | 1     | XL   | the big task  |

## Links

- [phase 1](./phase-1-demo.md)
- [impl](./impl/phase-1-demo.md)
"""

PHASE1 = """# Phase 1 - Demo

### `aa-root` — Root
- **Size:** M
- **Dependencies:** none
- **Definition:** do the root thing so downstream work can start.
- **Acceptance criteria:**
  - [ ] meets QG-DEMO-ONE
  - [ ] a unit test proves the root behaviour

### `bb-big` — Big
- **Size:** XL
- **Dependencies:** aa-root
- **Definition:** do the big thing on top of the root.
- **Acceptance criteria:**
  - [ ] the big thing works end to end
"""

IMPL1 = """# Phase 1 - Implementation

### `aa-root` — Root tasks
RED: add a failing test. GREEN: implement. Commit boundary: one commit.

### `bb-big` — Big tasks
Task stack for the big thing.

#### PR slices: `bb-big`
| Slice | Scope | Proof | Rollback / evidence | Merge prerequisite |
|-------|-------|-------|---------------------|--------------------|
| 1     | part a | test x | revert slice | none |
| 2     | part b | test y | revert slice | slice 1 |
"""

STATE = {
    "aa-root": {"phase": 1, "status": "approved", "pr": None, "evidence": None},
    "bb-big": {"phase": 1, "status": "discovery", "pr": None, "evidence": None},
}


def build_valid(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    (root / "README.md").write_text(INDEX, encoding="utf-8")
    (root / "phase-1-demo.md").write_text(PHASE1, encoding="utf-8")
    (root / "impl").mkdir(exist_ok=True)
    (root / "impl" / "phase-1-demo.md").write_text(IMPL1, encoding="utf-8")
    (root / "program-state.json").write_text(json.dumps(STATE, indent=2), encoding="utf-8")
    return root


def fired(plan_dir: Path):
    problems, _, _ = audit.run_rules(plan_dir)
    return {p.rule for p in problems}, problems


class RoadmapAuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.plan = build_valid(Path(self.tmp.name) / "plan")

    def tearDown(self):
        self.tmp.cleanup()

    # -- the linchpin: no false positives ---------------------------------- #
    def test_valid_fixture_is_clean(self):
        rules, problems = fired(self.plan)
        self.assertEqual(problems, [], f"unexpected problems on a valid plan: {[str(p) for p in problems]}")

    # -- one test per rule ------------------------------------------------- #
    def test_unique_ids(self):
        p = self.plan / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8") +
                     "\n### `aa-root` — Duplicate\n- **Size:** S\n- **Dependencies:** none\n"
                     "- **Definition:** dup.\n- **Acceptance criteria:**\n  - [ ] x\n", encoding="utf-8")
        self.assertIn("unique-ids", fired(self.plan)[0])

    def test_deps_present_missing(self):
        p = self.plan / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace("- **Dependencies:** aa-root\n", ""), encoding="utf-8")
        self.assertIn("deps-present", fired(self.plan)[0])

    def test_deps_present_duplicated(self):
        p = self.plan / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace(
            "- **Dependencies:** aa-root\n",
            "- **Dependencies:** aa-root\n- **Dependencies:** aa-root\n"), encoding="utf-8")
        self.assertIn("deps-present", fired(self.plan)[0])

    def test_deps_resolvable(self):
        p = self.plan / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace(
            "- **Dependencies:** aa-root\n", "- **Dependencies:** zz-missing\n"), encoding="utf-8")
        self.assertIn("deps-resolvable", fired(self.plan)[0])

    def test_deps_acyclic(self):
        p = self.plan / "phase-1-demo.md"
        # make aa-root depend on bb-big (bb-big already depends on aa-root)
        p.write_text(p.read_text(encoding="utf-8").replace(
            "### `aa-root` — Root\n- **Size:** M\n- **Dependencies:** none\n",
            "### `aa-root` — Root\n- **Size:** M\n- **Dependencies:** bb-big\n"), encoding="utf-8")
        self.assertIn("deps-acyclic", fired(self.plan)[0])

    def test_deps_not_later_phase(self):
        (self.plan / "phase-2-demo.md").write_text(
            "# Phase 2\n\n### `cc-late` — Late\n- **Size:** M\n- **Dependencies:** none\n"
            "- **Definition:** later work.\n- **Acceptance criteria:**\n  - [ ] x\n", encoding="utf-8")
        p = self.plan / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace(
            "### `aa-root` — Root\n- **Size:** M\n- **Dependencies:** none\n",
            "### `aa-root` — Root\n- **Size:** M\n- **Dependencies:** cc-late\n"), encoding="utf-8")
        self.assertIn("deps-not-later-phase", fired(self.plan)[0])

    def test_registry_agreement_size(self):
        p = self.plan / "README.md"
        p.write_text(p.read_text(encoding="utf-8").replace("| `aa-root`   | 1     | M    |",
                                                           "| `aa-root`   | 1     | S    |"), encoding="utf-8")
        self.assertIn("registry-agreement", fired(self.plan)[0])

    def test_registry_agreement_missing(self):
        p = self.plan / "README.md"
        p.write_text(p.read_text(encoding="utf-8").replace(
            "| `bb-big`    | 1     | XL   | the big task  |\n", ""), encoding="utf-8")
        self.assertIn("registry-agreement", fired(self.plan)[0])

    def test_impl_coverage(self):
        p = self.plan / "impl" / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace("### `aa-root` — Root tasks", "### Root tasks"),
                     encoding="utf-8")
        self.assertIn("impl-coverage", fired(self.plan)[0])

    def test_slice_tables(self):
        p = self.plan / "impl" / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace("#### PR slices: `bb-big`", "#### Notes for bb-big"),
                     encoding="utf-8")
        self.assertIn("slice-tables", fired(self.plan)[0])

    def test_slice_tables_waived_for_draft(self):
        p = self.plan / "impl" / "phase-1-demo.md"
        txt = p.read_text(encoding="utf-8").replace("#### PR slices: `bb-big`", "#### Notes for bb-big")
        p.write_text(audit.DRAFT_MARKER + "\n" + txt, encoding="utf-8")
        self.assertNotIn("slice-tables", fired(self.plan)[0])

    def test_qg_refs_resolve(self):
        p = self.plan / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace("meets QG-DEMO-ONE", "meets QG-DOES-NOT-EXIST"),
                     encoding="utf-8")
        self.assertIn("qg-refs-resolve", fired(self.plan)[0])

    def test_qg_has_number(self):
        p = self.plan / "README.md"
        p.write_text(p.read_text(encoding="utf-8").replace(
            "### QG-DEMO-ONE — demo latency\nTarget: end-to-end under 10 ms.",
            "### QG-DEMO-ONE — demo latency\nTarget: fast enough."), encoding="utf-8")
        self.assertIn("qg-has-number", fired(self.plan)[0])

    def test_links_resolve(self):
        p = self.plan / "README.md"
        p.write_text(p.read_text(encoding="utf-8") + "\n[broken](./does-not-exist.md)\n", encoding="utf-8")
        self.assertIn("links-resolve", fired(self.plan)[0])

    def test_links_resolve_bad_anchor(self):
        p = self.plan / "README.md"
        p.write_text(p.read_text(encoding="utf-8").replace(
            "[phase 1](./phase-1-demo.md)", "[phase 1](./phase-1-demo.md#no-such-heading)"), encoding="utf-8")
        self.assertIn("links-resolve", fired(self.plan)[0])

    def test_word_budget(self):
        original = dict(audit.WORD_BUDGET)
        audit.WORD_BUDGET["phase"] = 5
        try:
            self.assertIn("word-budget", fired(self.plan)[0])
        finally:
            audit.WORD_BUDGET.clear()
            audit.WORD_BUDGET.update(original)

    def test_size_valid(self):
        p = self.plan / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace("- **Size:** M\n", "- **Size:** Z\n"), encoding="utf-8")
        self.assertIn("size-valid", fired(self.plan)[0])

    def test_acceptance_present(self):
        p = self.plan / "phase-1-demo.md"
        txt = p.read_text(encoding="utf-8")
        txt = txt.replace("  - [ ] the big thing works end to end\n", "")
        p.write_text(txt, encoding="utf-8")
        self.assertIn("acceptance-present", fired(self.plan)[0])

    def test_state_missing_entry(self):
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        del state["bb-big"]
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        self.assertIn("state-consistency", fired(self.plan)[0])

    def test_state_status_needs_evidence(self):
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["bb-big"]["status"] = "implementing"  # no pr/evidence
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        self.assertIn("state-consistency", fired(self.plan)[0])

    def test_state_invalid_status(self):
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["bb-big"]["status"] = "in-progress"  # not a valid enum value
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        self.assertIn("state-consistency", fired(self.plan)[0])

    # -- new rules / regressions from the independent review -------------- #
    def test_deps_parsed_from_sub_bullets(self):  # C2
        p = self.plan / "phase-1-demo.md"
        p.write_text(p.read_text(encoding="utf-8").replace(
            "- **Dependencies:** aa-root\n",
            "- **Dependencies:**\n  - `zz-missing`\n"), encoding="utf-8")
        # deps must be parsed from the body block, so the bad dep is caught:
        self.assertIn("deps-resolvable", fired(self.plan)[0])

    def test_deps_completion_order(self):  # C3
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["aa-root"]["status"] = "approved"  # dependency NOT shipped
        state["bb-big"].update(status="shipped", pr="https://x/pr/9", evidence="https://x/run/9")
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        self.assertIn("deps-completion-order", fired(self.plan)[0])

    def test_state_non_dict_entry_no_crash(self):  # I5
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["bb-big"] = "approved"  # a string, not an object
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        rules, _ = fired(self.plan)  # must not raise
        self.assertIn("state-consistency", rules)

    def test_registry_short_row_no_crash(self):  # I6
        idx = ("## Initiative registry\n| Phase | ID | Size |\n|---|---|---|\n"
               "| 1 |\n| 1 | `aa-root` | M |\n")
        reg, _ = audit.parse_registry(idx)  # id not at column 0 + a short row
        self.assertIn("aa-root", reg.rows)

    def test_implementing_needs_only_pr(self):  # I1
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["aa-root"].update(status="shipped", pr="https://x/pr/1", evidence="https://x/run/1")
        state["bb-big"].update(status="implementing", pr="https://x/pr/2")  # pr but no evidence
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        self.assertEqual(fired(self.plan)[1], [])  # implementing with a pr and no evidence is clean

    def test_measuring_needs_evidence(self):  # I1
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["aa-root"].update(status="shipped", pr="https://x/pr/1", evidence="https://x/run/1")
        state["bb-big"].update(status="measuring", pr="https://x/pr/2")  # no evidence
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        self.assertIn("state-consistency", fired(self.plan)[0])

    def test_slice_required_once_started(self):  # I7
        imp = self.plan / "impl" / "phase-1-demo.md"
        txt = imp.read_text(encoding="utf-8").replace("#### PR slices: `bb-big`", "#### Notes for bb-big")
        imp.write_text(audit.DRAFT_MARKER + "\n" + txt, encoding="utf-8")  # draft would waive...
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["aa-root"].update(status="shipped", pr="https://x/pr/1", evidence="https://x/run/1")
        state["bb-big"].update(status="implementing", pr="https://x/pr/2")  # ...but it has started
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        self.assertIn("slice-tables", fired(self.plan)[0])

    # -- CLI-level behaviours --------------------------------------------- #
    def test_emit_state_seeds_from_scratch_as_discovery(self):
        (self.plan / "program-state.json").unlink()  # no prior state
        _, inits, _ = audit.run_rules(self.plan)
        state = audit.emit_state(self.plan, inits)
        self.assertEqual(state["aa-root"]["status"], "discovery")
        self.assertEqual(state["bb-big"]["status"], "discovery")

    def test_emit_state_preserves_progress(self):  # I2 - never wipes shipped
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["aa-root"].update(status="shipped", pr="https://x/pr/1", evidence="https://x/run/1")
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        _, inits, _ = audit.run_rules(self.plan)
        regen = audit.emit_state(self.plan, inits)
        self.assertEqual(regen["aa-root"]["status"], "shipped")
        self.assertEqual(regen["aa-root"]["pr"], "https://x/pr/1")
        self.assertEqual(regen["aa-root"]["evidence"], "https://x/run/1")

    def test_frontier_discovery_with_deps_shipped_is_ready(self):  # C1
        state = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        state["aa-root"].update(status="shipped", pr="https://x/pr/1", evidence="https://x/run/1")
        state["bb-big"]["status"] = "discovery"  # NOT approved
        (self.plan / "program-state.json").write_text(json.dumps(state), encoding="utf-8")
        _, inits, _ = audit.run_rules(self.plan)
        fr = audit.compute_frontier(self.plan, inits)
        self.assertEqual(fr["activePhase"], 1)
        self.assertIn("bb-big", fr["ready"])  # ready == deps shipped, regardless of approval
        self.assertFalse(fr["allComplete"])

    def test_frontier_window_excludes_far_phase(self):  # advance-slot discipline
        (self.plan / "phase-3-demo.md").write_text(
            "# Phase 3\n\n### `dd-far` — Far\n- **Size:** M\n- **Dependencies:** none\n"
            "- **Definition:** far-phase work.\n- **Acceptance criteria:**\n  - [ ] x\n", encoding="utf-8")
        idx = self.plan / "README.md"
        idx.write_text(idx.read_text(encoding="utf-8").replace(
            "| `bb-big`    | 1     | XL   | the big task  |\n",
            "| `bb-big`    | 1     | XL   | the big task  |\n| `dd-far`    | 3     | M    | far task      |\n"),
            encoding="utf-8")
        (self.plan / "impl" / "phase-3-demo.md").write_text(
            audit.DRAFT_MARKER + "\n### `dd-far` — Far tasks\ndraft\n", encoding="utf-8")
        st = json.loads((self.plan / "program-state.json").read_text(encoding="utf-8"))
        st["dd-far"] = {"phase": 3, "status": "discovery", "pr": None, "evidence": None}
        (self.plan / "program-state.json").write_text(json.dumps(st), encoding="utf-8")
        self.assertEqual(fired(self.plan)[1], [])  # still a valid plan
        _, inits, _ = audit.run_rules(self.plan)
        fr = audit.compute_frontier(self.plan, inits)
        self.assertIn("dd-far", fr["ready"])             # dependency-ready
        self.assertIn("dd-far", fr["readyOutOfWindow"])  # but phase 3 > active(1)+1
        self.assertNotIn("dd-far", fr["readyInWindow"])


if __name__ == "__main__":
    unittest.main()

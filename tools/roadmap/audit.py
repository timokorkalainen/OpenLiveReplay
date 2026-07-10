#!/usr/bin/env python3
"""Structural audit for the OpenLiveReplay Broadcast-Perfection roadmap.

This is the self-enforcing lane for the autonomous execution model
(docs/broadcast-plan/). It parses the canonical index, the phase files, the
implementation plans and the machine-readable state, and fails fast (< 1 s) when
any structural rule is broken so a malformed roadmap can never merge.

Usage:
    python tools/roadmap/audit.py [--plan-dir DIR]   # audit (exit 1 on any error)
    python tools/roadmap/audit.py --frontier         # audit, then print the ready set as JSON
    python tools/roadmap/audit.py --emit-state       # (re)generate program-state.json from the docs

The rules are documented in docs/broadcast-plan/README.md ("The audit lane").
Every rule has a test in tools/roadmap/test_audit.py proving it detects its
defect class. Pure stdlib; runs on any Python 3.8+ with no build required.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

# --------------------------------------------------------------------------- #
# Configuration
# --------------------------------------------------------------------------- #
ID_RE = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)+")
QG_RE = re.compile(r"QG-[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*")
SIZES = {"S", "M", "L", "XL", "XXL"}
BIG_SIZES = {"XL", "XXL"}
STATUSES = {"discovery", "approved", "implementing", "measuring", "shipped",
            "stopped", "superseded"}
STATUSES_STARTED = {"implementing", "measuring", "shipped"}
STATUSES_NEED_PR = {"implementing", "measuring", "shipped"}      # a (draft) PR must exist
STATUSES_NEED_EVIDENCE = {"measuring", "shipped"}                # CI/measurement evidence must exist
STATUSES_TERMINAL = {"shipped", "stopped", "superseded"}
STATUSES_UNSTARTED = {"discovery", "approved"}
WORD_BUDGET = {"index": 4000, "phase": 3500}
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
DRAFT_MARKER = "<!-- draft -->"


@dataclass
class Initiative:
    iid: str
    title: str
    phase: int
    size: str
    deps: List[str]
    dep_field_count: int
    definition: str
    acceptance: List[str]
    qg_refs: Set[str]
    source: str  # file:line


@dataclass
class Registry:
    rows: Dict[str, dict] = field(default_factory=dict)  # id -> {phase,size,summary}


@dataclass
class Problem:
    rule: str
    message: str

    def __str__(self) -> str:  # pragma: no cover - formatting only
        return f"[{self.rule}] {self.message}"


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #
def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _word_count(text: str) -> int:
    # Strip fenced code blocks and HTML comments so prose budget is what counts.
    text = re.sub(r"```.*?```", " ", text, flags=re.DOTALL)
    text = re.sub(r"<!--.*?-->", " ", text, flags=re.DOTALL)
    return len(text.split())


def _slug(heading: str) -> str:
    s = heading.strip().lower()
    s = s.replace("`", "")
    s = re.sub(r"[^\w\s-]", "", s)
    s = re.sub(r"\s+", "-", s)
    return s


def _within(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def _split_cells(row: str) -> List[str]:
    row = row.strip()
    if row.startswith("|"):
        row = row[1:]
    if row.endswith("|"):
        row = row[:-1]
    return [c.strip() for c in row.split("|")]


# --------------------------------------------------------------------------- #
# Parsers
# --------------------------------------------------------------------------- #
def parse_registry(index_text: str) -> Tuple[Registry, List[Problem]]:
    """Parse the '## Initiative registry' table. Tolerant of padded tables."""
    problems: List[Problem] = []
    reg = Registry()
    lines = index_text.splitlines()
    in_section = False
    header: Optional[List[str]] = None
    col: Dict[str, int] = {}
    for ln in lines:
        if ln.startswith("## "):
            in_section = "initiative registry" in ln.lower()
            header = None
            continue
        if not in_section:
            continue
        if "|" not in ln:
            continue
        cells = _split_cells(ln)
        if header is None:
            header = [c.lower() for c in cells]
            aliases = {"id": {"id"}, "phase": {"phase", "ph", "#"}, "size": {"size", "sz"}}
            for key, al in aliases.items():
                for i, h in enumerate(header):
                    if h in al or h.startswith(key):
                        col[key] = i
                        break
            continue
        if set("".join(cells)) <= set("-: "):  # separator row
            continue
        if "id" not in col or col["id"] >= len(cells):
            continue
        raw_id = cells[col["id"]].strip("` ")
        m = ID_RE.fullmatch(raw_id)
        if not m:
            continue
        phase_txt = cells[col["phase"]] if "phase" in col and col["phase"] < len(cells) else ""
        size_txt = cells[col["size"]].strip() if "size" in col and col["size"] < len(cells) else ""
        pm = re.search(r"\d+", phase_txt)
        summary = cells[-1] if cells else ""
        reg.rows[raw_id] = {
            "phase": int(pm.group()) if pm else None,
            "size": size_txt,
            "summary": summary,
        }
    return reg, problems


def parse_quality_gates(index_text: str) -> Tuple[Dict[str, str], List[Problem]]:
    """Collect QG definitions ('### QG-XXX — ...') from the '## Quality gates' section."""
    problems: List[Problem] = []
    gates: Dict[str, str] = {}
    in_section = False
    current: Optional[str] = None
    body: List[str] = []

    def flush():
        if current is not None:
            gates[current] = "\n".join(body)

    for ln in index_text.splitlines():
        if ln.startswith("## "):
            flush()
            current, body[:] = None, []
            in_section = "quality gate" in ln.lower()
            continue
        if not in_section:
            continue
        h = HEADING_RE.match(ln)
        if h and h.group(1) == "###":
            flush()
            body = []
            m = QG_RE.search(h.group(2))
            current = m.group(0) if m else None
        elif current is not None:
            body.append(ln)
    flush()
    return gates, problems


def parse_phase_file(path: Path) -> Tuple[List[Initiative], List[Problem]]:
    problems: List[Problem] = []
    inits: List[Initiative] = []
    text = _read(path)
    pm = re.search(r"phase-(\d+)", path.name)
    phase = int(pm.group(1)) if pm else 0
    lines = text.splitlines()

    blocks: List[Tuple[str, str, int, List[str]]] = []  # id,title,lineno,body
    cur = None
    for i, ln in enumerate(lines, 1):
        h = HEADING_RE.match(ln)
        if h and h.group(1) == "###":
            title_raw = h.group(2)
            mid = ID_RE.search(title_raw.replace("`", " "))
            # An H3 counts as an initiative only if it starts with an id token.
            first = title_raw.strip().strip("`").split()
            starts_with_id = bool(first) and bool(ID_RE.fullmatch(first[0].strip("`")))
            if starts_with_id and mid:
                if cur:
                    blocks.append(cur)
                iid = first[0].strip("`")
                title = title_raw.split("—", 1)[-1].split(" - ", 1)[-1].strip() \
                    if ("—" in title_raw or " - " in title_raw) else title_raw
                cur = (iid, title, i, [])
                continue
        if h and h.group(1) in ("#", "##"):
            if cur:
                blocks.append(cur)
                cur = None
        if cur:
            cur[3].append(ln)
    if cur:
        blocks.append(cur)

    for iid, title, lineno, body in blocks:
        btext = "\n".join(body)
        size_m = re.search(r"\*\*Size:\*\*\s*([A-Za-z]+)", btext)
        # Count the Dependencies label(s); parse ids from the whole span up to the
        # next **Field:** / heading so sub-bullet or continuation-line deps are not
        # silently dropped (a value-less "- **Dependencies:**\n  - `x`" form).
        dep_field_count = len(re.findall(r"\*\*Dependencies:\*\*", btext))
        def_m = re.search(r"\*\*Definition:\*\*\s*(.+)", btext)
        deps: List[str] = []
        # Span from the label to the next **Field:** (which may carry a "- " list
        # marker), heading, or end-of-block -- so Definition/Acceptance prose (full
        # of hyphenated words) is never mistaken for dependency ids.
        dep_span = re.search(
            r"\*\*Dependencies:\*\*(.*?)(?=\n\s*(?:[-*]\s+)?\*\*[A-Za-z][^*\n]*:\*\*|\n#{1,6}\s|\n\s*---|\Z)",
            btext, flags=re.DOTALL)
        if dep_span:
            deps = list(dict.fromkeys(ID_RE.findall(dep_span.group(1).replace("`", " "))))
        acc = re.findall(r"^\s*[-*]\s*\[[ xX]\]\s*(.+)$", btext, flags=re.MULTILINE)
        qg_refs = set(QG_RE.findall(btext))
        inits.append(Initiative(
            iid=iid, title=title.strip(), phase=phase,
            size=(size_m.group(1) if size_m else "?"),
            deps=deps, dep_field_count=dep_field_count,
            definition=(def_m.group(1).strip() if def_m else ""),
            acceptance=acc, qg_refs=qg_refs,
            source=f"{path.name}:{lineno}",
        ))
    return inits, problems


def parse_impl_files(impl_dir: Path) -> Tuple[Dict[int, bool], Set[str], List[Tuple[str, bool]]]:
    """Return (phase->draft?, headings-with-ids, [(slice-heading-id, draft?)])."""
    draft_by_phase: Dict[int, bool] = {}
    covered_headings: Set[str] = set()
    slice_headings: List[Tuple[str, bool]] = []
    if not impl_dir.is_dir():
        return draft_by_phase, covered_headings, slice_headings
    for path in sorted(impl_dir.glob("phase-*.md")):
        text = _read(path)
        pm = re.search(r"phase-(\d+)", path.name)
        phase = int(pm.group(1)) if pm else 0
        is_draft = DRAFT_MARKER in text
        draft_by_phase[phase] = is_draft
        for ln in text.splitlines():
            h = HEADING_RE.match(ln)
            if not h:
                continue
            htext = h.group(2)
            for tok in ID_RE.findall(htext.replace("`", " ")):
                covered_headings.add(tok)
            if "pr slice" in htext.lower():
                for tok in ID_RE.findall(htext.replace("`", " ")):
                    slice_headings.append((tok, is_draft))
    return draft_by_phase, covered_headings, slice_headings


# --------------------------------------------------------------------------- #
# The rules
# --------------------------------------------------------------------------- #
def run_rules(plan_dir: Path) -> Tuple[List[Problem], Dict[str, Initiative], Registry]:
    problems: List[Problem] = []
    index = plan_dir / "README.md"
    if not index.exists():
        return [Problem("layout", f"canonical index missing: {index}")], {}, Registry()

    index_text = _read(index)
    reg, p = parse_registry(index_text)
    problems += p
    gates, p = parse_quality_gates(index_text)
    problems += p

    phase_files = sorted(plan_dir.glob("phase-*.md"))
    inits: Dict[str, Initiative] = {}
    for pf in phase_files:
        parsed, p = parse_phase_file(pf)
        problems += p
        for it in parsed:
            # R1 unique ids
            if it.iid in inits:
                problems.append(Problem("unique-ids",
                    f"duplicate initiative id '{it.iid}' ({it.source} and {inits[it.iid].source})"))
            else:
                inits[it.iid] = it
            # R2 exactly one Dependencies field
            if it.dep_field_count != 1:
                problems.append(Problem("deps-present",
                    f"{it.iid} ({it.source}) has {it.dep_field_count} Dependencies fields (need exactly 1)"))
            # R13 size valid
            if it.size not in SIZES:
                problems.append(Problem("size-valid",
                    f"{it.iid} ({it.source}) has invalid size '{it.size}' (need one of {sorted(SIZES)})"))
            # R14 acceptance present
            if not it.acceptance:
                problems.append(Problem("acceptance-present",
                    f"{it.iid} ({it.source}) has no measurable acceptance criteria"))

    # R3/R4/R5 dependency graph
    for it in inits.values():
        for d in it.deps:
            if d not in inits:
                problems.append(Problem("deps-resolvable",
                    f"{it.iid} depends on unknown initiative '{d}'"))
            elif inits[d].phase > it.phase:
                problems.append(Problem("deps-not-later-phase",
                    f"{it.iid} (phase {it.phase}) depends on later-phase '{d}' (phase {inits[d].phase})"))
    cyc = _find_cycle(inits)
    if cyc:
        problems.append(Problem("deps-acyclic", "dependency cycle: " + " -> ".join(cyc)))

    # R6 registry <-> definitions agreement
    reg_ids, def_ids = set(reg.rows), set(inits)
    for missing in sorted(def_ids - reg_ids):
        problems.append(Problem("registry-agreement",
            f"initiative '{missing}' defined in a phase file but absent from the index registry"))
    for extra in sorted(reg_ids - def_ids):
        problems.append(Problem("registry-agreement",
            f"registry lists '{extra}' but no phase file defines it"))
    for iid in sorted(reg_ids & def_ids):
        r, d = reg.rows[iid], inits[iid]
        if r["phase"] != d.phase:
            problems.append(Problem("registry-agreement",
                f"{iid}: registry phase {r['phase']} != phase-file phase {d.phase}"))
        if r["size"] and r["size"] != d.size:
            problems.append(Problem("registry-agreement",
                f"{iid}: registry size '{r['size']}' != phase-file size '{d.size}'"))

    # R7/R8 implementation coverage + PR-slice tables
    draft_by_phase, covered, slice_ids = parse_impl_files(plan_dir / "impl")
    slice_set = {s for s, _ in slice_ids}
    status_map = _load_status_map(plan_dir)
    for it in inits.values():
        if it.iid not in covered:
            problems.append(Problem("impl-coverage",
                f"{it.iid} has no implementation-task heading under impl/"))
        if it.size in BIG_SIZES and it.iid not in slice_set:
            # The draft waiver holds only while the initiative has not started; once
            # its state is beyond 'approved' the slice table is required regardless.
            started = status_map.get(it.iid) in STATUSES_STARTED
            phase_is_draft = draft_by_phase.get(it.phase, False)
            if not phase_is_draft or started:
                problems.append(Problem("slice-tables",
                    f"{it.iid} is {it.size} but has no 'PR slices: {it.iid}' table "
                    f"(draft waiver does not apply: draft={phase_is_draft}, started={started})"))

    # R9 quality-gate references resolve
    for it in inits.values():
        for ref in it.qg_refs:
            if ref not in gates:
                problems.append(Problem("qg-refs-resolve",
                    f"{it.iid} references undefined quality gate '{ref}'"))
    # R15 gates carry a concrete number
    for gid, gtext in gates.items():
        if not re.search(r"\d", gtext):
            problems.append(Problem("qg-has-number",
                f"quality gate '{gid}' has no concrete numeric target"))

    # R10 relative links resolve
    problems += _check_links(plan_dir)

    # R11 word budgets
    for name, budget in (("index", WORD_BUDGET["index"]),):
        wc = _word_count(index_text)
        if wc > budget:
            problems.append(Problem("word-budget", f"index README.md is {wc} words (budget {budget})"))
    for pf in phase_files:
        wc = _word_count(_read(pf))
        if wc > WORD_BUDGET["phase"]:
            problems.append(Problem("word-budget", f"{pf.name} is {wc} words (budget {WORD_BUDGET['phase']})"))

    # R12 state-file consistency
    problems += _check_state(plan_dir, inits)

    return problems, inits, reg


def _find_cycle(inits: Dict[str, Initiative]) -> Optional[List[str]]:
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {k: WHITE for k in inits}
    stack: List[str] = []

    def visit(n: str) -> Optional[List[str]]:
        color[n] = GRAY
        stack.append(n)
        for d in inits[n].deps:
            if d not in inits:
                continue
            if color[d] == GRAY:
                return stack[stack.index(d):] + [d]
            if color[d] == WHITE:
                r = visit(d)
                if r:
                    return r
        stack.pop()
        color[n] = BLACK
        return None

    for n in inits:
        if color[n] == WHITE:
            r = visit(n)
            if r:
                return r
    return None


def _check_links(plan_dir: Path) -> List[Problem]:
    problems: List[Problem] = []
    md_files = list(plan_dir.rglob("*.md"))
    heading_cache: Dict[Path, Set[str]] = {}

    def slugs_of(path: Path) -> Set[str]:
        if path not in heading_cache:
            s: Set[str] = set()
            if path.exists() and path.suffix == ".md":
                for ln in _read(path).splitlines():
                    h = HEADING_RE.match(ln)
                    if h:
                        s.add(_slug(h.group(2)))
            heading_cache[path] = s
        return heading_cache[path]

    for md in md_files:
        base = md.parent
        for m in LINK_RE.finditer(_read(md)):
            target = m.group(1).strip()
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            path_part, _, anchor = target.partition("#")
            if path_part:
                resolved = (base / path_part).resolve()
                if not resolved.exists():
                    problems.append(Problem("links-resolve",
                        f"{md.name}: broken link target '{target}'"))
                    continue
                anchor_file = resolved
            else:
                anchor_file = md
            if anchor and anchor_file.suffix == ".md" and _within(anchor_file, plan_dir):
                if _slug(anchor) not in slugs_of(anchor_file):
                    problems.append(Problem("links-resolve",
                        f"{md.name}: link '{target}' has no matching heading in {anchor_file.name}"))
    return problems


def _load_state(plan_dir: Path):
    """Return (state_dict_or_None, error_str_or_None)."""
    state_path = plan_dir / "program-state.json"
    if not state_path.exists():
        return None, "program-state.json missing"
    try:
        state = json.loads(_read(state_path))
    except json.JSONDecodeError as e:
        return None, f"program-state.json is not valid JSON: {e}"
    if not isinstance(state, dict):
        return None, "program-state.json must be an object keyed by initiative id"
    return state, None


def _load_status_map(plan_dir: Path) -> Dict[str, str]:
    state, _ = _load_state(plan_dir)
    out: Dict[str, str] = {}
    for iid, entry in (state or {}).items():
        if isinstance(entry, dict):
            out[iid] = entry.get("status")
    return out


def _check_state(plan_dir: Path, inits: Dict[str, Initiative]) -> List[Problem]:
    problems: List[Problem] = []
    state, err = _load_state(plan_dir)
    if err:
        return [Problem("state-consistency", err)]

    reg_ids = set(inits)
    for missing in sorted(reg_ids - set(state)):
        problems.append(Problem("state-consistency", f"program-state.json missing entry for '{missing}'"))
    for extra in sorted(set(state) - reg_ids):
        problems.append(Problem("state-consistency", f"program-state.json has stale entry '{extra}'"))

    def status_of(i):
        e = state.get(i)
        return e.get("status") if isinstance(e, dict) else None

    for iid, entry in state.items():
        if iid not in reg_ids:
            continue
        if not isinstance(entry, dict):
            problems.append(Problem("state-consistency",
                f"{iid}: entry must be a JSON object ({{phase,status,pr,evidence}})"))
            continue
        status = entry.get("status")
        if status not in STATUSES:
            problems.append(Problem("state-consistency",
                f"{iid}: invalid status '{status}' (need one of {sorted(STATUSES)})"))
            continue
        if entry.get("phase") != inits[iid].phase:
            problems.append(Problem("state-consistency",
                f"{iid}: state phase {entry.get('phase')} != roadmap phase {inits[iid].phase}"))
        if status in STATUSES_NEED_PR and not entry.get("pr"):
            problems.append(Problem("state-consistency",
                f"{iid}: status '{status}' requires a 'pr' link (open a draft PR before starting)"))
        if status in STATUSES_NEED_EVIDENCE and not entry.get("evidence"):
            problems.append(Problem("state-consistency",
                f"{iid}: status '{status}' requires an 'evidence' link (CI run / measurement)"))

    # deps-completion-order: you cannot start/finish ahead of a dependency.
    for iid, it in inits.items():
        if status_of(iid) in STATUSES_STARTED:
            for d in it.deps:
                if d in inits and status_of(d) != "shipped":
                    problems.append(Problem("deps-completion-order",
                        f"{iid} is '{status_of(iid)}' but dependency '{d}' is "
                        f"'{status_of(d)}' (must be shipped first)"))
    return problems


# --------------------------------------------------------------------------- #
# Frontier + state emission
# --------------------------------------------------------------------------- #
def compute_frontier(plan_dir: Path, inits: Dict[str, Initiative]) -> dict:
    state = json.loads(_read(plan_dir / "program-state.json"))

    def status(i):
        e = state.get(i)
        return e.get("status", "discovery") if isinstance(e, dict) else "discovery"

    shipped = {i for i in inits if status(i) == "shipped"}
    ready, in_progress, blocked = [], [], []
    for iid, it in inits.items():
        st = status(iid)
        if st in ("implementing", "measuring"):
            in_progress.append(iid)
        elif st in STATUSES_TERMINAL:
            continue
        elif all(d in shipped for d in it.deps):
            # Ready == every dependency shipped (matches the documented governance
            # rule). 'approved' vs 'discovery' does not gate readiness; it only
            # records whether a re-plan has ratified the item's impl slices.
            ready.append(iid)
        else:
            blocked.append(iid)
    unfinished_phases = [it.phase for i, it in inits.items() if status(i) not in STATUSES_TERMINAL]
    all_shipped = not [i for i in inits if status(i) not in STATUSES_TERMINAL]
    active = min(unfinished_phases) if unfinished_phases else None
    # Phases are not hard gates, so `ready` (deps satisfied) can include out-of-phase
    # items. The selection window is the active phase plus one advance slot; the loop
    # picks from readyInWindow (honouring WIP + the single advance slot), never from
    # readyOutOfWindow.
    in_window = [i for i in ready if active is not None and inits[i].phase <= active + 1]
    out_window = [i for i in ready if active is not None and inits[i].phase > active + 1]
    return {
        "activePhase": active,
        "ready": sorted(ready),
        "readyInWindow": sorted(in_window),
        "readyOutOfWindow": sorted(out_window),
        "inProgress": sorted(in_progress),
        "blocked": sorted(blocked),
        "allComplete": all_shipped,
    }


def emit_state(plan_dir: Path, inits: Dict[str, Initiative]) -> dict:
    """Regenerate program-state.json from the docs WITHOUT losing progress:
    preserve each existing entry's status/pr/evidence verbatim, add genuinely new
    initiatives as 'discovery', and drop entries no longer in the registry. Safe to
    re-run mid-program (e.g. after adding an initiative)."""
    existing, _ = _load_state(plan_dir)
    existing = existing or {}
    state = {}
    for iid, it in sorted(inits.items(), key=lambda kv: (kv[1].phase, kv[0])):
        prev = existing.get(iid)
        prev = prev if isinstance(prev, dict) else {}
        state[iid] = {
            "phase": it.phase,
            "status": prev.get("status", "discovery"),
            "pr": prev.get("pr"),
            "evidence": prev.get("evidence"),
        }
    return state


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Audit the Broadcast-Perfection roadmap.")
    default_plan = Path(__file__).resolve().parents[2] / "docs" / "broadcast-plan"
    ap.add_argument("--plan-dir", type=Path, default=default_plan)
    ap.add_argument("--frontier", action="store_true", help="after a clean audit, print the ready set as JSON")
    ap.add_argument("--emit-state", action="store_true", help="(re)generate program-state.json from the docs")
    args = ap.parse_args(argv)

    plan_dir = args.plan_dir.resolve()

    if args.emit_state:
        problems, inits, _ = run_rules(plan_dir)
        # emit-state ignores the state-consistency rule (it is generating that file).
        state = emit_state(plan_dir, inits)
        (plan_dir / "program-state.json").write_text(
            json.dumps(state, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {plan_dir / 'program-state.json'} ({len(state)} initiatives)")
        return 0

    problems, inits, _ = run_rules(plan_dir)
    if problems:
        print(f"roadmap audit FAILED: {len(problems)} problem(s)\n", file=sys.stderr)
        for pr in sorted((str(p) for p in problems)):
            print("  " + pr, file=sys.stderr)
        return 1

    if args.frontier:
        print(json.dumps(compute_frontier(plan_dir, inits), indent=2))
        return 0

    print(f"roadmap audit OK: {len(inits)} initiatives, 0 problems.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

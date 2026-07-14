#!/usr/bin/env python3
"""Bounded model check of the transport epoch commit protocol.

The model follows the production ownership boundaries rather than treating a
seek as one generic actor.  Every C++ family that publishes committed output
state is represented separately and reaches the shared
PlaybackWorker::commitOutputStateLocked primitive:

* published reuse and live-start fallback in requestSeekTo;
* reuse and full worker reposition;
* early operator completion from decoded coverage;
* armed-cut promotion;
* device-loss and memory-pressure recovery.

OutputRuntime dispatch is split in production order: configuration capture,
provider execution, identity selection (including hold-last), generation
recheck, lease acquisition, and completion.  A reset during an active lease
bumps configuration immediately, coalesces a pending epoch clear, lets the
already validated lease finish, and applies the clear before opening the next
lease.

Accepted modes are ``fixed``, ``mut_f1``, one ``mut_f2_<family>`` per commit
family, and ``all``.  ``all`` succeeds only when fixed proves the bounded
invariant and every mutant produces a concrete counterexample.

Each scenario is explored as an independent graph from the same initial
state. Reported totals (523 states for ``fixed``) are the aggregate across the
eleven scenario graphs, not the size of one coupled graph.
"""

from __future__ import annotations

import sys
from collections import deque
from dataclasses import dataclass, replace
from typing import Optional


FRAME = 1
TOLERANCE = 1
MAX_COMPLETIONS = 2

PHASE_NAMES = {
    0: "idle",
    1: "config-captured",
    2: "provider-complete",
    3: "identity-selected",
    4: "generation-rechecked",
    5: "lease-active",
}


@dataclass(frozen=True)
class Actor:
    key: str
    label: str
    source: str
    playhead: int
    seek_generation: int
    identity_generation: int


ACTORS = (
    Actor(
        "published_reuse",
        "published reuse",
        "requestSeekTo -> commitOutputStateLocked",
        300,
        2,
        2,
    ),
    Actor(
        "live_fallback",
        "live fallback",
        "requestSeekTo live-start fallback -> commitOutputStateLocked",
        320,
        2,
        2,
    ),
    Actor(
        "reuse_reposition",
        "reuse reposition",
        "repositionTo reuse path -> commitOutputStateLocked",
        340,
        2,
        2,
    ),
    Actor(
        "full_reposition",
        "full reposition",
        "commitFullRepositionOutputStateLocked -> commitOutputStateLocked",
        360,
        2,
        2,
    ),
    Actor(
        "early_operator",
        "early operator completion",
        "tryCompleteOperatorSeekFromCurrentOutputCache -> commitOutputStateLocked",
        380,
        2,
        2,
    ),
    Actor(
        "armed_cut",
        "armed cut",
        "maybeFireScheduledCut -> commitOutputStateLocked",
        400,
        1,
        2,
    ),
    Actor(
        "device_loss_recovery",
        "device-loss recovery",
        "handleGpuDeviceLoss/maybeFireScheduledCut -> commitOutputStateLocked",
        120,
        1,
        2,
    ),
    Actor(
        "memory_pressure_recovery",
        "memory-pressure recovery",
        "handleCriticalGpuMemoryPressure -> commitOutputStateLocked",
        140,
        1,
        2,
    ),
)
ACTOR_BY_KEY = {actor.key: actor for actor in ACTORS}


@dataclass(frozen=True)
class Scenario:
    name: str
    actor: Actor
    commit_phase: Optional[int] = None
    hold_last: bool = False
    f2_owner: bool = True


F1_SCENARIO = Scenario(
    "f1_inflight_snapshot",
    ACTOR_BY_KEY["published_reuse"],
    commit_phase=2,
    f2_owner=False,
)
DEFERRED_SCENARIO = Scenario(
    "deferred_reset_during_active_lease",
    ACTOR_BY_KEY["full_reposition"],
    commit_phase=5,
    f2_owner=False,
)
HOLD_LAST_SCENARIO = Scenario(
    "hold_last_deferred_reset",
    ACTOR_BY_KEY["armed_cut"],
    commit_phase=5,
    hold_last=True,
    f2_owner=False,
)
SCENARIOS = (F1_SCENARIO, DEFERRED_SCENARIO, HOLD_LAST_SCENARIO) + tuple(
    Scenario(actor.key, actor) for actor in ACTORS
)


@dataclass(frozen=True)
class State:
    actor_done: bool = False
    committed_playhead: int = 100
    committed_seek_generation: int = 1
    committed_identity_generation: int = 1
    config_generation: int = 0
    epoch_have: bool = True
    epoch_anchor_playhead: int = 100
    epoch_anchor_frame: int = 0
    frame_index: int = 0
    phase: int = 0
    captured_config: int = -1
    snapshot_playhead: int = -1
    snapshot_seek_generation: int = -1
    snapshot_identity_generation: int = -1
    snapshot_source: int = -1
    selected_playhead: int = -1
    selected_seek_generation: int = -1
    selected_identity_generation: int = -1
    selected_source: int = -1
    selected_hold_last: bool = False
    lease_active: bool = False
    lease_playhead: int = -1
    lease_seek_generation: int = -1
    lease_identity_generation: int = -1
    pending_epoch_reset: bool = False
    completions: int = 0


@dataclass(frozen=True)
class Violation:
    actor: str
    state: str
    sampled_playhead: int
    committed_playhead: int
    seek_generation: int
    identity_generation: int
    config_generation: int
    rendered_identity: str
    reason: str


def epoch_reset(state: State, mutate_f1: bool) -> State:
    state = replace(
        state,
        config_generation=(
            state.config_generation if mutate_f1 else state.config_generation + 1
        ),
    )
    if state.lease_active:
        return replace(state, pending_epoch_reset=True)
    return replace(state, epoch_have=False)


def render_identity(state: State) -> str:
    kind = "hold-last" if state.selected_hold_last else "exact"
    return (
        f"{kind}(source={state.selected_source},"
        f"seek={state.selected_seek_generation},"
        f"identity={state.selected_identity_generation})"
    )


def violation_at_completion(state: State, sampled: int, scenario: Scenario) -> Optional[Violation]:
    reasons = []
    if abs(sampled - state.lease_playhead) > TOLERANCE:
        reasons.append("sampled playhead diverges from the leased committed playhead")
    if state.selected_playhead != state.lease_playhead:
        reasons.append("selected snapshot playhead is stale")
    if state.selected_seek_generation != state.lease_seek_generation:
        reasons.append("selected seek generation is stale")
    if state.selected_identity_generation != state.lease_identity_generation:
        reasons.append("rendered identity generation is stale")
    if not reasons:
        return None
    return Violation(
        actor=scenario.actor.key,
        state=PHASE_NAMES[state.phase],
        sampled_playhead=sampled,
        committed_playhead=state.lease_playhead,
        seek_generation=state.lease_seek_generation,
        identity_generation=state.lease_identity_generation,
        config_generation=state.config_generation,
        rendered_identity=render_identity(state),
        reason="; ".join(reasons),
    )


def mutation_flags(mode: str, scenario: Scenario) -> tuple[bool, bool]:
    mutate_f1 = mode == "mut_f1"
    mutate_f2 = (
        scenario.f2_owner
        and mode == f"mut_f2_{scenario.actor.key}"
    )
    return mutate_f1, mutate_f2


def actions(state: State, scenario: Scenario, mode: str):
    out = []
    mutate_f1, mutate_f2 = mutation_flags(mode, scenario)

    # The generation recheck and lease acquisition are distinct modeled phases
    # for observability, but both execute under OutputRuntime::m_mutex. A commit
    # reset cannot acquire that mutex between phases 4 and 5.
    commit_is_enabled = not state.actor_done and state.phase != 4
    if scenario.commit_phase is not None:
        commit_is_enabled = commit_is_enabled and state.phase == scenario.commit_phase
    if commit_is_enabled:
        committed = replace(
            state,
            actor_done=True,
            committed_playhead=scenario.actor.playhead,
            committed_seek_generation=scenario.actor.seek_generation,
            committed_identity_generation=scenario.actor.identity_generation,
        )
        if not mutate_f2:
            committed = epoch_reset(committed, mutate_f1)
        label = (
            f"actor.{scenario.actor.key}.commitOutputStateLocked"
            + (" [F2 RESET OMITTED]" if mutate_f2 else "+resetPlayEpoch")
        )
        out.append((label, committed, None))

    if state.phase == 0 and state.completions < MAX_COMPLETIONS:
        out.append(
            (
                "output.captureConfig",
                replace(state, phase=1, captured_config=state.config_generation),
                None,
            )
        )
    elif state.phase == 1:
        source = state.committed_playhead
        if scenario.hold_last:
            source = state.committed_playhead - 20
        out.append(
            (
                "output.providerCall",
                replace(
                    state,
                    phase=2,
                    snapshot_playhead=state.committed_playhead,
                    snapshot_seek_generation=state.committed_seek_generation,
                    snapshot_identity_generation=state.committed_identity_generation,
                    snapshot_source=source,
                ),
                None,
            )
        )
    elif state.phase == 2:
        out.append(
            (
                "output.selectIdentity[hold-last]"
                if scenario.hold_last
                else "output.selectIdentity[exact]",
                replace(
                    state,
                    phase=3,
                    selected_playhead=state.snapshot_playhead,
                    selected_seek_generation=state.snapshot_seek_generation,
                    selected_identity_generation=state.snapshot_identity_generation,
                    selected_source=state.snapshot_source,
                    selected_hold_last=scenario.hold_last,
                ),
                None,
            )
        )
    elif state.phase == 3:
        if state.captured_config != state.config_generation:
            out.append(
                (
                    "output.generationRecheck[reject]",
                    replace(state, phase=0),
                    None,
                )
            )
        else:
            out.append(
                (
                    "output.generationRecheck[accept]",
                    replace(state, phase=4),
                    None,
                )
            )
    elif state.phase == 4:
        out.append(
            (
                "output.acquireLease",
                replace(
                    state,
                    phase=5,
                    lease_active=True,
                    lease_playhead=state.committed_playhead,
                    lease_seek_generation=state.committed_seek_generation,
                    lease_identity_generation=state.committed_identity_generation,
                ),
                None,
            )
        )
    elif state.phase == 5:
        epoch_have = state.epoch_have
        epoch_anchor_playhead = state.epoch_anchor_playhead
        epoch_anchor_frame = state.epoch_anchor_frame
        if not epoch_have:
            epoch_have = True
            epoch_anchor_playhead = state.selected_playhead
            epoch_anchor_frame = state.frame_index
        sampled = epoch_anchor_playhead + (state.frame_index - epoch_anchor_frame) * FRAME
        violation = violation_at_completion(state, sampled, scenario)

        # Mirrors both completion sites in OutputRuntime: the already validated
        # lease renders first, pending mutations apply while dispatchActive is
        # still true, and only then is the lease/barrier cleared.
        if state.pending_epoch_reset:
            epoch_have = False
        completed = replace(
            state,
            phase=0,
            lease_active=False,
            pending_epoch_reset=False,
            epoch_have=epoch_have,
            epoch_anchor_playhead=epoch_anchor_playhead,
            epoch_anchor_frame=epoch_anchor_frame,
            frame_index=state.frame_index + 1,
            completions=state.completions + 1,
        )
        out.append(("output.completeLease", completed, violation))

    return out


def explore(scenario: Scenario, mode: str):
    initial = State()
    queue = deque([initial])
    parent = {initial: (None, None)}
    states = 0
    while queue:
        state = queue.popleft()
        states += 1
        for label, next_state, violation in actions(state, scenario, mode):
            if violation is not None:
                trace = [label]
                cursor = state
                while parent[cursor][0] is not None:
                    previous, previous_label = parent[cursor]
                    trace.append(previous_label)
                    cursor = previous
                trace.reverse()
                return states, trace, violation
            if next_state not in parent:
                parent[next_state] = (state, label)
                queue.append(next_state)
    return states, None, None


def run(mode: str) -> bool:
    print(f"=== config: {mode} ===")
    explored = 0
    first_counterexample = None
    for scenario in SCENARIOS:
        states, trace, violation = explore(scenario, mode)
        explored += states
        verdict = "COUNTEREXAMPLE" if violation else "invariant holds"
        print(f"  {scenario.name:38s} {states:5d} states  -> {verdict}")
        if violation is not None and first_counterexample is None:
            first_counterexample = (scenario, trace, violation)

    if first_counterexample is None:
        print(
            f"  VERDICT: PROOF -- invariant holds over {explored} reachable states "
            f"aggregated across {len(SCENARIOS)} independent scenario graphs "
            f"(bounded to {MAX_COMPLETIONS} completed leases)\n"
        )
        return True

    scenario, trace, violation = first_counterexample
    print(f"  VERDICT: COUNTEREXAMPLE (scenario: {scenario.name})")
    for index, label in enumerate(trace, 1):
        print(f"    {index:2d}. {label}")
    print(
        "  COUNTEREXAMPLE: "
        f"actor={violation.actor} "
        f"state={violation.state} "
        f"sampled_playhead={violation.sampled_playhead} "
        f"committed_playhead={violation.committed_playhead} "
        "generation="
        f"seek:{violation.seek_generation}/"
        f"identity:{violation.identity_generation}/"
        f"config:{violation.config_generation} "
        f"rendered_identity={violation.rendered_identity} "
        f"reason={violation.reason}\n"
    )
    return False


MUTANTS = ("mut_f1",) + tuple(f"mut_f2_{actor.key}" for actor in ACTORS)
MODES = ("fixed",) + MUTANTS


def main() -> int:
    selected = sys.argv[1] if len(sys.argv) > 1 else "all"
    if selected != "all" and selected not in MODES:
        print(f"usage: {sys.argv[0]} [all|{'|'.join(MODES)}]", file=sys.stderr)
        return 2

    modes = MODES if selected == "all" else (selected,)
    results = {mode: run(mode) for mode in modes}
    if selected != "all":
        expected = selected == "fixed"
        return 0 if results[selected] == expected else 1

    print("=== differential gate ===")
    expected = {"fixed": True, **{mutant: False for mutant in MUTANTS}}
    differential_ok = True
    for mode in modes:
        got = results[mode]
        ok = got == expected[mode]
        differential_ok = differential_ok and ok
        print(
            f"  [{'OK' if ok else 'FAIL':4s}] {mode:40s} "
            f"expected {'PROOF' if expected[mode] else 'COUNTEREXAMPLE':15s} "
            f"got {'PROOF' if got else 'COUNTEREXAMPLE'}"
        )
    return 0 if differential_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

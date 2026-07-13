#!/usr/bin/env python3
"""Exhaustive interleaving model check of the playback-transport frame-accuracy
invariant (Challenge 01, docs/hardest-technical-challenges.md).

THE PROPERTY
  On every output tick that renders a NON-placeholder frame from a snapshot
  taken with the commit gate OPEN (committedGen == seekGen), the epoch-sampled
  media time must equal the snapshot-visible playhead within one frame
  duration.  (While the gate is CLOSED an in-flight reposition legitimately
  holds the last committed playhead while the epoch keeps advancing; that
  transient is bounded by the reposition latency budget and is exempt here,
  exactly as the production e2e gate treats it.)

THE MODEL
  Three actors over the real synchronization skeleton, interleaved at critical-
  section granularity (every modeled shared access is mutex-guarded or a
  paired release/acquire atomic in the source, so sequentially-consistent
  interleaving of whole critical sections is sound for the logic-omission bug
  class this checks; see the fidelity table in the challenge doc):

    UI thread      requestSeekTo republish (playbackworker.cpp:227) and
                   generation bump (:241) as SEPARATE atomic steps (the output
                   thread reads these atomics without m_mutex), armCut.
    Worker thread  begin/commit full or cache-reuse reposition
                   (CommitGate::canCommitReposition), then the post-commit pair from
                   refreshOutputAfterSeekCommit: resetPlayEpoch() and
                   dispatchImmediate() as SEPARATE steps in the broken protocol.
    Output thread  background tick split exactly as outputruntime.cpp:
                   snapshot taken OUTSIDE the lock (:362) [which also fires a
                   due armed cut inside makeOutputSnapshot, playbackworker.cpp
                   :2322/:4042-4150], then the lease re-check block (:365-375:
                   immediateDispatchRequests / configGeneration /
                   nextOutputFrameIndex) and dispatchTick (:380).

  The play epoch is plain state owned by the dispatcher (outputdispatcher.h:
  195-196), re-anchored per clockedStateForTick (outputdispatcher.cpp:443-464)
  and sampled per OutputFrameClock::samplePlayheadMsForOutputTick
  (outputframeclock.cpp:16-20).  resetPlayEpoch from a foreign thread with no
  dispatch active applies immediately (outputruntime.cpp:114-126) and bumps
  NONE of the three re-checked values -- that fact is the point.

CONFIGURATIONS
  head    the protocol exactly as written today          -> expect a
          counterexample (TWO real holes, see below)
  fixed   BOTH repairs applied                           -> expect PROOF
  mutA    'fixed' minus the reposition-commit re-anchor  -> expect the
          historical far-back counterexample
  mutB    'fixed' minus the armed-cut re-anchor (:4130)  -> expect the
          historical armed-cut counterexample
  mutReuse 'fixed' minus the reuse-commit re-anchor      -> expect the
          reuse fast-path H2 counterexample

  The two holes 'head' exhibits (both previously unknown):
    H1 swallowed reset: a background snapshot taken pre-commit passes the
       :371-372 re-checks after the worker's resetPlayEpoch applied (the reset
       bumps none of the re-checked values), so the stale snapshot dispatches
       and re-anchors the freshly-cleared epoch at the pre-seek playhead.
    H2 commit-to-reset gap: refreshOutputAfterSeekCommit resets the epoch as a
       SEPARATE step after the commit (:459-469), so ticks landing between the
       commit and the reset render gate-open frames against the stale anchor.

  The repair ('fixed'):
    F1 resetPlayEpoch (every site) also bumps m_configGeneration, so any
       in-flight pre-reset snapshot fails the :371 re-check and is discarded.
    F2 every reposition commit applies the epoch reset ATOMICALLY inside its
       m_bufferMutex critical section (exactly as the armed-cut fire already
       does at :4130), not afterwards from the refresh helper.

  Differential gate (challenge acceptance): fixed MUST prove; head, mutA,
  mutB and mutReuse MUST each produce a concrete schedule.

Run:  python docs/hardest-technical-challenges/transport_epoch_modelcheck.py [head|fixed|mutA|mutB|mutReuse|all]
"""

import sys
from collections import deque

FRAME = 1          # one frame duration == one model time unit
TOL = 1            # invariant tolerance: one frame duration
MAX_DISPATCH = 7   # bounded exploration: total rendered ticks per run

# ---------------------------------------------------------------------------
# State: a flat tuple (hashable).  Field index constants for readability.
# ---------------------------------------------------------------------------
FIELDS = [
    "tp",       # transport playhead (m_transport pos)
    "sg",       # m_seekGeneration
    "ag",       # m_armSeekGen
    "cg",       # m_committedGeneration
    "cp",       # m_committedPlayheadMs
    "lv",       # m_lastVisiblePlayheadMs
    "st",       # m_seekTargetMs (-1 none)
    "wphase",   # worker: 0 idle, 1 staging(reposition in flight)
    "wstart",   # worker: captured startGen
    "wtgt",     # worker: reposition target
    "wpost",    # worker post-commit obligation: 0 none, 1 need epoch reset,
                #   2 need immediate dispatch  (refreshOutputAfterSeekCommit)
    "reuse",    # scripted worker commit uses the cache-reuse fast path
    "publ", "pubh",   # published output cache coverage [lo, hi]
    "armed",    # m_cutArmed
    "covers",   # m_stagingCovers
    "sched",    # m_scheduledCutFrame (-1 none)
    "eh",       # dispatcher m_havePlayEpoch
    "ea",       # epoch anchor playhead   (m_playEpoch.playStartedAtPlayheadMs)
    "ef",       # epoch anchor frame      (m_playEpoch.playStartedAtOutputFrame)
    "fi",       # dispatcher nextOutputFrameIndex
    "cfg",      # m_configGeneration
    "bgphase",  # background tick: 0 idle, 1 snapshot taken (pre-lease)
    "bgvis", "bgcov", "bgopen", "bgfi", "bgcfg",  # the stale-able snapshot
    "immreq",   # m_immediateDispatchRequests (0/1; immediate modeled atomic)
    "seekint",  # scripted seek intent remaining (0/1)
    "armint",   # scripted arm intent remaining (0/1)
    "disp",     # dispatch budget consumed
]
IX = {name: i for i, name in enumerate(FIELDS)}


def get(s, k):
    return s[IX[k]]


def put(s, **kw):
    s = list(s)
    for k, v in kw.items():
        s[IX[k]] = v
    return tuple(s)


def covered(s, playhead):
    return get(s, "publ") <= playhead <= get(s, "pubh")


# ---------------------------------------------------------------------------
# The dispatch/render step shared by the background tick and the immediate
# dispatch.  Mirrors clockedStateForTick + samplePlayheadMsForOutputTick +
# the renderBus divergence measurement (outputdispatcher.cpp:299-303), but as
# a checked INVARIANT rather than an observational counter.
# Returns (new_state, violation_or_None).
# ---------------------------------------------------------------------------
def render(s, vis, cov, gate_open, label):
    eh, ea, ef, fi = get(s, "eh"), get(s, "ea"), get(s, "ef"), get(s, "fi")
    if not eh:                    # (re-)anchor: outputdispatcher.cpp:454-458
        eh, ea, ef = True, vis, fi
    sampled = ea + (fi - ef) * FRAME      # outputframeclock.cpp:16-20, speed 1
    violation = None
    if gate_open and cov and abs(sampled - vis) > TOL:
        violation = (f"{label}: rendered NON-placeholder frame with "
                     f"sampledPlayhead={sampled} vs visiblePlayhead={vis} "
                     f"(divergence {abs(sampled - vis)} frames, gate open)")
    # dispatching one frame == one frame of wall time: transport advances too
    s = put(s, eh=eh, ea=ea, ef=ef, fi=fi + 1, tp=get(s, "tp") + FRAME,
            disp=get(s, "disp") + 1)
    return s, violation


# ---------------------------------------------------------------------------
# Actions.  Each returns list of (action_label, new_state, violation_or_None).
# ---------------------------------------------------------------------------
def actions(s, cfgflags):
    out = []
    site_a, site_b, fix_bump, fix_atomic, fix_reuse_atomic = cfgflags

    def epoch_reset(ns):
        ns = put(ns, eh=False)
        if fix_bump:                       # F1: invalidate in-flight snapshots
            ns = put(ns, cfg=get(ns, "cfg") + 1)
        return ns

    # ---- UI thread -------------------------------------------------------
    # requestSeekTo, step 1: republish held playhead (playbackworker.cpp:227).
    if get(s, "seekint") == 1:
        out.append(("ui.seek.republish(:227)",
                    put(s, cp=get(s, "lv"), seekint=2), None))
    # requestSeekTo, step 2: bump m_seekGeneration + set target (:241, :210).
    # (Fast-path publish not taken: the scripted target lies outside the
    #  published cache, as in the far-back scenario.)
    if get(s, "seekint") == 2:
        out.append(("ui.seek.bump(:241)",
                    put(s, sg=get(s, "sg") + 1, st=SEEK_TARGET, seekint=0),
                    None))
    # armCut: m_armSeekGen := seekGen; m_cutArmed := true (:3718-3733).
    if get(s, "armint") == 1:
        out.append(("ui.armCut(:3732)",
                    put(s, ag=get(s, "sg"), armed=True, armint=0), None))

    # ---- Worker thread ---------------------------------------------------
    # Begin reposition for the pending seek.
    if get(s, "st") >= 0 and get(s, "wphase") == 0:
        out.append(("worker.beginReposition",
                    put(s, wphase=1, wstart=get(s, "sg"), wtgt=get(s, "st")),
                    None))
    # Commit the reposition (CommitGate::canCommitReposition, :3352; stores
    # :3389-3397; transport rebase; lastVisible).  wpost encodes the follow-up
    # refreshOutputAfterSeekCommit obligations.
    if get(s, "wphase") == 1:
        if get(s, "wstart") == get(s, "sg") and get(s, "st") == get(s, "wtgt"):
            t = get(s, "wtgt")
            ns = put(s, publ=t - 2, pubh=t + 2, cp=t, lv=t,
                     cg=get(s, "wstart"), tp=t, st=-1, wphase=0)
            atomic_commit = fix_atomic and (not get(s, "reuse") or fix_reuse_atomic)
            if site_a and atomic_commit:
                # F2: epoch reset INSIDE the commit critical section, as the
                # armed-cut fire already does (:4130).
                ns = epoch_reset(ns)
                ns = put(ns, wpost=2)
            elif site_a:
                ns = put(ns, wpost=1)   # today: reset is a later, separate step
            else:
                ns = put(ns, wpost=2)   # mutation A: re-anchor site deleted
            path = "reuse" if get(s, "reuse") else "full"
            out.append((f"worker.commitReposition[{path}]"
                        + ("+resetPlayEpoch[F2]" if site_a and atomic_commit else ""),
                        ns, None))
        else:
            out.append(("worker.abortReposition(superseded)",
                        put(s, wphase=0), None))
    # Post-commit step 1 (today's protocol): resetPlayEpoch as a separate call
    # (refreshOutputAfterSeekCommit :468 -> outputruntime.cpp:114-126,
    # immediate-apply path: no dispatch is active at that instant).
    if get(s, "wpost") == 1:
        ns = epoch_reset(put(s, wpost=2))
        out.append(("worker.resetPlayEpoch(:468)", ns, None))
    # Post-commit step 2: dispatchImmediate (:469).  Modeled atomically: takes
    # a FRESH snapshot and dispatches (its internal optimistic re-check always
    # sees its own consistent capture in this atomic rendering).
    if get(s, "wpost") == 2 and get(s, "disp") < MAX_DISPATCH:
        gate_open = get(s, "cg") == get(s, "sg")
        vis = get(s, "tp") if gate_open else get(s, "cp")
        cov = covered(s, vis)
        ns = put(s, wpost=0)
        ns, vio = render(ns, vis, cov, gate_open, "worker.dispatchImmediate(:469)")
        if gate_open:
            ns = put(ns, lv=vis)
        out.append(("worker.dispatchImmediate(:469)", ns, vio))
    # Worker stages the armed cut, then schedules it.
    if get(s, "armed") and not get(s, "covers"):
        out.append(("worker.fillStaging(:3742)", put(s, covers=True), None))
    if get(s, "armed") and get(s, "covers") and get(s, "sched") < 0:
        out.append(("worker.scheduleCut(:4006)",
                    put(s, sched=get(s, "fi") + 2), None))

    # ---- Output thread: background tick ----------------------------------
    # Step 1 -- snapshot OUTSIDE the lock (outputruntime.cpp:362).  Inside the
    # snapshot provider, m_bufferMutex is held and a due armed cut FIRES FIRST
    # (playbackworker.cpp:2322 -> maybeFireScheduledCut :4042-4150).
    if get(s, "bgphase") == 0 and get(s, "disp") < MAX_DISPATCH:
        ns = s
        # maybeFireScheduledCut: due?
        if get(ns, "sched") >= 0 and get(ns, "fi") >= get(ns, "sched"):
            if get(ns, "sg") != get(ns, "ag"):
                # manual-seek-wins abort (:4066-4082): full disarm, no swap.
                ns = put(ns, sched=-1, covers=False, armed=False)
            else:
                # fire (:4084-4150): swap+publish staging, transport rebase
                # (NO seekGen bump), epoch re-anchor obligation (site B :4130).
                t = CUT_TARGET
                ns = put(ns, publ=t - 2, pubh=t + 2, tp=t,
                         sched=-1, covers=False, armed=False)
                if site_b:
                    ns = epoch_reset(ns)
        gate_open = get(ns, "cg") == get(ns, "sg")
        vis = get(ns, "tp") if gate_open else get(ns, "cp")
        cov = covered(ns, vis)
        ns = put(ns, bgphase=1, bgvis=vis, bgcov=cov, bgopen=gate_open,
                 bgfi=get(ns, "fi"), bgcfg=get(ns, "cfg"))
        if gate_open:
            ns = put(ns, lv=vis)   # snapshot writes m_lastVisible (:2489)
        out.append(("output.snapshot(:362)+fireCut(:2322)", ns, None))
    # Step 2 -- lease re-check + dispatchTick (outputruntime.cpp:365-380).
    if get(s, "bgphase") == 1:
        if get(s, "immreq") > 0:
            out.append(("output.yieldToImmediate(:370)", put(s, bgphase=0), None))
        elif get(s, "cfg") != get(s, "bgcfg") or get(s, "fi") != get(s, "bgfi"):
            out.append(("output.recheckFailed(:371-372)->continue",
                        put(s, bgphase=0), None))
        else:
            ns = put(s, bgphase=0)
            ns, vio = render(ns, get(s, "bgvis"), get(s, "bgcov"),
                             get(s, "bgopen"), "output.dispatchTick(:380)")
            out.append(("output.dispatchTick(:380)", ns, vio))

    return out


# ---------------------------------------------------------------------------
# BFS over all interleavings.
# ---------------------------------------------------------------------------
SEEK_TARGET = 2
CUT_TARGET = 4


def initial(scenario):
    s = [0] * len(FIELDS)
    s = tuple(s)
    # Steady-state 1x play: epoch anchored and tracking the transport
    # (ea + (fi - ef) == tp), published cache covering the playhead.
    s = put(s, tp=20, cp=20, lv=20, st=-1, publ=18, pubh=30,
            sched=-1, eh=True, ea=20, ef=0)
    if scenario == "seek":
        s = put(s, seekint=1)
    elif scenario == "reuse":
        s = put(s, seekint=1, reuse=True)
    elif scenario == "cut":
        s = put(s, armint=1)
    elif scenario == "cut+seek":
        s = put(s, armint=1, seekint=1)
    return s


def explore(scenario, cfgflags):
    init = initial(scenario)
    seen = {init}
    parent = {init: (None, None)}
    q = deque([init])
    states = 0
    while q:
        s = q.popleft()
        states += 1
        for label, ns, vio in actions(s, cfgflags):
            if vio is not None:
                # reconstruct schedule
                trace = [(label + "  ** " + vio, None)]
                cur = s
                while parent[cur][0] is not None:
                    prev, lab = parent[cur]
                    trace.append((lab, cur))
                    cur = prev
                trace.reverse()
                return states, trace
            if ns not in seen:
                seen.add(ns)
                parent[ns] = (s, label)
                q.append(ns)
    return states, None


def run(name, cfgflags):
    print(f"=== config: {name} ===")
    total = 0
    worst = None
    for scenario in ("seek", "reuse", "cut", "cut+seek"):
        states, trace = explore(scenario, cfgflags)
        total += states
        print(f"  scenario {scenario:9s}: {states:7d} states explored", end="")
        if trace:
            print("  -> COUNTEREXAMPLE")
            if worst is None:
                worst = (scenario, trace)
        else:
            print("  -> invariant holds")
    if worst:
        scenario, trace = worst
        print(f"\n  VERDICT: COUNTEREXAMPLE  (scenario: {scenario})")
        print("  schedule:")
        for i, (lab, _st) in enumerate(trace):
            print(f"    {i + 1:2d}. {lab}")
    else:
        print(f"\n  VERDICT: PROOF -- invariant holds over all {total} states"
              f" (exhaustive within bounds: {MAX_DISPATCH} dispatches)")
    print()
    return worst is None


CONFIGS = {
    #          (site_a, site_b, F1_bump, F2_full_atomic, F2_reuse_atomic)
    "head":     (True,  True,  False, False, False),
    "fixed":    (True,  True,  True,  True,  True),
    "mutA":     (False, True,  True,  True,  True),
    "mutB":     (True,  False, True,  True,  True),
    "mutReuse": (True,  True,  True,  True,  False),
}

if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    names = list(CONFIGS) if which == "all" else [which]
    results = {}
    for n in names:
        results[n] = run(n, CONFIGS[n])
    if which == "all":
        print("=== differential gate ===")
        expected = {
            "head": False,
            "fixed": True,
            "mutA": False,
            "mutB": False,
            "mutReuse": False,
        }
        ok = all(results[n] == expected[n] for n in results)
        for n in results:
            want = "PROOF" if expected[n] else "COUNTEREXAMPLE"
            got = "PROOF" if results[n] else "COUNTEREXAMPLE"
            mark = "OK " if results[n] == expected[n] else "FAIL"
            print(f"  [{mark}] {n:6s} expected {want:15s} got {got}")
        sys.exit(0 if ok else 1)

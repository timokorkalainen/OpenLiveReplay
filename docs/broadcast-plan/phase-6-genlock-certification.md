# Phase 6 — Genlock, Certification & Award Polish

> Part of the [Broadcast-Perfection Plan](./README.md). The index holds the goal, architecture guardrails, non-goals, the quality gates referenced below, the governance rules and the audit lane. Read it first.

**Duration estimate.** ~4-6 months (overlaps Phase 5 hardware work)

## Why this phase, in this order

The final phase converts a capable system into a certifiable, award-submittable one and honestly closes the genlock ceiling..

## Initiatives

### `arch-genlock-reference-backend` — Genlock/NIC-PHC/hardware-capture TimingReference backend swap (behind the shipped seam)
- **Size:** XL
- **Dependencies:** none
- **Definition:** Swap a genlock/NIC-PHC/hardware-capture backend into the shipped TimingReference seam.
- **Acceptance criteria:**
  - [ ] a hardware-PHC / genlock backend swaps in behind the TimingReference seam with 0 pipeline restructure

### `ns6-evidence-harness` — Award-grade measurement, certification & evidence harness (NS-6) + dual-jury submission package
- **Size:** L
- **Dependencies:** none
- **Definition:** Assemble the reproducible, real-gear measurement and certification evidence into the dual-jury submission package.
- **Acceptance criteria:**
  - [ ] every north-star KPI has a re-runnable measurement and a documented tier in the submission package

### `qe-soak-chaos-ci` — Wire the 24/7 soak + fault-injection into scheduled CI with MKV-conformance validation
- **Size:** L
- **Dependencies:** `qe-two-clock-drift-rig`
- **Definition:** Make 24/7 resilience a measured gate: run the soak fleet nightly under injected faults and track MTBF/uptime.
- **Acceptance criteria:**
  - [ ] Nightly >=1 h soak + all chaos scenarios green: 0 crashes/leaks/stalls under sanitizers
  - [ ] Every fault scenario yields a readable finalized MKV

### `qe-st2110-ptp-conformance` — ST 2110-21 + ST 2059 PTP timing conformance harness
- **Size:** XL
- **Dependencies:** none
- **Definition:** Measure the ST 2110 sender against the network-compatibility gap model and PTP lock against the broadcast profile, for genlock-grade certification evidence.
- **Acceptance criteria:**
  - [ ] ST 2110-21 sender measured inside the Narrow gap model (or gap reported honestly)
  - [ ] ST 2059 PTP offset measured and bounded, ceiling reported not overclaimed

### `qe-two-clock-drift-rig` — Two-clock drift & genlock soak rig
- **Size:** L
- **Dependencies:** none
- **Definition:** Prove bounded frame-phase lock with source and recorder on genuinely independent clocks, replacing the single-wall-clock slope==1.0 artifact.
- **Acceptance criteria:**
  - [ ] Two-clock run yields slope measurably != 1.0 and drift bounded within measured ppm
  - [ ] 24 h nightly reports 0 crash/leak/stall and a drift-over-time trace

### `rel-24x7-soak` — 24/7 endurance + fault-injection validation harness with recovery SLAs
- **Size:** L
- **Dependencies:** `rel-storage-guardian`, `rel-crash-safe-mkv`, `rel-output-autorecover`, `rel-worker-watchdog`, `rel-redundant-source-failover`
- **Definition:** Prove the reliability claims by running the full record+output pipeline for >=24 h under scripted fault injection and asserting recovery SLAs and zero content loss.
- **Acceptance criteria:**
  - [ ] A 24 h run injects each fault class repeatedly and every recovery SLA passes with zero recorded-content loss; the report shows availability >= 99.999% over the run
  - [ ] The harness is deterministic enough to gate regressions (fault schedule is seeded/reproducible)

---

[« Phase 5](./phase-5-facility-interop.md) · [Index](./README.md) · [Implementation plan](./impl/phase-6-genlock-certification.md)

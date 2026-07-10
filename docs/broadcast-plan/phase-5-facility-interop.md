# Phase 5 — Facility Interop & Hardware I/O

> Part of the [Broadcast-Perfection Plan](./README.md). The index holds the goal, architecture guardrails, non-goals, the quality gates referenced below, the governance rules and the audit lane. Read it first.

**Duration estimate.** ~8-10 months (hardware/SDK-gated, parallelizable)

## Why this phase, in this order

This is the switcher-trust and facility-citizen program (NS-1/NS-2)..

## Initiatives

### `bvp-st2110-real` — Real ST 2110-20 (RFC 4175 10-bit 4:2:2) + ANC + VPID + ST 2110-21 pacing
- **Size:** XL
- **Dependencies:** `bvp-highbit-frame-model`, `bvp-decode-highbit`
- **Definition:** Emit standards-compliant ST 2110-20 uncompressed video with proper pgroup packing, ST 2110-21 pacing, ST 2110-40 ANC and ST 352 VPID.
- **Acceptance criteria:**
  - [ ] tst_st2110framer proves pgroup byte layout matches RFC 4175 for 10-bit 4:2:2 on golden vectors
  - [ ] A loopback e2e depacketizes the RTP stream and reconstructs the frame bit-exactly; VPID and ANC timecode decode to the expected values

### `hwio-capture-half` — SDI / ST 2110 ingest (the capture half)
- **Size:** XL
- **Dependencies:** none
- **Definition:** Capture the facility feed over SDI / ST 2110, reusing the output framer and the genlock/PTP/NMOS work.
- **Acceptance criteria:**
  - [ ] a 1080p SDI input and a ST 2110-20 flow record frame-exact vs a marker source
  - [ ] 0 dropped frames over a 1 h capture soak

### `ing-envelope-broaden` — HE-AAC / AAC-LD / LATM decode
- **Size:** M
- **Dependencies:** `ing-audio-degrade`
- **Definition:** Broaden the native audio decode envelope beyond AAC-LC for real contribution feeds.
- **Acceptance criteria:**
  - [ ] HE-AAC v1/v2 and AAC-LD sources decode at correct pitch
  - [ ] unsupported variants degrade to amber-audio, not source failure

### `ing-linux-native-decode` — Native H.264/HEVC + AAC decode on Linux
- **Size:** L
- **Dependencies:** `ing-windows-live-validation`
- **Definition:** Implement the Linux video + AAC decoder backends so a Linux build actually ingests (a stub today).
- **Acceptance criteria:**
  - [ ] a Linux CI job runs an ingest e2e that decodes H.264 + AAC

### `ing-udp-file-restore` — Restore udp:// (multicast MPEG-TS) and file:// ingest
- **Size:** M
- **Dependencies:** none
- **Definition:** Re-add native udp:// (multicast MPEG-TS, parser already exists) and file:// ingest.
- **Acceptance criteria:**
  - [ ] a udp:// multicast MPEG-TS source and a file:// source each record frame-exact (e2e)

### `pi-automation-protocols` — Playout/newsroom automation adapters (VDCP + MOS)
- **Size:** L
- **Dependencies:** `pi-recall-playlist-ui`, `pi-clip-export-engine`
- **Definition:** Let playout automation and newsroom systems cue/recall/take clips via industry protocols so the app slots into an automated chain.
- **Acceptance criteria:**
  - [ ] tst_vdcp / tst_mos protocol conformance against captured message fixtures; an e2e drives a cue+take over VDCP that recalls and plays the intended entry frame-accurately

### `pi-decklink-sdi-backend` — Real DeckLink SDI/HDMI output backend (embedded audio + RP188 VANC TC)
- **Size:** L
- **Dependencies:** none
- **Definition:** Fill the shipped DeckLink sink's stub with a real SDK backend: genlocked SDI/HDMI, embedded audio, and RP188 VANC timecode from the programme TC.
- **Acceptance criteria:**
  - [ ] Opt-in hardware smoke on a DeckLink device: output locks to genlock/PTP reference; a downstream analyzer reads RP188 TC == programme TC; embedded audio present
  - [ ] Software CI keeps the stub path green (no SDK required to build)

### `pi-nmos-control` — AMWA NMOS node — IS-04 registration/discovery + IS-05 connection management (+ HTTP + DNS-SD stack)
- **Size:** XL
- **Dependencies:** `pi-control-api-hardening`
- **Definition:** Make the app a first-class NMOS node so any broadcast controller can discover its NDI/ST2110 senders (and ingest receivers) and connection-manage them.
- **Acceptance criteria:**
  - [ ] AMWA nmos-testing suite (IS-04-01/02, IS-05-01/02) run opt-in in CI -> 100% of applicable tests pass
  - [ ] An IS-05 sender activation flips the target on and emits a correct RFC 4566 SDP

### `pi-remi-cloud` — REMI / remote & cloud production hooks (SRT/RIST program egress, remote control bridge, headless render)
- **Size:** XL
- **Dependencies:** `pi-control-api-hardening`, `pi-clip-export-engine`
- **Definition:** Add remote/cloud production: SRT/RIST program egress back to facility, remote control bridging over the authenticated API, and a headless cloud-render export worker.
- **Acceptance criteria:**
  - [ ] E2E: program-out over SRT to a loopback receiver is frame-accurate and reconnects on drop
  - [ ] Headless export of a fixture is byte-identical to the GUI export path

### `pi-st2110-egress` — ST 2110-10/-20/-30/-40 network egress with PTP pacing, SDP and SMPTE 2022-7 redundancy
- **Size:** XL
- **Dependencies:** `pi-nmos-control`
- **Definition:** Turn the ST 2110 framer into an actual PTP-paced network sender with SDP and SMPTE 2022-7 seamless redundancy — genlock-grade IP output.
- **Acceptance criteria:**
  - [ ] Extend tests/e2e/run_iotarget_cadence_e2e.sh: captured RTP conforms to ST 2110-21 narrow (VRX <= 8) via a pcap analyzer; a receiver node locks and decodes
  - [ ] 2022-7 single-leg drop shows 0 visible glitch on the receiver

### `pi-tally-gpi-is07` — Tally / GPI / vision-mixer integration (with IS-07)
- **Size:** M
- **Dependencies:** `pi-nmos-control`
- **Definition:** Make the replay station a live-gallery citizen: tally in/out (on-air + preview), GPI trigger in/out, and IS-07 event transport tied into the take chain.
- **Acceptance criteria:**
  - [ ] tst_tally + e2e: an IS-07 tally-in event lights the on-air lamp and (if configured) arms the cut within 1 frame; a GPI-in edge triggers a recall; tally-out mirrors the program bus

### `qe-nmos-conformance` — AMWA NMOS IS-04/IS-05 conformance in CI
- **Size:** L
- **Dependencies:** none
- **Definition:** Stand up the official AMWA NMOS Testing Tool against the app's IS-04/IS-05 node so discovery/connection-management is certifiably correct.
- **Acceptance criteria:**
  - [ ] AMWA NMOS Testing Tool IS-04 + IS-05 auto-test suites report green nightly
  - [ ] Results archived as certification evidence

### `rel-alerting` — Rule-driven alerting & escalation (SNMP / webhook / dry-contact / on-air banner)
- **Size:** M
- **Dependencies:** `rel-telemetry-bus`
- **Definition:** Deliver actionable alerts on defined fault conditions over the transports a broadcast facility actually uses, so unattended failures are noticed immediately.
- **Acceptance criteria:**
  - [ ] tst_alerting: each rule raises/clears with correct hysteresis and no flapping under a noisy metric series
  - [ ] Integration: a forced storageCritical fires a webhook payload and an on-screen banner; ack clears it

### `rel-hot-standby-recorder` — Hot-standby / mirrored-recorder redundancy with primary election + planned-maintenance graceful drain
- **Size:** XL
- **Dependencies:** `rel-redundant-source-failover`, `rel-session-restore`, `rel-nmos-observability`
- **Definition:** Run a mirrored instance recording the same sources so a total primary failure loses zero content and playback can source from either — the N+1 posture pro replay operations require.
- **Acceptance criteria:**
  - [ ] e2e tst_hot_standby: two instances recording the same two sources; SIGKILL the primary; the standby's recording is continuous and content-complete across the failure, and output re-homes to it
  - [ ] Split-brain test: a network partition never yields two masters driving the same output

### `rel-nmos-observability` — NMOS IS-07 event/alarm emission tied to the ops telemetry bus
- **Size:** L
- **Dependencies:** `rel-telemetry-bus`, `rel-alerting`
- **Definition:** Register the recorder and its sources/outputs to the NMOS control plane so broadcast orchestration and monitoring can discover, connect, and receive alarms per the standard.
- **Acceptance criteria:**
  - [ ] Against a reference NMOS registry (e.g. an open-source implementation in CI), the node registers, appears in IS-04 queries, an IS-05 connection call re-points a source, and an IS-07 alarm is observed on a forced fault
  - [ ] Schema-validates against the NMOS specs

### `rel-redundant-source-failover` — Dual/redundant source feeds with hitless (ST 2022-7-class) failover preserving PTS/timeline continuity
- **Size:** XL
- **Dependencies:** `rel-degradation-ladder`, `rel-worker-watchdog`
- **Definition:** Let a view be backed by primary+backup feeds and switch to the healthy one at a clean boundary with a bounded, measured gap, keeping the recording continuous.
- **Acceptance criteria:**
  - [ ] e2e tst_source_failover: two SRT feeds with common TC; killing the active leg switches to the backup with the recorded switch gap <= 1 frame and no track freeze; recording stays continuous across the switch
  - [ ] Without common TC the switch is bounded and the measured gap is reported honestly (not hidden)

### `tc-ltc-vitc-anc` — LTC / VITC / ANC (RP 188) timecode in and out
- **Size:** M
- **Dependencies:** `hwio-capture-half`
- **Definition:** Add the LTC / VITC / RP188 ANC timecode carriers a facility expects alongside the SDI path.
- **Acceptance criteria:**
  - [ ] injected LTC and VITC read back frame-exact on a downstream device

---

[« Phase 4](./phase-4-operator-ux.md) · [Index](./README.md) · [Implementation plan](./impl/phase-5-facility-interop.md) · [Phase 6 »](./phase-6-genlock-certification.md)

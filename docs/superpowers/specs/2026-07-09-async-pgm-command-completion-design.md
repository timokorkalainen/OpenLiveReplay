# Async PGM command completion design

**Status:** approved design, revised after an independent adversarial review
(33 confirmed findings folded in); implementation not started
**Platforms:** all (control plane is platform-agnostic)
**Related specs:** `2026-07-08-pgm-first-transactional-output-design.md`

## Problem

The PGM-first transactional output work gave external operator commands
(`transport.seek`, `transport.stepFrame`, `action.jog`) a transactional mode: when a
client sends `waitForPgm:true`, the command acknowledges only after the program
output has accepted the target frame. That mode is delivered by a synchronous block:

- The control plane — `ControlWebSocketServer`, `UIManagerControlAdapter`,
  `UIManager` — is constructed on the main thread (`main.cpp`) and never moved to
  another thread. WebSocket messages are handled on the GUI thread.
- With `waitForPgm:true` (gated by `wantsPgmWait`,
  `websocket/uimanagercontroladapter.cpp`), the seek/step/jog handlers call
  `UIManager::seekPlaybackAndWaitForPgm(..., kControlPgmWaitTimeoutMs = 250)` →
  `PlaybackWorker::seekToAndWaitForPgm()`, which blocks the GUI thread on a
  `QWaitCondition` for up to 250 ms on a cache miss.

Two facts frame the actual defect:

1. **The blocking path is opt-in and currently used only by the E2E drivers**
   (`tests/e2e/macos_app_driver.py`, `tests/e2e/ios_marker_srt_oracle.py`). Default
   WebSocket seek/step/jog, StreamDeck, MIDI, and QML all use the non-blocking
   fire-and-forget path today.
2. **The default path gives the operator no PGM confirmation at all.** A client that
   wants the PGM guarantee must opt into `waitForPgm:true` and thereby stall the
   GUI/control event loop and serialise every other control client behind the wait.

So the transactional contract is, as designed today, unusable by production control
surfaces: adopting it would buy exactly the head-of-line GUI blocking that the
PGM-first spec warned against ("must not freeze the UI"). This spec makes the
transactional contract non-blocking so real clients can use it.

## Goal

One transactional contract, delivered in two phases and blocking nothing:

- a command with `waitForPgm:true` is accepted immediately with an
  `ok:true, status:"accepted"` acknowledgement carrying the seek generation;
- the playback worker resolves the PGM transaction on its own thread exactly as
  today;
- the originating client — and only it — receives one correlated
  `command.completed` event carrying the outcome (PGM identity and latency on
  success; `superseded` / `timeout` / `pgm_not_submitted` otherwise);
- no shared event loop ever parks on a wait condition.

## Scope decision: fate of `waitForPgm`

`waitForPgm:true` **selects the two-phase transactional contract**. The blocking
WebSocket behaviour is removed, not preserved behind the flag:

- Commands **with** `waitForPgm:true`: immediate `accepted` ack + one
  `command.completed` event. The blocking `seekPlaybackAndWaitForPgm` call
  disappears from the adapter.
- Commands **without** it: today's plain non-transactional `seekPlayback()` /
  `jogExternal()` branches, unchanged — no transaction registration, no completion
  event, no added per-scrub-sample PGM dispatch load.
- The C++ blocking API `PlaybackWorker::seekToAndWaitForPgm()` **remains** for unit
  tests; nothing on the WebSocket path calls it anymore.

The in-repo consumers of the blocking ack migrate in the same change (see Backward
compatibility): both E2E drivers and the static guard tests that pin the blocking
shape.

## Non-goals

- Do not weaken the substance of the PGM guarantee — the confirmation still reports
  the committed PGM frame identity; it arrives as the second message.
- Do not change the default (non-`waitForPgm`) command path or its load profile.
- Do not rework StreamDeck/MIDI feedback (they consume snapshot/patch state and never
  used the blocking ack).
- Do not remove `seekToAndWaitForPgm()` from C++.

## Architecture

```
WS client            Server (GUI thread)           Adapter (GUI thread)          Worker thread
 seek{id:7,          textMessageReceived ──▶ executeCommand("transport.seek")
   waitForPgm:true}                            ├─ registry.registerPending(epoch,gen,client,id:7)
                                               ├─ uiManager.seekPlaybackAsync(ms) ─▶ requestSeekTo() → gen=42
 ◀ ack{id:7,ok:true, ◀ CommandResult::accepted{epoch,gen} ┘   (cache-hit: completes inline, see §1)
   status:accepted,gen}
                        ... GUI thread free ...                ... worker decodes + PGM submits ...
                                                                 completeOperatorSeekTransaction(42)
                     event{command.completed}  ◀ registry ◀── UIManager relay ◀ emit operatorSeekCompleted(42, result)
 ◀ {type:event,name:command.completed,            (explicit Qt::QueuedConnection, result passed by value)
    data:{id,gen,done,pgmPts,latencyMs}}
```

### 1. Non-blocking enqueue with the cache-hit clause (`UIManager` / `PlaybackWorker`)

`UIManager::seekPlaybackAsync(int64_t ms) -> {workerEpoch, generation}` (and the
step/jog equivalents) call
`PlaybackWorker::requestSeekTo(ms, dir, /*registerOperatorTransaction=*/true)` and
return without entering any wait loop.

**Cache-hit clause (load-bearing).** The inline PGM dispatch for a covered target is
the *caller's* job today — it lives in `seekToAndWaitForPgm`
(`playbackworker.cpp:318-322`), not in `requestSeekTo`. When the returned
`SeekRequestResult` has `committedFromPublishedCache = true`, the worker will never
reposition for this generation (`m_seekTargetMs` is already cleared), so the async
enqueue must perform the same inline completion the synchronous path performs:
`dispatchPgmAfterSeekCommit(clampedTargetMs)` +
`completeOperatorSeekTransaction(...)` + `refreshPreviewAfterSeekCommit()`. Without
this clause the common warm-scrub case never dispatches PGM and every such command
resolves as a bogus timeout. The completion signal fired by this inline path is
delivered safely because the connection is explicitly queued (§2).

### 2. Completion signal — one emission point, explicit queued connection

`PlaybackWorker` emits `operatorSeekCompleted(uint64_t generation,
OperatorSeekResult result)` from **exactly one place**:
`completeOperatorSeekTransaction` — the single point that resolves a transaction
(reposition commit, output-cache completion, and the §1 inline cache-hit path all
funnel there). The worker computes `submittedPgm` and `pgmIdentity` there. It has
**no** timeout or elapsed-time computation of its own (today's `timedOut` /
`elapsedNs` live in the blocking wait loop, which the async flow does not use):
timeouts are resolved by the registry deadline (§3), and `latencyMs` is computed by
the registry from its accepted timestamp.

**Supersession is detected in the worker**, at the only choke point that sees every
generation bump: when `requestSeekTo` advances `m_seekGeneration` past a
still-waiting, uncompleted transaction — *regardless of whether the new seek
registers a transaction* — it emits
`operatorSeekCompleted(oldGeneration, {done:false, reason:"superseded"})` and clears
the orphaned slot. This covers supersession by other external commands **and** by
QML scrubs, live-follow, playlist jumps, and goLive, which bump the generation
without registering transactions and would otherwise strand pendings until the
deadline.

**Connection discipline (normative).** All `operatorSeekCompleted` connections MUST
be explicit `Qt::QueuedConnection`, and `OperatorSeekResult` MUST be passed by value
(a snapshot, never a reference into mutex-protected state). `Qt::AutoConnection` is
wrong here: the §1 cache-hit emission happens on the GUI thread, where an auto
connection is *direct* — the slot would run synchronously under the worker mutex and
(depending on call order) before `executeCommand` returns. Queued delivery runs the
slot after the current event completes, after the pending entry exists.

**Abandonment API.** `PlaybackWorker::abandonOperatorSeekTransaction(uint64_t
generation)` — thread-safe (takes `m_mutex`); if the slot matches `generation` and
is not completed, clears `waiting`, exactly as the synchronous wait-loop timeout does
today. The registry deadline calls this so a timed-out command cannot trigger a late
forced PGM dispatch or leave altered reposition policy behind.

### 3. Pending registry — standalone, testable component

A new `websocket/pendingcommandregistry.{h,cpp}`: a small standalone QObject with no
`UIManager` dependency (unit-testable at the `ci` label):

- `registerPending(epoch, generation, clientId, commandId, deadlineMs)` — registers
  one entry; if an older entry for the same epoch is still pending, it is *not*
  closed here (the worker's superseded emission is authoritative), but the deadline
  timer is armed.
- `onOperatorSeekCompleted(epoch, generation, resultJson)` — resolves the matching
  entry (one-shot; late or unmatched signals are dropped).
- per-entry deadline (single `QTimer` on the registry's thread, order of ~1 s —
  generous, since nothing blocks) — resolves `done:false, reason:"timeout"`, emits
  the abandonment callback (§2), and drops the entry.
- `onClientDisconnected(clientId)` — drops that client's entries.
- signal `commandCompleted(clientId, commandId, QJsonObject completion)`.

**Epoch keying.** `PlaybackWorker` is destroyed and recreated per playback session
(`uimanager.cpp` start/restart paths) and its generation counter restarts, so bare
generations collide across workers. `UIManager` maintains a monotonic
`workerEpoch`, incremented at each worker construction; the async enqueue returns
`{epoch, generation}`; the registry keys entries by the pair; on epoch change,
still-pending entries of the old epoch resolve as `superseded`.

**Wiring/ownership.** The worker→control connection cannot be made once: the adapter
holds only `UIManager*` and the worker is null at control-plane construction and
replaced per session. `UIManager` therefore owns a stable relay signal
`operatorSeekCompleted(epoch, generation, result)`; the queued
worker→`UIManager` connection is re-made immediately after each `new
PlaybackWorker` (both the start-recording and restart paths). The adapter connects
once, at construction, `UIManager` → registry. `UIManagerControlAdapter` (already a
QObject) declares the outward `commandCompleted` signal by forwarding the
registry's; the abstract `ControlApiAdapter` interface stays a plain non-QObject and
is unchanged, so existing fakes are unaffected.

### 4. Completion delivery (`ControlWebSocketServer`)

The server connects to the concrete adapter's `commandCompleted(clientId, commandId,
completion)` and sends the event to the matching socket only.

- **Client identity (normative):** `controlClientId` MUST be unique for the server's
  lifetime — a monotonic serial (`QString::number(++m_nextClientSerial)`) assigned in
  `handleNewConnection`, **not** the socket pointer value used today
  (`controlwebsocketserver.cpp:126`), which can be reincarnated by heap reuse and
  misroute a completion to a different client.
- The server keeps `clientId → QWebSocket*`, erased in `handleSocketDisconnected`; a
  completion whose clientId is absent is silently dropped; the disconnect also calls
  `registry.onClientDisconnected(clientId)` (via the adapter) so pendings don't
  linger to their deadline.

## Semantics

- **One-shot, first-writer-wins.** Each pending entry resolves exactly once, by the
  first of: worker completion, worker superseded emission, epoch change, client
  disconnect, or registry deadline. Late signals for resolved entries are dropped.
- **Four completion outcomes:**
  - `done:true` with `pgmPts` + `latencyMs` — PGM accepted the committed target;
  - `done:false, reason:"pgm_not_submitted"` — the seek committed but no PGM frame
    was submitted (`requiredSubmitted=false`: e.g. no PGM NDI output configured, or
    the critical dispatch failed) — mirrors the synchronous path's failure code;
  - `done:false, reason:"superseded"` — a newer seek (external or local) replaced it;
  - `done:false, reason:"timeout"` — the target never committed before the registry
    deadline (frame not yet recorded); the worker transaction is abandoned (§2).
- **`latencyMs`** is registry-measured: accepted-timestamp → completion arrival. The
  authoritative PGM latency gate remains the NDI marker measurement in the E2E.
- **Ordering / multiple clients.** One playhead, one worker: generations linearise
  seeks; concurrent clients resolve as last-generation-wins, others `superseded`.

## Protocol

Additive; both messages reuse existing envelopes:

- Immediate acknowledgement — keeps the `ok` discriminator every existing client
  checks: `{type:"ack", id, ok:true, status:"accepted", generation, workerEpoch}`.
  Concretely `CommandResult::accepted(epoch, generation)` → `success(details)` with
  `status:"accepted"` in the details.
- Completion — the server's established event envelope (`{type:"event", name, data}`),
  sent only to the originating socket:
  `{type:"event", name:"command.completed", data:{id, generation, done, pgmPts,
  latencyMs, reason?}}`.
- A command sent with `waitForPgm:true` but **no `id`** cannot be correlated; it is
  rejected with a validation error (the protocol already validates per-command
  arguments) rather than accepted-then-orphaned.

## Backward compatibility — full consumer inventory

Consumers of the current blocking `waitForPgm` ack, all migrated in this change:

1. `tests/e2e/macos_app_driver.py` — stops treating the ack as the PGM confirmation;
   correlates `command.completed` (by `id`) for transaction evidence; keeps NDI
   marker latency assertions; adds an assertion that the `accepted` ack round-trips
   fast even when the marker arrives later (direct proof the control path no longer
   blocks).
2. `tests/e2e/ios_marker_srt_oracle.py` — same migration; its
   `require_pgm_transaction` ack-shape assertions (`pgmTransaction` fields) move to
   the `command.completed` payload.
3. Static guard tests that pin the blocking shape
   (`tests/e2e/test_control_adapter_wait_static.py` and the related driver static
   tests) — re-pinned to the two-phase contract: adapter must *not* call the blocking
   wait; completion must flow through the registry.

Clients that read only the first ack still observe `ok:true` and the command still
executes. The two-phase contract is documented as a protocol addition.

## Testing contract

- **Registry** (`tst_pendingcommandregistry`, new, `ci` label — no UIManager
  dependency): register→complete resolves once with correct payload; late/unmatched
  signals dropped; deadline resolves `timeout` and fires the abandonment callback;
  epoch change resolves old entries `superseded`; `onClientDisconnected` drops
  entries; one-shot under interleaved resolve/deadline races.
- **Worker** (`tst_playbackworker`): `QSignalSpy` on `operatorSeekCompleted` —
  (a) resolving a transaction emits once with correct `submittedPgm`/`pgmIdentity`;
  (b) a `requestSeekTo` generation bump over a waiting transaction emits
  `superseded` — including from a *non*-operator seek; (c) `abandonOperatorSeekTransaction`
  suppresses later completion; (d) the async enqueue's cache-hit clause dispatches
  PGM and completes inline (assert via the PGM sink + spy, no wait loop involved).
- **Non-blocking guard (structural, not timing):** with a transaction that never
  resolves, the async enqueue returns `{epoch, generation}` while
  `hasOperatorSeekTransaction` still reports waiting — proving no wait occurred; and
  the adapter's `waitForPgm:true` branch is pinned (static guard) to call the async
  enqueue, never `seekPlaybackAndWaitForPgm`.
- **Protocol** (`tst_controlprotocol`): `accepted` ack keeps `ok:true`;
  `command.completed` uses the event envelope; `waitForPgm:true` without `id` is
  rejected.
- **E2E:** the migrated drivers above; NDI marker latency gate unchanged.

## Risks

- The worker-emission + registry split has more moving parts than the blocking call
  it replaces; the registry's one-shot rule and the worker's single emission point
  keep each piece independently testable.
- A queued emit under the worker mutex is safe (posts an event), but only with the
  explicit `Qt::QueuedConnection` discipline of §2 — pinned by tests.
- Supersession vs. completion is a benign first-writer-wins race; either outcome is
  correct for the operator and the tests pin the resolution rule.
- Clients that relied on the blocking ack semantics must adopt the completion event;
  both in-repo consumers migrate in this change and the protocol change is
  documented.

## Acceptance criteria

- No operator command blocks the GUI or control event loop; the adapter never calls
  a blocking wait on any WebSocket path.
- Every accepted `waitForPgm:true` seek/step/jog receives exactly one
  `command.completed` event with one of the four defined outcomes; cache-hit seeks
  complete with PGM evidence (not timeout).
- Local (QML/live-follow/playlist) seeks supersede external pendings promptly —
  no stranded entries mis-reported as `timeout`.
- A registry deadline abandons the worker transaction (no late forced PGM dispatch).
- The macOS app E2E passes with the migrated driver, PGM NDI marker latency within
  the existing gate, and the accepted-ack-fast assertion proving the non-blocking
  path.
- No claim of reduced command-to-PGM latency or eliminated UI stall is made without
  the structural non-blocking guard passing and the E2E ack-vs-marker evidence.

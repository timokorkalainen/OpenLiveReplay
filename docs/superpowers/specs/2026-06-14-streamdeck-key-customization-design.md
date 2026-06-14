# Stream Deck Key Customization Design

**Date:** 2026-06-14
**Status:** Approved design, pending implementation plan
**Target:** iPadOS builds of OpenLiveReplay (Stream Deck integration)
**Builds on:** `docs/superpowers/specs/2026-06-11-streamdeck-integration-design.md`

## Goal

Let users remap which replay action each Stream Deck key, dial-press, and
dial-turn triggers — using a learn-style flow modeled on the existing MIDI
mapping UI. Primary target is the Stream Deck + XL (36 keys, 6 dials, touch
strip), but the design adapts to every model. Mappings persist per deck model.

## Requirements (agreed)

- **Interaction:** Learn-style — a list of *actions*, each with a **Learn**
  button; click it, then press the physical key / press or turn a dial to
  bind it. Mirrors the MIDI mapping UI.
- **Scope:** Keys and dials (both turn and press) are bindable. The touch
  strip stays fixed as the scrub bar + timecode surface.
- **Persistence:** Per deck-model, stored in `AppSettings` JSON via the
  existing `SettingsManager`, alongside the MIDI bindings.
- **Defaults:** With no saved map for a model, the existing built-in default
  layout applies (works untouched). User edits override and persist.

## Non-goals (YAGNI)

- Making the touch strip's content selectable (stays scrub + timecode).
- New continuous (rotate) actions beyond Jog. Dial-turn binds Jog only; the
  model is extensible but no new rotate actions ship here.
- Editing mappings while no deck is connected (learn-style needs the device).
- Multiple simultaneous decks of the same model with distinct maps (one map
  per model id).

## Data model

Three per-model tables. The key table already exists in `DeckState`; two dial
tables are new. Keys are model ids (`"plusXL"`, `"mini"`, …); `-1` = unbound.

| Table | Index → value | Notes |
|-------|---------------|-------|
| `keyMap[model]` | keyIndex → actionId | exists today |
| `dialPressMap[model]` | dialIndex → actionId | new; discrete actions |
| `dialRotateMap[model]` | dialIndex → actionId | new; Jog only today |

### Assignable items (the editor list)

| Item | Action id | Bindable to |
|------|-----------|-------------|
| Play/Pause | 0 | key or dial-press |
| Rewind 5× | 1 | key or dial-press |
| Forward 5× | 2 | key or dial-press |
| Next Frame | 3 | key or dial-press |
| Go Live | 4 | key or dial-press |
| Capture | 5 | key or dial-press |
| Multiview | 6 | key or dial-press |
| Prev Frame | 7 | key or dial-press |
| Jog | 8 | **dial-turn only** |
| Record | 9 | key or dial-press |
| Timecode | 20 | **key only** (display) |
| Speed | 21 | **key only** (display) |

Action ids are unchanged from the integration spec; this feature only changes
*which control* maps to each id.

### Move / displace semantics (MIDI parity)

Learning action `A` onto control `C`:
1. Remove `A` from its previous control (set that slot to `-1`).
2. Set `C`'s slot to `A`.
3. If `C` previously held a different action `B`, `B` is displaced →
   becomes Unassigned.

Invariant: each action occupies ≤1 control; each control shows ≤1 action.
No confusing duplicates, and the editor can show one binding label per action.

### Element types

The bridge reports the learned control as an `elementType` int:
`0` = key, `1` = dial-press, `2` = dial-turn. This same enum is used by
`learnInput` and by `StreamDeckMappingStore::bind`.

### Validation

Enforced in C++ before a binding is applied (invalid input is ignored and
learn mode keeps listening):
- Jog (8): only `dial-turn`.
- Timecode (20) / Speed (21): only `key`.
- All other actions: `key` or `dial-press`.

## Architecture

```
  QML "Button Mapping" group (Main.qml, inside the Stream Deck card)
        │  beginStreamDeckLearn / clearStreamDeckBinding / resetStreamDeckDefaults
        │  reads: streamDeckLearnAction, streamDeckBindingLabel(a), version, connected/keyCount/dialCount
        ▼
  UIManager ── owns ──► StreamDeckMappingStore  (new, cross-platform, unit-tested)
        │                  • 3 per-model tables
        │                  • bind() applies move/displace + validation
        │                  • bindingLabel(action), clear(), resetToDefault(model)
        │                  • toJson()/fromJson()  (AppSettings + SettingsManager)
        │
        │ setLearnMode(bool) ▲   ▲ learnInput(elementType,index)   ▼ push maps
        ▼                    │   │                                  │
  StreamDeckManager (QObject; .mm on iOS, stub elsewhere)
        │  Q_PROPERTY connected/keyCount/dialCount/deviceModel
        │  setLearnMode / signal learnInput / setKeyMapping / setDialMapping
        ▼  @objc
  OLRStreamDeckBridge  ──►  DeckState (keyMappings + dialRotate/PressMappings)
        │                         └─► ReplayDeckLayout / ReplayDialStrip
        ▼ USB
  Stream Deck + XL
```

### Components & responsibilities

1. **`StreamDeckMappingStore`** (`streamdeck/streamdeckmappingstore.{h,cpp}`,
   new, plain cross-platform C++ class — no Qt signals; `UIManager` emits the
   QML-facing change signals) — the three per-model tables,
   `bind(action, elementType, index)` applying move/displace + validation,
   `clear(action)`, `bindingLabel(action)`, `resetToDefault(model, keyCount,
   dialCount)`, and JSON (de)serialization into `AppSettings`. No iOS deps;
   this is where the unit tests live.

2. **`StreamDeckManager`** (existing; extend) — stops discarding the connect
   payload. New Q_PROPERTYs `keyCount`, `dialCount` (and existing
   `deviceModel`). New: `setLearnMode(bool)`, signal `learnInput(int
   elementType, int index)`, `pushMaps(keyMap, dialRotateMap, dialPressMap)`
   forwarding to the bridge. The stub no-ops all of it.

3. **`OLRStreamDeckBridge`** (existing; extend) —
   - `@objc func setLearnMode(_ active: Bool)`
   - `@objc var onLearnInput: ((Int, Int) -> Void)?`  (elementType, index)
   - `@objc func setDialMapping(rotate: [Int], press: [Int], forModel: String)`
   - In learn mode, key/dial events call `onLearnInput` instead of dispatching.

4. **`DeckState`** (existing; extend) — add `dialRotateMappings` and
   `dialPressMappings` (same change-guard pattern as `keyMappings`), plus
   `rotateAction(forDial:model:)` / `pressAction(forDial:model:)` lookups.

5. **`ReplayDialStrip`** (existing; extend) — scrub bar + timecode stays as
   the dial-screen background (tap = seek, unchanged). Each dial section
   overlays its bound action's icon/label. Rotation fires the dial's rotate
   action (Jog → scrub); press fires the dial's press action.

6. **`UIManager`** (existing; thin wiring) — owns a `StreamDeckMappingStore`;
   exposes the QML API; routes `beginStreamDeckLearn(action)` →
   `setLearnMode(true)`, and `StreamDeckManager::learnInput` →
   `store.bind(...)` → persist via `SettingsManager` → `pushMaps` →
   `setLearnMode(false)` → bump `streamDeckBindingsVersion`.

## Data flow

**Learn:** QML `beginStreamDeckLearn(8)` → UIManager records pending action,
calls `streamDeck->setLearnMode(true)`, sets `streamDeckLearnAction=8` →
bridge routes raw input → user turns dial 2 → `learnInput(2, 2)` → UIManager
validates (Jog needs turn ✓) → `store.bind(8, turn, 2)` (move/displace) →
`SettingsManager::save` → `streamDeck->pushMaps(...)` → bridge updates
`DeckState` → deck re-renders live → `setLearnMode(false)`,
`streamDeckLearnAction=-1`, `streamDeckBindingsVersion++` → QML refreshes.

**Normal dispatch (unchanged ids):** key press → `keyMap` → `actionTriggered`;
dial press → `dialPressMap` → `actionTriggered`; dial turn → `dialRotateMap`
→ `jogTriggered`. All flow through the existing `UIManager` dispatch.

**Connect:** on `onDeviceConnected(name, model, keyCount, dialCount)`,
UIManager pushes the persisted (or default) maps for `model` before the first
render, so a freshly connected deck shows the user's layout.

## QML editor

New "Button Mapping" `GroupBox` in the Stream Deck card. Visible only when
`streamDeck.connected`; otherwise a hint to connect the deck. A `Repeater`
over the 12 items renders rows of: name · binding label
(`streamDeckBindingLabel(action)`) · **Learn** (→ "Listening…" when
`streamDeckLearnAction === action`) · **Clear**. A header row carries **Reset
to default**. Learn buttons hint the gesture ("turn a dial" for Jog, "press a
key" for Timecode/Speed). Refresh is driven by `streamDeckBindingsVersion`,
exactly like `midiBindingsVersion`.

## Error handling & edge cases

- **No deck connected:** editor hidden; learn calls are no-ops (stub or
  guarded).
- **Disconnect mid-learn:** `connectedChanged(false)` cancels learn
  (`setLearnMode(false)`, `streamDeckLearnAction=-1`).
- **Invalid gesture during learn** (e.g., key while learning Jog): ignored,
  keeps listening.
- **Re-learn / second Learn click:** cancels the prior listening row.
- **Unknown/legacy model id in saved JSON:** loaded as-is; applied only when
  that model connects. Corrupt entries fall back to defaults for that model.
- **keyCount/dialCount smaller than a saved index:** out-of-range bindings are
  dropped on load for the connected geometry.

## Testing

- **`StreamDeckMappingStore` unit tests** (new, cross-platform — the core
  payoff): move/displace semantics; validation (Jog→turn only, TC/Speed→key
  only, others key|press); per-model isolation; JSON round-trip; reset-to-
  default reproduces the built-in layout; out-of-range pruning on load.
- **Swift bridge XCTest** (extend): `DeckState` dial tables change-guarding;
  `rotateAction/pressAction(forDial:)` lookups; learn-mode routing reports raw
  elements and suppresses dispatch.
- **Simulator pass**: via the debug Stream Deck simulator, learn a key, a
  dial-press, and Jog onto a dial; confirm live re-render, displace behavior,
  Clear, Reset, and persistence across relaunch.

No C++ test harness predates this; `StreamDeckMappingStore` tests slot into
the existing `tests/unit` suite (added by the playback work on main).

## File structure

- Create: `streamdeck/streamdeckmappingstore.{h,cpp}`
- Create: `tests/unit/tst_streamdeckmappingstore.cpp` (+ register in
  `tests/unit/CMakeLists.txt`)
- Modify: `streamdeck/streamdeckmanager.{h,mm}` + `_stub.cpp` (learn mode,
  geometry props, dial-map push, `learnInput` signal)
- Modify: `ios/streamdeck-bridge/Sources/OLRStreamDeckBridge.swift` (learn
  mode, `onLearnInput`, `setDialMapping`)
- Modify: `ios/streamdeck-bridge/Sources/DeckState.swift` (dial tables +
  lookups)
- Modify: `ios/streamdeck-bridge/Sources/ReplayDeckLayout.swift`
  (`ReplayDialStrip` per-dial labels + bound press/rotate)
- Modify: `ios/streamdeck-bridge/Tests/DeckStateTests.swift` (dial tables,
  learn routing)
- Modify: `settingsmanager.h` (`AppSettings` gains the three per-model tables)
  + `settingsmanager.cpp` (serialize/deserialize)
- Modify: `uimanager.{h,cpp}` (own the store, QML API, wiring)
- Modify: `Main.qml` (Button Mapping group)

## Approaches considered

1. **Learn-style, action-centric, dedicated mapping store (chosen).** Matches
   the MIDI UI the user knows; "press the thing" solves which-of-36-keys;
   move/displace keeps a clean binding model; the store isolates testable
   logic from the large `UIManager` and the iOS bridge.
2. **Visual grid editor** (tap a key in an on-screen deck grid, pick an
   action). More intuitive spatially, but more QML, and the user explicitly
   wanted MIDI parity. Rejected.
3. **Inline mapping logic in `UIManager`** (like MIDI today). Fewer files, but
   `UIManager` is already ~1500 lines and the three-table + validation +
   serialization logic is non-trivial and worth unit-testing in isolation.
   Rejected in favor of the store.

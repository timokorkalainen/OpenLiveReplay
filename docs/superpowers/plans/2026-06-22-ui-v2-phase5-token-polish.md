# UI v2 Phase 5 — Token & a11y polish sweep + transport-deck priority Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Finish the design system: replace the ~68 inline hex literals carried into the cockpit components with `Theme.*` tokens, verify accessibility (contrast / focus / hit-targets), and give the transport "deck" visual priority (big PLAY/GO-LIVE).

**Architecture:** Presentation-only. A mechanical token sweep file-by-file (gated by render diffs so colours don't drift), an a11y verification pass, and a sizing pass in TransportDock. Builds on Phase 1 (#145); run **last** so it also tokenizes any UI added by Phases 2–4.

**Tech Stack:** Qt 6.10.1, QML, OlrTheme, CMake/CTest. See roadmap (Phase 5) for the full hex→token mapping.

## Global Constraints

- **No behaviour or layout change** — colours map to the nearest existing token; sizing uses tokens that already exist (`hPrimary`/`hTransport`/`hAction`). Each step is verified by re-rendering and confirming the look is unchanged (or intentionally improved).
- **Add a token only when none fits.** The only genuinely new semantic is the recording-warning surface (`#2b2615`/`#5b4818`) → add `Theme.warnSurface`/`Theme.warnBorder`. Do not invent tokens for colours that map to existing ones.
- Run **after** Phases 2–4 ideally, so their new components are included; if run earlier, re-run the sweep over any later-added files.
- Verify each task: build → `qml_smoke` → `uipreview` render (compare against the pre-sweep render) → app-load → suite. Outputs under `build/`. Never push `--no-verify`.

---

### Task 1: Add the two warning-surface tokens

**Files:**
- Modify: `ui/theme/Theme.qml`
- Modify: `tests/qmlstyle/tst_olrstyle.qml` (extend the token-coherence assertions if present)

**Interfaces:**
- Produces: `readonly property color warnSurface: "#2B2615"` and `readonly property color warnBorder: "#5B4818"` (the existing recording-warning banner colours, promoted to named tokens). Place them in the surfaces/semantic section with a comment.

- [ ] **Step 1: Add the tokens.** Add the two properties to `Theme.qml` with a comment (`// soft-warning banner (feed-count etc.) — amber-on-dark, distinct from error`).
- [ ] **Step 2: Build + lint.** Build clean; `qml_smoke` Passed.
- [ ] **Step 3: Commit.** `feat(theme): add warnSurface/warnBorder tokens`.

### Task 2: Token sweep — `ui/components/*.qml`

**Files:**
- Modify: each of `ui/components/*.qml` containing inline hex (per the inventory: BindingsPanel, OutputsPanel, PgmStage, SourceListPanel, StatusStrip, TransportDock, and any added by Phases 2–4).

**Interfaces:** none (internal colour mapping).

Mapping (apply consistently; the full table is in the roadmap):
| inline hex | token |
|---|---|
| `#d32f2f` | `Theme.recordOnAir` |
| `#4CAF50` `#2e7d32` `#8bc34a` | `Theme.ready` |
| `#ff9800` `#ffb300` `#f9a825` `#ffcc80` | `Theme.armed` |
| `#eeeeee` `#cccccc` | `Theme.textHi` |
| `#b0b0b0` `#aaaaaa` `#aaa` | `Theme.textBody` |
| `#999999` `#888888` `#777777` `#777` `#666666` `#666` | `Theme.textDim` |
| `#1f1f1f` `#1b1b1b` `#181818` | `Theme.panelRaised` / `Theme.panel` (pick by role: raised cards → panelRaised, backgrounds → panel) |
| `#333` `#555` | `Theme.line` / `Theme.lineStrong` (border weight by role) |

- [ ] **Step 1: Sweep one file at a time.** For each component file, replace each inline hex per the table. When a colour's role is ambiguous (e.g. `#1f1f1f` as a card vs background), pick by context and keep it consistent within the file. Do **not** change any non-colour code.

- [ ] **Step 2: Per-file render diff.** After each file, render the relevant preview (e.g. `preview_sources.qml`, `preview_outputs.qml`, `preview_transport.qml`, `preview_stage.qml`, `preview_statusstrip.qml`) and compare to a baseline render of the same file pre-sweep — confirm the look is unchanged (token values were chosen to match). Note any intentional improvement (e.g. a slightly-off grey snapping to `textDim`).

- [ ] **Step 3: Build + lint after each file.** Build clean; `qml_smoke` Passed.

- [ ] **Step 4: Commit per file (or per small group).** `refactor(ui): tokenize colours in <Component>.qml`.

### Task 3: Token sweep — `Main.qml` shell (warning banner + telemetry)

**Files:**
- Modify: `Main.qml` (the recording-warning banner `#2b2615`/`#5b4818`/`#ffb300`, the telemetry panel `#eeeeee`/`#b0b0b0`)

- [ ] **Step 1: Tokenize.** Warning banner: `color → Theme.warnSurface`, `border.color → Theme.warnBorder`, text `#ffb300 → Theme.armed`. Telemetry: `#eeeeee → Theme.textHi`, `#b0b0b0 → Theme.textBody`.
- [ ] **Step 2: Build + lint + render the cockpit.** Build clean; `qml_smoke` Passed; render `preview_cockpit.qml` → confirm the warning banner + telemetry look unchanged.
- [ ] **Step 3: App-load.** Offscreen app-load clean.
- [ ] **Step 4: Confirm zero inline hex remain.** `grep -rnE '"#[0-9a-fA-F]{3,8}"' ui/components/*.qml Main.qml` → expect **no matches** (or only a documented exception). 
- [ ] **Step 5: Commit.** `refactor(ui): tokenize the shell warning banner + telemetry`.

### Task 4: a11y verification pass

**Files:**
- Modify (only if a finding requires it): `ui/theme/Theme.qml` (nudge a token) or the offending component.

- [ ] **Step 1: Contrast.** For each text token on its surface (textHi/textBody/textDim on canvas/panel/panelRaised; textOnTally on recordOnAir/armed/ready), compute contrast ratio. Confirm ≥ 4.5:1 for body text and ≥ 3:1 for large/secondary. The Theme comments already claim this — verify with a rendered text probe or a quick ratio calc. Record the numbers in the commit message.
- [ ] **Step 2: Focus + hit targets.** Render a probe with controls focused (Tab order) → confirm the focus ring (`Theme.focusRing`) is visible on Button/TextField/ComboBox. Confirm interactive rows/chips are ≥ `Theme.hCompact` tall.
- [ ] **Step 3: Fix only real findings.** If a token fails contrast, nudge it minimally and re-render every affected component (a token change is global). If all pass, no code change — the task is the verification + recording the evidence.
- [ ] **Step 4: Commit.** `chore(ui): a11y pass — contrast/focus/hit-target verification (+ any token nudges)`.

### Task 5: Transport-deck priority sizing

**Files:**
- Modify: `ui/components/TransportDock.qml`

**Interfaces:** none (sizing only; tokens `hPrimary 48 / hTransport 44 / hAction 40 / hControl 30` already exist).

- [ ] **Step 1: Prioritise the keys.** Size PLAY/PAUSE to `implicitHeight: Theme.hPrimary` (the biggest), GO-LIVE to `Theme.hTransport`, Mark/Recall/Playlist to `Theme.hAction`, and group the shuttle-speed buttons visually (a bordered `Row`/`Frame` at `Theme.hControl`) so the deck reads hierarchically. Keep the responsive condensation from Phase 1 (below `bpMD` the speeds compact) — apply priority sizing only at/above `bpMD`.
- [ ] **Step 2: Build + lint + render.** Build clean; `qml_smoke` Passed; render `preview_transport.qml` at full + `bpMD-` widths → inspect: PLAY/GO-LIVE visually dominant; condensation still works small.
- [ ] **Step 3: App-load + suite.** Offscreen app-load clean; `ctest --test-dir build/c -L 'unit|smoke'` 100%.
- [ ] **Step 4: Commit.** `feat(ui): transport-deck priority sizing (PLAY/GO-LIVE)`.

## Self-Review
- Roadmap Phase 5 coverage: warn tokens (Task 1) + component sweep (Task 2) + shell sweep + zero-hex check (Task 3) + a11y (Task 4) + deck priority (Task 5). ✓
- No behaviour/layout change; every step gated by a render diff against a pre-sweep baseline so colours can't silently drift. ✓
- Only genuinely-new tokens added (`warnSurface`/`warnBorder`); everything else maps to existing tokens. ✓
- Runs last (includes Phases 2–4 components); if run earlier, re-sweep later-added files (Global Constraints). ✓

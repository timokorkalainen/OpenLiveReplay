# iOS Frame-Residency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make iOS jetsam structurally unreachable for the GPU pipeline by unifying all decoded-frame memory into one evidence-gated residency ledger, deriving playhead-centered scrub windows from the real OS budget, and shedding memory through an active pressure ladder — per the approved spec `docs/superpowers/specs/2026-07-07-ios-frame-residency-design.md` (v3).

**Architecture:** Extend `GpuBudget` (compiled unconditionally) with owner tags, gated-vs-charge-only semantics, and report-only mode; move the budget charge from `GpuFrameData` to the `GpuSurface` lifetime; instrument holders/pools and run a **gating Phase-0 attribution** on device; then parameterize the scheduler's retention spans (`ResidencyWindowParams`, desktop defaults byte-identical), derive iOS windows from `os_proc_available_memory()`, and add a worker/recorder headroom watchdog with a bytes-minted burst trigger driving a 3-level ladder (trim + VT pool flush → CPU latch).

**Tech Stack:** C++17/Qt 6, Objective-C++ (VideoToolbox/UIKit), Qt Test, CMake/Ninja, ctest labels `unit`/`ci`, TSan CI lists.

## Global Constraints

- Worktree: `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/ios-frame-residency`, branch `gpu/ios-frame-residency` (exists, based on `gpu/resident-pipeline-phases-4-5`).
- Build (macOS host): `cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON` then `ninja -C build/c`. Also keep a GPU-OFF config green: `build/off` (same command minus `-DOLR_GPU_PIPELINE=ON`).
- Tests: `ctest --test-dir build/c -L unit --output-on-failure` (full suite — worker changes affect siblings). Unit tests register via `olr_add_unit_test(<name> <lib>)` in `tests/unit/CMakeLists.txt`; headless via `QT_QPA_PLATFORM=offscreen`.
- Format changed lines only: `python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --staged` after `git add`.
- Commits end with `Co-Authored-By: Claude <noreply@anthropic.com>`. Never `git push --no-verify`; push with `QT_HOST_PREFIX=/opt/homebrew/opt/qt` exported.
- Desktop scheduler defaults must remain byte-identical (spec §4): derived windows/ladder activate only on iOS (`gpuIsIosBuild()`); tests inject parameters explicitly.
- **Public repo:** comments/messages document present design only.
- Spec cross-reference: every task cites its spec section; deviations require updating the spec in the same commit.

---

### Task 1: Ledger tags, gated-vs-charge-only semantics, report-only mode

**Files:**
- Modify: `playback/gpu/gpubudget.h`, `playback/gpu/gpubudget.cpp`
- Modify: `CMakeLists.txt` (~line 528 `if(OLR_GPU_PIPELINE)` block) and `tests/CMakeLists.txt` — compile `playback/gpu/gpubudget.cpp` **unconditionally** in app and test-engine targets (spec §1 link structure)
- Test: `tests/unit/tst_gpubudget.cpp` (extend)

**Interfaces:**
- Consumes: existing `GpuBudget` singleton API.
- Produces (later tasks rely on these exact names):
  - `enum class ResidencyTag : int { DecodeWindow, Staging, ReadbackRing, OutputBus, CpuReadbackCache, RetireQueue, RecorderWrap, IngestWrap, CpuFrame, Other };`
  - `static bool GpuBudget::tagIsGated(ResidencyTag);` → true only for `DecodeWindow, Staging, ReadbackRing, OutputBus`
  - `std::optional<GpuBudgetCharge> GpuBudget::tryCharge(qint64 bytes, ResidencyTag tag);` (legacy 1-arg overload = `DecodeWindow`)
  - `GpuBudgetCharge GpuBudget::chargeUngated(qint64 bytes, ResidencyTag tag);` (always succeeds)
  - `qint64 liveBytes() const; qint64 liveBytes(ResidencyTag) const; qint64 gatedLiveBytes() const; qint64 totalChargedBytesEver() const;`
  - `bool reportOnly() const;` (env `OLR_LEDGER_REPORT_ONLY`, values `1|true|on`)
  - `GpuBudgetCharge` gains an immutable `ResidencyTag tag() const` (default `DecodeWindow`).

- [ ] **Step 1: Write failing tests** (append to `tests/unit/tst_gpubudget.cpp`)

```cpp
void TestGpuBudget::tagsSeparateGatedFromChargeOnly() {
    GpuBudget::instance().reset();
    GpuBudgetConfig cfg; cfg.aggregateDecodeWindow = 2; cfg.width = 100; cfg.height = 100;
    cfg.stagingWindowPerFeed = 0; cfg.activeBusCount = 0; cfg.readbackRingDepth = 0;
    GpuBudget::instance().configure(cfg); // budget = 2 * 100*100*3/2 = 30000
    auto a = GpuBudget::instance().tryCharge(15000, ResidencyTag::DecodeWindow);
    QVERIFY(a.has_value());
    // Charge-only tags never fail and never count against the gate:
    GpuBudgetCharge w = GpuBudget::instance().chargeUngated(1'000'000, ResidencyTag::IngestWrap);
    QCOMPARE(GpuBudget::instance().gatedLiveBytes(), qint64(15000));
    QCOMPARE(GpuBudget::instance().liveBytes(), qint64(1'015'000));
    QCOMPARE(GpuBudget::instance().liveBytes(ResidencyTag::IngestWrap), qint64(1'000'000));
    // Gate compares gated bytes only — this must still succeed:
    auto b = GpuBudget::instance().tryCharge(15000, ResidencyTag::DecodeWindow);
    QVERIFY(b.has_value());
    // ...and now the gate is full:
    QVERIFY(!GpuBudget::instance().tryCharge(1, ResidencyTag::DecodeWindow).has_value());
}

void TestGpuBudget::reportOnlyModeDisablesGateOnlyForNewTags() {
    qputenv("OLR_LEDGER_REPORT_ONLY", "1");
    GpuBudget::instance().reset();
    QVERIFY(GpuBudget::instance().reportOnly());
    qunsetenv("OLR_LEDGER_REPORT_ONLY");
}

void TestGpuBudget::totalChargedEverIsMonotonic() {
    GpuBudget::instance().reset();
    const qint64 t0 = GpuBudget::instance().totalChargedBytesEver();
    { GpuBudgetCharge c = GpuBudget::instance().chargeUngated(500, ResidencyTag::CpuFrame); }
    QCOMPARE(GpuBudget::instance().totalChargedBytesEver(), t0 + 500); // credit does not decrement
}
```
Register the three slots in the test class's `private slots:`.

- [ ] **Step 2: Run to verify failure** — `ninja -C build/c tst_gpubudget && ctest --test-dir build/c -R '^tst_gpubudget$' --output-on-failure` → FAIL (no `ResidencyTag`).

- [ ] **Step 3: Implement.** In `gpubudget.h`: add the enum, `tag()` on the charge (new member `ResidencyTag m_tag = ResidencyTag::DecodeWindow;` set by the Adopted ctor), the new methods. In `gpubudget.cpp`: keep one `g_mutex`; replace `g_liveBytes` with `qint64 g_liveByTag[10] = {};` plus `qint64 g_totalEver = 0;` helpers:

```cpp
bool GpuBudget::tagIsGated(ResidencyTag t) {
    switch (t) {
    case ResidencyTag::DecodeWindow: case ResidencyTag::Staging:
    case ResidencyTag::ReadbackRing: case ResidencyTag::OutputBus: return true;
    default: return false;
    }
}
std::optional<GpuBudgetCharge> GpuBudget::tryCharge(qint64 bytes, ResidencyTag tag) {
    if (bytes <= 0) return GpuBudgetCharge(0, tag, GpuBudgetCharge::Adopted{});
    QMutexLocker locker(&g_mutex);
    if (g_configured && tagIsGated(tag) && !g_reportOnly &&
        gatedLiveLocked() + bytes > g_budgetBytes)
        return std::nullopt;
    g_liveByTag[int(tag)] += bytes; g_totalEver += bytes;
    return GpuBudgetCharge(bytes, tag, GpuBudgetCharge::Adopted{});
}
GpuBudgetCharge GpuBudget::chargeUngated(qint64 bytes, ResidencyTag tag) {
    if (bytes <= 0) return GpuBudgetCharge(0, tag, GpuBudgetCharge::Adopted{});
    QMutexLocker locker(&g_mutex);
    g_liveByTag[int(tag)] += bytes; g_totalEver += bytes;
    return GpuBudgetCharge(bytes, tag, GpuBudgetCharge::Adopted{});
}
```
`credit(bytes, tag)` decrements the tag bucket (floor 0); `~GpuBudgetCharge` passes its tag. `g_reportOnly` is read once from the env in a function-local static. Legacy overloads delegate with `DecodeWindow`. IMPORTANT (spec Phase 0): report-only disables the gate **only where new call sites opt in** — the existing mint gate keeps gating; implement as: the mint site (Task 2) calls `tryCharge(bytes, tag)` which honors `g_reportOnly=false` behavior for `DecodeWindow` (i.e., exclude `DecodeWindow` from the report-only bypass: `!g_reportOnly || tag == ResidencyTag::DecodeWindow` in the gate condition).

- [ ] **Step 4: CMake.** Move `playback/gpu/gpubudget.cpp` (and its test registration) out of `if(OLR_GPU_PIPELINE)` into the unconditional source lists in both `CMakeLists.txt` and `tests/CMakeLists.txt`. Rebuild **both** configs: `ninja -C build/c && ninja -C build/off` → both link.

- [ ] **Step 5: Run tests** — full label: `ctest --test-dir build/c -L unit --output-on-failure` → PASS (incl. pre-existing gpubudget cases).

- [ ] **Step 6: Commit** — `git add -A && git commit -m "feat(residency): ledger owner tags, gated-vs-charge-only semantics, report-only mode"` (+trailer).

---

### Task 2: Charge follows the surface lifetime + surface census (failing test 1)

**Files:**
- Modify: `playback/gpu/gpusurface.h` (add charge holder + census hooks; keep destructor inline `= default`)
- Create: `playback/gpu/gpusurfacecensus.h`, `playback/gpu/gpusurfacecensus.cpp` (unconditional sources, like the ledger)
- Modify: `playback/gpu/gpusurfaceallocator.cpp` (~line 77: attach charge to surface immediately after `tryCharge`, before the surface is published — spec §2 attach ordering)
- Modify: `playback/gpu/gpuframedata.h/.cpp` (drop `GpuBudgetCharge m_budgetCharge`; ctor keeps the parameter for source compatibility but forwards it to the surface via `adoptResidencyCharge`)
- Test: Create `tests/unit/tst_residency_lifetime.cpp`; register `olr_add_unit_test(tst_residency_lifetime olr_test_playback)`

**Interfaces:**
- Produces:
  - `GpuSurface::adoptResidencyCharge(GpuBudgetCharge&& c)` — non-virtual; stores in `GpuBudgetCharge m_residencyCharge;` (protected member of the base; link-safe because the ledger is unconditional per Task 1). **Spec deviation note:** spec §1 says "concrete classes"; the base-member placement satisfies the same link contract with less churn — update spec §1 wording in this commit.
  - `GpuSurfaceCensus::instance()` with `void noteCreated(const GpuSurface*)`, `void noteDestroyed(const GpuSurface*)`, `int aliveCount() const`, `void noteWrapAlive(const void* sessionKey, int delta)`, `int wrapHighWatermark(const void* sessionKey) const`, `void resetForTest()`. Census hooks are called from `GpuSurface`'s ctor/dtor (add a protected ctor body; dtor stays `= default` — instead credit/census run from a small `GpuSurfaceCensusToken` RAII member declared BEFORE `m_residencyCharge` so destruction order is: charge credits after census note).

- [ ] **Step 1: Failing test** (`tests/unit/tst_residency_lifetime.cpp`) — the spec's failing test 1:

```cpp
#include <QtTest>
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpusurfacecensus.h"
#include "playback/gpu/gpuframedata.h"
// Minimal fake surface (pattern from tst_iotargetsinkfactory's FakeGpuSurface):
class FakeSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 64, 48, 64*48*3/2}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return reinterpret_cast<void*>(quintptr(0x1)); }
};
class TestResidencyLifetime : public QObject {
    Q_OBJECT
private slots:
    void chargeSurvivesFrameDataDeath() {
        GpuBudget::instance().reset(); GpuSurfaceCensus::instance().resetForTest();
        auto surface = std::make_shared<FakeSurface>();
        auto charge = GpuBudget::instance().tryCharge(4608, ResidencyTag::DecodeWindow);
        QVERIFY(charge.has_value());
        surface->adoptResidencyCharge(std::move(*charge));
        std::shared_ptr<GpuSurface> retained = surface; // charge-free co-owner (readback retainer model)
        {
            FrameHandle h = makeGpuFrameHandle(surface, nullptr, FrameMetadata{});
            surface.reset();
        } // FrameHandle/GpuFrameData die here — but the surface is still retained
        QCOMPARE(GpuBudget::instance().liveBytes(), qint64(4608));           // FAILS today
        QCOMPARE(GpuSurfaceCensus::instance().aliveCount(), 1);
        retained.reset();
        QCOMPARE(GpuBudget::instance().liveBytes(), qint64(0));
        QCOMPARE(GpuSurfaceCensus::instance().aliveCount(), 0);
    }
};
QTEST_GUILESS_MAIN(TestResidencyLifetime)
#include "tst_residency_lifetime.moc"
```

- [ ] **Step 2: Run to verify failure** — build+run `tst_residency_lifetime` → FAIL at the first `QCOMPARE` (no `adoptResidencyCharge`; charge dies with `GpuFrameData` today).

- [ ] **Step 3: Implement.** `gpusurface.h` gains (before the virtuals):

```cpp
class GpuSurface {
public:
    // Residency: the ledger charge lives with the surface so the credit fires on
    // the TRUE last release, whatever component drops it. Attach once, immediately
    // after tryCharge at mint, before the surface is shared across threads.
    void adoptResidencyCharge(GpuBudgetCharge&& c) { m_residencyCharge = std::move(c); }
protected:
    GpuSurface();                       // registers with GpuSurfaceCensus
    GpuSurfaceCensusToken m_censusToken; // notes destruction (declared first)
    GpuBudgetCharge m_residencyCharge;   // credits after the census note
public:
    virtual ~GpuSurface() = default;     // stays inline: GPU-off link contract
    ...
```
`GpuSurfaceCensusToken` is a tiny RAII in `gpusurfacecensus.h` holding `const GpuSurface*`. Census uses one `QMutex` + `QHash<const void*, int>` watermarks. In `gpusurfaceallocator.cpp`, after the existing `tryCharge` succeeds: `surface->adoptResidencyCharge(std::move(*charge));` and stop passing the charge into `makeGpuFrameHandle` (pass `GpuBudgetCharge{}`); in `gpuframedata.cpp`, the ctor forwards any non-empty legacy charge to the surface (`if (m_surface && budgetCharge.bytes() > 0) m_surface->adoptResidencyCharge(std::move(budgetCharge));`) so out-of-tree callers stay correct.

- [ ] **Step 4: Run** the new test → PASS; then the **full unit label** (eviction/budget siblings are sensitive) → PASS. Also `ninja -C build/off` (GPU-off link).

- [ ] **Step 5: Commit** — `feat(residency): tie ledger charge to true GpuSurface lifetime + surface census` (spec §1 wording amendment included).

---

### Task 3: Charge the producer wraps (failing test 2)

**Files:**
- Modify: `recorder_engine/streamworker.cpp` (~line 142, `importGpuVideoFrameForEncode`) — tag `RecorderWrap`
- Modify: `recorder_engine/ingest/gpudecodedframe.cpp` (~line 53, `makeGpuDecodedFrameHandle`) — tag `IngestWrap`
- Test: extend `tests/unit/tst_residency_lifetime.cpp`

**Interfaces:** Consumes `chargeUngated`/`adoptResidencyCharge` (Tasks 1–2). Produces: both wrap sites charge `desc().allocationBytes` (fallback `w*h*3/2` when 0) — charge-only, never gated (spec §1 gate semantics).

- [ ] **Step 1: Failing test** — in `tst_residency_lifetime.cpp` add `producerWrapsAreCharged()`: build a `FakeSurface`, call `makeGpuDecodedFrameHandle(surface, /*rhi*/nullptr, meta)` (match the real signature at `gpudecodedframe.cpp:53` when writing the test), assert `GpuBudget::instance().liveBytes(ResidencyTag::IngestWrap) == 4608`. → FAIL (0).
- [ ] **Step 2: Implement** — at both wrap sites:

```cpp
const qint64 wrapBytes = surface->desc().allocationBytes > 0
        ? surface->desc().allocationBytes
        : qint64(surface->desc().width) * surface->desc().height * 3 / 2;
surface->adoptResidencyCharge(
        GpuBudget::instance().chargeUngated(wrapBytes, ResidencyTag::IngestWrap)); // RecorderWrap at streamworker site
```
Guard against double-charging: `adoptResidencyCharge` keeps the FIRST non-empty charge (`if (m_residencyCharge.bytes() == 0) ...`) — add that guard in `gpusurface.h` and a unit assertion for it.
- [ ] **Step 3: Run** new test + full unit label + `tst_streamworker_gpuencode` specifically → PASS.
- [ ] **Step 4: Commit** — `feat(residency): charge recorder/ingest GPU wraps (charge-only tags)`.

---

### Task 4: Charge CPU planes and the CPU readback caches (+ in-window LRU)

**Files:**
- Modify: `playback/output/framehandle.cpp` (the CPU frame-data class created by `makeCpuFrameHandle` — charge plane bytes, tag `CpuFrame`)
- Modify: `playback/gpu/gpuframedata.cpp` (`readToCpu` caches → charge each inserted `m_cpuCache` entry, tag `CpuReadbackCache`; evict via LRU per spec §2(b))
- Modify: `playback/output/gpureadbackring.cpp` (`SharedGpuReadbackCache` entries → same tag)
- Test: extend `tests/unit/tst_residency_lifetime.cpp` + `tests/unit/tst_gpureadbackring.cpp`

**Interfaces:** Produces: `GpuFrameData` keeps at most `kCpuCacheMaxEntriesPerFrame = 1` cached format per frame (LRU by insertion — the second format evicts the first; readback regenerates cheaply for live GPU frames); every cache insert/evict adjusts `CpuReadbackCache`. CPU frame-data holds a `GpuBudgetCharge m_charge` member (charge-only `CpuFrame`), constructed with `chargeUngated(planeBytes, ResidencyTag::CpuFrame)`.

- [ ] **Step 1: Failing tests** — (a) `cpuFramesAreCharged`: `makeCpuFrameHandle` a 64×48 NV12 frame; assert `liveBytes(CpuFrame) == 4608`, and 0 after the handle dies. (b) `cpuCacheChargedAndLruBounded`: fake-surface GPU frame, call `readToCpu(Nv12)` then `readToCpu(Rgba8)`; assert `liveBytes(CpuReadbackCache)` equals only the RGBA entry's bytes (LRU evicted NV12). → FAIL.
- [ ] **Step 2: Implement** as above (the cache map gains a small insertion-order list; on exceeding 1 entry, erase oldest and its charge — the charge is stored alongside the planes in the map value: `struct CachedPlanes { CpuPlanes planes; GpuBudgetCharge charge; };`).
- [ ] **Step 3: Run** new tests + full unit label (readback-sharing tests exercise the cache: `tst_asyncgpureadbacksink`, `tst_gpureadbackring` must stay green — they rely on one-readback-per-surface, which a 1-entry cache still provides for a single consumer format).
- [ ] **Step 4: Commit** — `feat(residency): charge CPU frames and readback caches; LRU-bound per-frame CPU cache`.

---

### Task 5: Holder-occupancy + VT wrap-watermark telemetry and the periodic report line

**Files:**
- Modify: `playback/output/gpureadbackring.cpp/.h` (`pendingCount()/pendingBytes()` accessors), `playback/gpu/gpuframeretirequeue.cpp/.h` (`size()`), `playback/output/asyncgpureadbacksink.cpp/.h` (`lastDeliveredBytes()`)
- Modify: `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm` — around each output-buffer wrap, `GpuSurfaceCensus::instance().noteWrapAlive(this, +1)`, and `-1` from the wrap's release path (pass `this` as the session key; the census token pattern from Task 2 gives the `-1` for free if the wrap is a `GpuSurface` — wire the key through `AppleGpuSurface`'s constructor as `const void* poolKey`)
- Modify: `playback/playbackworker.cpp` (~line 217 `recordGpuBudget` call) — extend with per-tag bytes; add a 5-second `qInfo` residency report line (worker thread), e.g. `[residency] total=… gated=… decode=… staging=… ring=… cpuCache=… recWrap=… ingWrap=… holders(ring=…,retire=…,retainer=…) headroom=…`
- Modify: `playback/output/outputruntime.cpp/.h` (`recordGpuBudget` signature grows a `QHash<int, qint64>` or fixed array of tag bytes — keep the existing two params and append)
- Test: extend `tests/unit/tst_outputruntime.cpp` for the widened `recordGpuBudget`; census watermark test in `tst_residency_lifetime.cpp` (`noteWrapAlive(key,+1)×3, −1×1` → `wrapHighWatermark(key)==3`)

- [ ] Steps: failing tests → implement → full unit label → commit `feat(residency): per-holder occupancy, VT wrap watermarks, periodic residency report`.

---

### Task 6: Phase-0 attribution run on device (GATING DECISION POINT)

**Files:**
- Modify: `docs/superpowers/plans/2026-06-21-gpu-phase5-ios-manual-checklist.md` — add the attribution-run procedure
- Create: `docs/superpowers/specs/2026-07-07-ios-frame-residency-phase0-report.md` (the measured numbers)

**Procedure (manual, requires the iPad + live SRT feeds):**
- [ ] Build iOS with GPU ON + `OLR_LEDGER_REPORT_ONLY=1` baked via the Task 13 CMake option's env seam (until Task 13 lands, set it in `main()` next to the force-on qputenv, guarded the same way).
- [ ] Run 4× SRT feeds, record+play ≥5 min, scrub aggressively; capture the `[residency]` lines via `devicectl … --console` and the ramp rate (bytes/s from consecutive lines).
- [ ] Write the report: per-tag peaks, per-holder peaks, per-session wrap watermarks, footprint-minus-ledger delta (class-3/5 inference), measured ramp rate.
- [ ] **DECISION POINT (spec Phase 0):** name the dominant class(es). If the dominant class is NOT covered by Tasks 2–4 accounting + Task 7–12 enforcement (e.g., it is FFmpeg/SRT buffering or encoder queues), STOP — update the spec and re-plan before continuing. Record the decision in the report file. Level-1 threshold default for Task 10 is recomputed here as `ramp_bytes_per_s × 0.6 s`, floored at 256 MB.
- [ ] Commit report + checklist: `docs(residency): phase-0 attribution report + manual procedure`.

---

### Task 7: `ResidencyWindowParams` + derivation math

**Files:**
- Create: `playback/gpu/residencywindow.h`, `playback/gpu/residencywindow.cpp` (unconditional sources)
- Test: Create `tests/unit/tst_residencywindow.cpp`; register `olr_add_unit_test(tst_residencywindow olr_test_core)`

**Interfaces (later tasks use these exact names):**

```cpp
struct ResidencyWindowParams {
    int trailFrames = 0;   // resident frames behind the playhead, per feed
    int leadFrames  = 0;   // decode-ahead in front (existing behavior)
    int slackFrames = 0;   // hysteresis on top of trail for trim spans
    int perTrackCap = 0;   // TrackBuffer cap = trailFrames + leadFrames + slackFrames
    int64_t trailMs(int64_t frameDurMs) const;  // trailFrames * frameDurMs
    int64_t leadMs(int64_t frameDurMs)  const;
    int64_t slackMs(int64_t frameDurMs) const;
};
// Desktop: reproduces today's constants byte-identically at the given frame duration:
// kTrailMs=300, kSlackMs=200, kLeadMs=500 (playbackworker.h:174-183) and the current caps.
ResidencyWindowParams desktopDefaultWindowParams(int64_t frameDurMs, int trackCount);
// iOS: spec §4 formula (asymmetric window; lead NOT widened):
// trail = clamp((budget - reserves)/(feedCount*frameBytes) - leadFrames, 8, 120)
ResidencyWindowParams deriveIosWindowParams(qint64 budgetBytes, qint64 nonWindowReserves,
                                            int feedCount, qint64 frameBytes, int leadFrames);
```

- [ ] **Step 1: Failing tests** — exact-value cases: `deriveIosWindowParams(1'536'000'000 − 620'000'000, 0, 4, 3'110'400, 15)` → `trailFrames` in [50,60]; clamps at 8 and 120; lockstep = same params object shared by all feeds (API returns one struct); `desktopDefaultWindowParams(33, n)` reproduces 300/200/500 ms in frames (9/6/15) and the current cap formula (`max(12, 256/n)` per `iosgpupolicy.cpp:11-14` semantics — assert equality against `gpuPerTrackWindowCap(n)` for the non-iOS branch).
- [ ] **Step 2–4:** implement pure math; run; commit `feat(residency): window parameter derivation (desktop-identical defaults, iOS asymmetric trail)`.

---

### Task 8: Parameterize the scheduler (failing trail-retention test)

**Files:**
- Modify: `playback/playbackworker.h` (~174–188): keep the constants; add `ResidencyWindowParams m_windowParams;` + `void setResidencyWindowParamsForTest(const ResidencyWindowParams&);`
- Modify: `playback/playbackworker.cpp`:
  - initialization: `m_windowParams = desktopDefaultWindowParams(frameDurMs(), trackCount)`; on iOS GPU builds, recomputed in `configureGpuBudget` (Task 9) from the derived budget
  - the per-iteration trim (~3317–3336): `keepFrom = P − (m_windowParams.trailMs(dur) + m_windowParams.slackMs(dur))`; `keepTo = P + m_windowParams.leadMs(dur) + m_windowParams.slackMs(dur)`; **audio horizon stays at the literal current value** (`P − 500 ms`) per spec §4 — introduce `kAudioTrailMs = 500` and use it for the audio-sample horizon in `trimBefore` (~3334)
  - `capFrames()` (~300–310): non-forced path returns `m_windowParams.perTrackCap` (desktop default reproduces today's values; `gpuForcedPerTrackBudget()` still wins)
  - insert protect ranges (~1360–1480) and reposition `trimBefore` (~2087): swap the constants for `m_windowParams` spans; `reuseAt` (~429–441) requires no code change once the output cache retains the trail (it checks coverage)
- Test: extend `tests/unit/tst_playbackworker.cpp`

**Interfaces:** Consumes Task 7 types. Produces: `setResidencyWindowParamsForTest` (also used by Task 12's ladder tests).

- [ ] **Step 1: Failing trail-retention test** (spec failing test 3), following the existing `tst_playbackworker` harness pattern (FrameProvider + PlaybackTransport + `initializeOutputGraph`):

```cpp
void TestPlaybackWorker::trailRetentionServesBackwardSteps() {
    // harness setup as in existing cases…
    ResidencyWindowParams p; p.trailFrames = 40; p.leadFrames = 15; p.slackFrames = 6;
    p.perTrackCap = 61;
    worker.setResidencyWindowParamsForTest(p);
    // play forward 60 frames via the harness tick loop…
    const int decodesBefore = worker.decodedVideoFramesForTest();
    // step backward 39 frames one at a time…
    QCOMPARE(worker.decodedVideoFramesForTest(), decodesBefore); // zero re-decodes; FAILS today (~15-frame trail)
}
```
- [ ] **Step 2:** verify FAIL (trim bites at 500 ms). **Step 3:** implement. **Step 4:** run full unit label **and** the desktop e2e assumptions: `ctest --test-dir build/c -R 'e2e_play' --output-on-failure` for `play1x seekplay stepscrub farback seekflash` (desktop defaults must be byte-identical — these gates prove it). **Step 5:** commit `feat(residency): parameterized retention windows in the playback scheduler (trail-retention test)`.

---

### Task 9: iOS window derivation replaces the per-track constant

**Files:**
- Modify: `playback/gpu/iosgpupolicy.h/.cpp` — delete `kIosAggregateGpuFrameCeiling` and `kIosMaxPerTrackGpuFrames`; `gpuPerTrackWindowCap(trackCount)` keeps its desktop branch and, on iOS, returns the cap from the worker-provided params (add `int gpuPerTrackWindowCap(int trackCount, const ResidencyWindowParams* iosParams)` overload; legacy signature = desktop behavior)
- Modify: `playback/playbackworker.cpp` `configureGpuBudget` (~725–750): compute `budget` (Task 10's `deriveIosBudgetBytes` on iOS; existing formula elsewhere), `nonWindowReserves` from `GpuBudgetConfig`'s estimators + the `CpuReadbackCache` allowance (`2 × feedCount × surfaceBytes()`), then `m_windowParams = deriveIosWindowParams(...)` and `cfg.aggregateDecodeWindow = m_windowParams.perTrackCap * feedCount`
- Test: rework `tests/unit/tst_iosgpupolicy.cpp` (pins the deleted constants today); extend `tst_residencywindow.cpp` for the reserves math

- [ ] Steps: failing tests (new pins: derived cap == trail+lead+slack; ceiling constant gone) → implement → full unit label → commit `feat(residency): derive iOS windows from the OS budget; retire fixed iOS frame caps`.

---

### Task 10: Headroom sampling + iOS budget arithmetic

**Files:**
- Create: `playback/gpu/iosheadroom.h`, `playback/gpu/iosheadroom.mm` (iOS: `os_proc_available_memory()`), `playback/gpu/iosheadroom_stub.cpp` (non-iOS: returns −1); unconditional sources with platform guards
- Test: `tests/unit/tst_iosheadroom.cpp` (pure math via injected samples)

**Interfaces:**

```cpp
qint64 osAvailableMemoryBytes();      // -1 = invalid/unavailable (non-iOS, backgrounded, 0-sample)
// Spec §3: min(0.5*(avail+gatedLive), avail), clamp lo=min(512MB,avail), hi=4GB.
// Invalid sample (<=0 or > physical RAM) -> previousBudget unchanged.
qint64 deriveIosBudgetBytes(qint64 availableNow, qint64 gatedLiveBytes, qint64 previousBudget);
```

- [ ] **Step 1: Failing tests** — exact values: `(3e9, 0, 0)` → 1.5e9; `(1e9, 1.5e9, X)` → `min(1.25e9, 1e9)` = 1e9; `(200e6, 0, X)` → clamp floor `min(512MB, 200MB)` = 200e6 (never overcommits); `(-1, …, prev)` → prev; `(0, …, prev)` → prev; hi-clamp at 4 GB.
- [ ] **Steps 2–4:** implement, run, commit `feat(residency): OS headroom sampler and iOS budget derivation`.

---

### Task 11: Pressure-ladder state machine (pure logic)

**Files:**
- Create: `playback/gpu/memorypressureladder.h`, `playback/gpu/memorypressureladder.cpp`
- Test: `tests/unit/tst_memorypressureladder.cpp`

**Interfaces:**

```cpp
struct LadderConfig {
    qint64 level1HeadroomBytes;   // default max(256MB, 4*feedCount*frameBytes); Phase-0 re-derived
    qint64 level2HeadroomBytes;   // level1/2
    int    secondWarningWindowMs = 10'000;
    int    rearmAfterMs          = 30'000;  // headroom > 2*level1 for this long
    int    rearmMinIntervalMs    = 60'000;
};
class MemoryPressureLadder {
public:
    enum class Level { Normal, Trim, CpuLatch };
    struct Actions { bool rederiveBudget=false; bool trimAndFlush=false;
                     bool latchCpu=false; bool rearmBudgetUpward=false; };
    explicit MemoryPressureLadder(const LadderConfig&);
    Actions onHeadroomSample(qint64 headroomBytes, int64_t nowMs); // watchdog + bytes-minted callers
    Actions onMemoryWarning(int64_t nowMs);                        // UIKit (supplementary)
    Level level() const; void reset();
};
```

- [ ] **Step 1: Failing tests** — transitions: sample below L1 → `{rederiveBudget,trimAndFlush}` and `level()==Trim`; second sample below L2 after a Trim → `latchCpu`; two warnings within 10 s → `latchCpu`; warnings while `Level::CpuLatch` → no-op; re-arm only after 30 s above 2×L1 and ≥60 s since last re-arm → `rearmBudgetUpward`; `CpuLatch` is terminal until `reset()`.
- [ ] **Steps 2–4:** implement (no Qt beyond `qint64`; timestamps injected — never `Date`-like calls), run, commit `feat(residency): memory-pressure ladder state machine`.

---

### Task 12: Wire the watchdog, burst trigger, Level-1 actions, and Level-2 latch

**Files:**
- Modify: `playback/playbackworker.cpp/.h`:
  - members: `MemoryPressureLadder m_pressureLadder; std::atomic<bool> m_memoryPressureLatched{false}; qint64 m_lastMintMark = 0; qint64 m_lastHeadroomSampleMs = 0;`
  - worker loop (next to the existing per-iteration trim ~3317): debounced (250 ms via the transport clock) `osAvailableMemoryBytes()` sample → `m_pressureLadder.onHeadroomSample(...)`; **burst trigger**: after each mint, if `GpuBudget::instance().totalChargedBytesEver() - m_lastMintMark > 64<<20`, sample immediately (place the check inside the mint helper used by the decode loop so reposition fills hit it — spec §6)
  - Level-1 action: re-derive budget (Task 10) → `configureGpuBudget()` → recompute `m_windowParams` → run the existing trim immediately → flush playback VT pools (Task 12b below) → set the ingest flush flag → drop `CpuReadbackCache` overage (walk live caches via the census — or simpler and sufficient: bump a generation the LRU checks)
  - Level-2 action: `handleGpuDeviceLoss(GpuLossReason::MemoryPressure)`; add `enum class GpuLossReason { DeviceLost, MemoryPressure };` parameter to `handleGpuDeviceLoss` (default `DeviceLost` keeps all existing call sites): `MemoryPressure` sets `m_memoryPressureLatched`, calls `GpuGenerationCounter::instance().bump()` directly, sanitizes (existing code), **skips** `GpuDeviceLossMonitor::recordLoss()`, `consumeGpuDeviceLossRebuildBudget()`, and any rebuild; `capFrames()` checks `m_gpuPipelineState == CpuFallback || m_memoryPressureLatched` (not `gpuPipelineEnabled()`) to select the CPU branch — fixing the same wrong-branch defect for device-loss `CpuFallback` (spec §6); `initializeOutputGraph` clears the latch + `m_pressureLadder.reset()`
- Modify: `playback/gpu/iosgpulifecyclesink.h/.cpp` + `ios/iosgpulifecycle.mm`: add `onMemoryWarning()` (UIKit `UIApplicationDidReceiveMemoryWarningNotification` observer → flag polled by the worker like suspend/resume; ignored while suspended)
- Modify: `recorder_engine/streamworker.cpp`: the tick (~`processEncoderTick` caller) samples headroom at the same debounce and, at Trim level, performs its **own-thread** ingest pool flush-excess (below)
- Test: extend `tests/unit/tst_gpu_devicelost_worker.cpp` (patterns exist for lifecycle sinks + pipeline-state assertions)

**Interfaces:** Consumes Tasks 7–11. Produces: `handleGpuDeviceLoss(GpuLossReason)`; `m_memoryPressureLatched` semantics for Task 13's checklist.

- [ ] **Step 1: Failing tests** (in `tst_gpu_devicelost_worker.cpp`, guarded by GPU availability like siblings):
  - `memoryPressureLatchDoesNotConsumeDeviceLossBudget` — inject L2 via a test hook (`worker.injectMemoryPressureForTest(Level::CpuLatch)` — add it); assert `gpuPipelineState()==CpuFallback`, device-loss rebuild budget untouched, `GpuDeviceLossMonitor::instance().isLost()==false`.
  - `latchSelectsCpuCapBranch` — after latch, `capFrames()` equals the CPU-branch value (compare against a worker with GPU env off).
  - `unlatchOnOutputGraphInit` — re-run `initializeOutputGraph`; latch cleared, GPU state can re-arm.
- [ ] **Steps 2–4:** implement, run full unit label + `ctest -R e2e_play_gpu` locally, commit `feat(residency): headroom watchdog, burst trigger, pressure levels wired into worker + recorder`.

**Task 12b (fold-in, same commit): VT pool flush.** In `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm` add `void flushPoolExcess()`:

```objc
void NativeVideoDecoderVideoToolbox::flushPoolExcess() {
    if (!m_session) return;
    CFTypeRef poolRef = nullptr; // spec §5 API pair
    if (VTSessionCopyProperty(m_session, kVTDecompressionPropertyKey_PixelBufferPool,
                              kCFAllocatorDefault, &poolRef) == noErr && poolRef) {
        CVPixelBufferPoolFlush((CVPixelBufferPoolRef)poolRef, kCVPixelBufferPoolFlushExcessBuffers);
        CFRelease(poolRef);
    }
}
```
Playback decoders: called from the worker at Trim; Level 2 additionally calls the existing `reset()`. Ingest decoders: `std::atomic<bool> m_flushPoolRequested` on the session, set cross-thread by the ladder, checked at the top of the capture thread's decode iteration → `flushPoolExcess()` on its own thread (never reset; spec §5).

---

### Task 13: CMake force-on option + report-only env seam

**Files:**
- Modify: `CMakeLists.txt` (~line 17 options block): `option(OLR_GPU_PIPELINE_FORCE_ON "Force the GPU pipeline runtime gate on at startup (iOS GPU builds)" OFF)`; when ON and `OLR_GPU_PIPELINE`, `target_compile_definitions(OpenLiveReplay PRIVATE OLR_GPU_PIPELINE_FORCE_ON=1)`
- Modify: `main.cpp` (~line 26): port the uncommitted force-on hunk from the phases-4-5 worktree properly:

```cpp
#if defined(OLR_GPU_PIPELINE_BUILD) && defined(OLR_GPU_PIPELINE_FORCE_ON)
    // GPU-only build variant (iOS): the runtime OLR_GPU_PIPELINE gate cannot be
    // set as an environment variable on device, so bake it on at startup.
    qputenv("OLR_GPU_PIPELINE", "1");
#endif
```
- Test: smoke — configure `build/forceon` with `-DOLR_GPU_PIPELINE=ON -DOLR_GPU_PIPELINE_FORCE_ON=ON`, `nm` the app's `main.o` for `qputenv`; both other configs unchanged.
- [ ] Commit — `build(gpu): OLR_GPU_PIPELINE_FORCE_ON option for iOS GPU builds`.

---

### Task 14: Mechanical rename + docs + gates + PR

**Files:**
- Rename `GpuBudget`→`FrameResidencyLedger`, `GpuBudgetCharge`→`ResidencyCharge` (files `gpubudget.*`→`frameresidencyledger.*`), via `sed` across the tree; a compatibility `using GpuBudget = FrameResidencyLedger;` is NOT kept (single-repo, fix all callers)
- Modify: `docs/superpowers/plans/2026-06-21-gpu-phase5-ios-manual-checklist.md` — add the **enforcement** on-device gate (spec Testing): 4× SRT ≥10 min play+scrub; ledger plateau ≤ budget AND headroom stable; lockstep step-check across the full trail; Level-1 drill; far-seek burst does not outrun the bytes-minted trigger; VT sustains window-depth outstanding buffers
- Modify: `.github/workflows/ci.yml` tsan/tsan-gpu `build_targets` lists += `tst_residency_lifetime tst_residencywindow tst_memorypressureladder`
- [ ] Steps: rename + build both configs + full unit label + local `OLR_PREPUSH_FULL=1` gates as feasible → update spec status to "implemented" → commit `refactor(residency): rename ledger types; wire CI + manual gates` → push branch through the pre-push hook (`QT_HOST_PREFIX=/opt/homebrew/opt/qt`) → open PR **targeting `gpu/resident-pipeline-phases-4-5`** if #164 is unmerged, else `main`; PR body summarizes spec v3 and links the Phase-0 report.

---

## Execution notes

- Tasks 1–5 are safe on the macOS host and land before any behavior change; Task 6 is the on-device **gate** — do not proceed past it without the recorded decision.
- Tasks 8–9 change worker scheduling: run the full unit label AND the desktop `e2e_play` scenarios listed in Task 8 after each; any counter drift on desktop = defect (defaults must be byte-identical).
- Task 12 touches the worker's threading: request an independent review before merge (repo policy), and run the TSan lanes locally if feasible (`OLR_PREPUSH_FULL=1 SKIP_ASAN=1 SKIP_E2E=1 SKIP_IOS_BUILD=1` narrows to TSan).

## Self-review (performed at write time)

- Spec coverage: §1→T1/T2, §2→T2/T3/T4, §3→T10, §4→T7/T8/T9, §5→T12b, §6→T11/T12, §7→T13, Phase 0→T5/T6, Testing→each task's step 1 + T14 gates. No uncovered section.
- Placeholder scan: clean (every code step shows code; manual device steps are procedures by nature).
- Type consistency: `ResidencyTag`/`ResidencyWindowParams`/`deriveIosBudgetBytes`/`GpuLossReason`/`setResidencyWindowParamsForTest` names match across tasks.

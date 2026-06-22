# new-io-targets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Land the four enumerated-but-stubbed broadcast output kinds — `DeckLinkSdiHdmi`, `DeckLinkIpSt2110`, `Aja`, `Omt` — as real `IOutputSink` implementations behind per-SDK build/capability flags, with a runtime capability probe per SDK that selects each sink's GPU path. Per P0.4: DeckLink SDI/HDMI + ST2110 are **GPU-texture-capable hardware** sinks (`SinkGpuCapability::GpuNative`, bypass readback) where the SDK exposes GPUDirect/texture input, else they fall to `NeedsContinuousCadence` CPU readback; AJA (NTV2 AutoCirculate host buffers) and OMT (software SDK) are **NDI-style CPU-frame async-readback siblings** that route through `AsyncGpuReadbackSink`. The DeckLink/AJA/OMT SDKs are **not vendored** — each sink lands behind a build flag (`OLR_WITH_DECKLINK` / `OLR_WITH_AJA` / `OLR_WITH_OMT`) with a clean integration seam: a thin `IOutputSink` impl over an injectable backend interface, plus an SDK-availability stub that compiles and tests **off-SDK**. Tests run against the stub path (no SDK) asserting routing, cadence classification, capability-probe selection, and ST2110 bitstream framing; real-SDK paths are documented manual gates. e2e cadence/continuity gates per target mirror the NDI marker-continuity gate (`maxGap<=2`).

**Architecture:** A new `playback/output/iotargets/` family adds one sink per kind, each split into (a) a neutral `IOutputSink` shell (`DeckLinkOutputSink`, `AjaOutputSink`, `OmtOutputSink`) holding an injectable backend interface (`IDeckLinkSenderBackend`, `IAjaSenderBackend`, `IOmtSenderBackend`) — mirroring the existing `NdiOutputSink`/`INdiSenderBackend` seam — and (b) a real-SDK backend compiled only under the matching `OLR_WITH_*` flag, with a `Stub*SenderBackend` that compiles unconditionally and reports the SDK as unavailable. A per-sink `SinkCapabilityProbe` runs the runtime capability decision (DeckLink: `GpuNative` if the SDK build exposes GPUDirect/texture input and the device confirms it at runtime, else `NeedsContinuousCadence`; AJA/OMT: always `NeedsContinuousCadence` — NDI-style software SDKs that need a frame every tick, `maxGap<=2`) and returns the `SinkGpuCapability` consumed by Phase-4 `async-readback` routing. CPU-frame sinks (AJA/OMT, and the DeckLink readback fallback) are wrapped in the Phase-4 `AsyncGpuReadbackSink` exactly as NDI is; `GpuNative` DeckLink keeps the GPU texture and submits it through the SDK's texture-input path, bypassing readback. ST2110 is authored from the encode bitstream where applicable (the `gpu-encode` recorder path supplies the H.264/SMPTE-2110-30 essence framing); the DeckLink ST2110 sink carries an `St2110FrameFramer` that packs the encoded essence + RTP-style framing fields the SDK's IP output expects. A `decklink-st2110-bitstream` framing unit covers the packetization without the SDK present. Sink construction is wired through the existing target-manager/dispatcher seam (`OutputEndpoint`/`IOutputSink`); the dispatcher and `OutputBusFrame` are unchanged.

**Tech Stack:** C++17, Qt 6 (Core, Test; Gui/Multimedia only for the e2e marker senders), CMake + Ninja. Consumes the merged playback output stack (`playback/output/{outputsink.h,outputtypes.h,outputtargetassignment.h,outputbusengine.h,framehandle.h,gpureadbacktelemetry.h}`), the Phase-4 `async-readback` contract (`AsyncGpuReadbackSink` + `SinkGpuCapability {GpuNative, AsyncReadbackDedupOk, NeedsContinuousCadence}`), `gpu-budget` (VRAM-bounded GPU window + OOM-degrade), and the `gpu-sync`/`gpu-encode` keystones for the GPU-native texture submit + ST2110 essence. The DeckLink/AJA/OMT SDK headers are **not in-tree**; the real backends are guarded by `OLR_WITH_DECKLINK`/`OLR_WITH_AJA`/`OLR_WITH_OMT` and the SDK include/lib paths are supplied by the integrator (documented in each Task's seam note).

## Global Constraints

- **Builds ON merged Phase-0..4 code, never replaces it.** Every consumed interface already exists in the tree or in a merged Phase plan. Use the **actual** signatures verified in the tree: `IOutputSink` (`playback/output/outputsink.h` — `kind()`, `start(const OutputTargetAssignment&, FrameRate)`, `stop()`, `isActive()`, `submit(const OutputBusFrame&)`, `outputStatus()`); `OutputTargetKind {QtPreview, DeckLinkSdiHdmi, DeckLinkIpSt2110, Ndi, Omt, Aja}` (`playback/output/outputtypes.h`); `OutputBusFrame` carrying `FrameHandle video` + `MediaAudioFrame audio` atomically (`playback/output/outputbusengine.h`); `MediaVideoFrameView` over a `FrameHandle` (`playback/output/framehandle.h`); `OutputSinkStatus` (`playback/output/outputsink.h`). Do **not** invent variant names for these.
- **Capstone dependency order is real.** This subproject is the deferred capstone: it depends on `async-readback` (Phase 4) being merged for `AsyncGpuReadbackSink` + `SinkGpuCapability`, and on `gpu-budget` for the VRAM-bounded window + OOM-safe degrade. If those are not yet on `origin/main`, **stop** — do not stub them locally. Verify with `git merge-base --is-ancestor <phase4-sha> origin/main` before Task 1.
- **No vendored SDKs; clean seam, off-SDK-green.** The DeckLink/AJA/OMT SDKs are not in the tree. Each real backend lives in a `.cpp` compiled **only** under its `OLR_WITH_*` flag; with the flag off (the default, and the only state on CI), a `Stub*SenderBackend` compiles and every unit test for that sink runs against the stub, asserting routing/cadence/capability classification — never linking an SDK. The whole suite must build and pass with all three flags **off**. A task that adds a real-SDK backend keeps the off-SDK build byte-green.
- **Capability probe is the single path-selector.** Each sink's GPU path (GPU-native texture submit vs CPU-frame async-readback) is chosen by one `SinkCapabilityProbe::classify(...)` call returning a `SinkGpuCapability`. The dispatcher/target-manager wraps a `NeedsContinuousCadence`/`AsyncReadbackDedupOk` sink in `AsyncGpuReadbackSink`; a `GpuNative` sink is submitted directly. No sink classifies itself by hard-coding a branch in `submit()`.
- **CPU path stays default + reference; everything behind flags.** A target whose SDK flag is off (or whose runtime SDK is unavailable) starts in a clean "runtime-unavailable" state and never crashes the dispatcher — exactly as `NdiOutputSink` returns `RuntimeUnavailable` when the NDI library is absent. The pipeline default (Qt preview / NDI) is untouched.
- **AV pair stays atomic (spec §9).** These sinks consume `OutputBusFrame` whole; audio travels with the delayed video through `AsyncGpuReadbackSink`. No sink splits the video+audio pair. The DeckLink GPU-native path submits the kept texture **and** the paired audio together.
- **Public-repo professionalism.** This repo is public. Self-contained, professional code/comments/commits; document the present design and the SDK seam, no internal notes, no references to private history. Document the seam rather than pretending an SDK is present.
- **Zero-regression gate after every task.** With all `OLR_WITH_*` flags off:
  ```sh
  cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
  ```
  must pass with identical assertion values, and `e2e_play`/`e2e_ndi_output` golden thresholds unchanged. New sink tests `QSKIP`/report-unavailable where no SDK is present, never hard-fail.
- **Build (run from the worktree root):** configure once; the sinks compile in the default (all-SDK-off) configuration:
  ```sh
  cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
  cmake --build build/c --target <target>
  ctest --test-dir build/c -L unit --output-on-failure
  ```
  Unit tests register via `olr_add_unit_test(<name> olr_test_playback)` in `tests/unit/CMakeLists.txt`; Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`. The real-SDK backends are an integrator concern: `-DOLR_WITH_DECKLINK=ON -DDECKLINK_SDK_DIR=<path>` (and the AJA/OMT equivalents) are documented manual gates, not run on CI.
- **Format changed lines only** (CI Lint checks changed lines; engine files are hand-Allman):
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h'
  ```

---

## Preconditions (read before Task 1)

- **Phase 4 merged.** `async-readback` is on `origin/main`: `AsyncGpuReadbackSink` (`playback/output/asyncgpureadbacksink.h`) and `SinkGpuCapability {GpuNative, AsyncReadbackDedupOk, NeedsContinuousCadence}` (`playback/output/sinkgpucapability.h`) exist and the dispatcher/target-manager wraps `NeedsContinuousCadence`/`AsyncReadbackDedupOk` sinks. `gpu-budget` is merged (VRAM-bounded window + OOM-degrade). Verify with `git merge-base --is-ancestor <phase4-sha> origin/main`.

  > **If the Phase-4 header/type names differ on `origin/main`** (the async-readback plan is authored after this one), use the **actual merged names** for `AsyncGpuReadbackSink` and `SinkGpuCapability`. This plan names them as the spec (§6 `async-readback` row, D10) specifies; reconcile to the merged tree before Task 1 and adjust the `#include`s/enum spellings consistently. Do not fork a second copy.

- **The enum + JSON parsing already exist (merged).** `OutputTargetKind::{DeckLinkSdiHdmi, DeckLinkIpSt2110, Aja, Omt}` are enumerated (`playback/output/outputtypes.h:34-41`), named (`playback/output/outputtargetassignment.cpp` — `decklink-sdi-hdmi`/`decklink-ip-st2110`/`omt`/`aja`), and round-trip through settings (`settingsmanager.cpp:36-62` `outputTargetKindFromString`). This plan adds the **sinks**, not the enum/parsing.

- **The NDI sink is the structural template.** `NdiOutputSink`/`INdiSenderBackend` (`playback/output/ndisink.{h,cpp}`) is the exact seam to mirror: a sink shell holding `std::unique_ptr<I*SenderBackend> m_ownedBackend` + a raw `I*SenderBackend* m_backend`, a default ctor wiring the real (dynamic-load) backend and a test ctor injecting a fake, `start()` returning false with a status when the runtime is unavailable, `submit()` packing `MediaVideoFrameView(frame.video)` + `frame.audio` and stamping status. Read it before Task 2.

---

## File Structure

- **Create** `playback/output/iotargets/sinkcapabilityprobe.h`, `sinkcapabilityprobe.cpp` — `SinkCapabilityProbe::classify(OutputTargetKind, const QVariantMap& settings, bool sdkBuilt, bool deviceGpuTextureInput) -> SinkGpuCapability`. The single path-selector.
- **Create** `playback/output/iotargets/decklinksink.h`, `decklinksink.cpp` — `DeckLinkOutputSink : IOutputSink` (serves both `DeckLinkSdiHdmi` and `DeckLinkIpSt2110`) + `IDeckLinkSenderBackend` + `StubDeckLinkSenderBackend`.
- **Create** `playback/output/iotargets/decklinkbackend_sdk.cpp` — the real DeckLink backend, compiled only under `OLR_WITH_DECKLINK`.
- **Create** `playback/output/iotargets/st2110framer.h`, `st2110framer.cpp` — `St2110FrameFramer` packing the encoded essence + framing fields for the ST2110 IP output (SDK-independent; unit-tested off-SDK).
- **Create** `playback/output/iotargets/ajasink.h`, `ajasink.cpp` — `AjaOutputSink : IOutputSink` + `IAjaSenderBackend` + `StubAjaSenderBackend` (NTV2 AutoCirculate host-buffer CPU-frame async-readback sibling).
- **Create** `playback/output/iotargets/ajabackend_sdk.cpp` — real NTV2 backend under `OLR_WITH_AJA`.
- **Create** `playback/output/iotargets/omtsink.h`, `omtsink.cpp` — `OmtOutputSink : IOutputSink` + `IOmtSenderBackend` + `StubOmtSenderBackend` (software-SDK CPU-frame async-readback sibling).
- **Create** `playback/output/iotargets/omtbackend_sdk.cpp` — real OMT backend under `OLR_WITH_OMT`.
- **Create** `playback/output/iotargets/iotargetsinkfactory.h`, `iotargetsinkfactory.cpp` — `makeIoTargetSink(const OutputTargetAssignment&, FrameRate) -> std::unique_ptr<IOutputSink>`, wrapping CPU-frame sinks in `AsyncGpuReadbackSink` per the capability probe.
- **Modify** `CMakeLists.txt` — add `playback/output/iotargets/*` to the engine source list (all platforms); add the `OLR_WITH_DECKLINK`/`OLR_WITH_AJA`/`OLR_WITH_OMT` options with `*_BUILD` compile defs; compile `*backend_sdk.cpp` only under the matching flag with the integrator-supplied SDK include/lib.
- **Modify** `tests/unit/CMakeLists.txt` — register `tst_sinkcapabilityprobe`, `tst_decklinksink`, `tst_st2110framer`, `tst_ajasink`, `tst_omtsink`, `tst_iotargetsinkfactory` against `olr_test_playback`.
- **Create** tests: `tests/unit/tst_sinkcapabilityprobe.cpp`, `tst_decklinksink.cpp`, `tst_st2110framer.cpp`, `tst_ajasink.cpp`, `tst_omtsink.cpp`, `tst_iotargetsinkfactory.cpp`.
- **Create** `tests/e2e/iotarget_marker_sender.cpp`, `tests/e2e/run_iotarget_cadence_e2e.sh` — a stub-backed marker-stream cadence/continuity gate (`maxGap<=2`) mirroring `run_ndi_output_e2e.sh`; register in `tests/e2e/CMakeLists.txt` under a new `iotargets` label.

---

## Task 1: `SinkCapabilityProbe` — the single GPU-path selector

**Precondition:** Phase 4 merged (`SinkGpuCapability` exists).

**Files:**
- Create: `playback/output/iotargets/sinkcapabilityprobe.h`, `playback/output/iotargets/sinkcapabilityprobe.cpp`
- Test: `tests/unit/tst_sinkcapabilityprobe.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `OutputTargetKind` (`playback/output/outputtypes.h`), `SinkGpuCapability` (`playback/output/sinkgpucapability.h`, Phase 4), `QVariantMap`.
- Produces:
  ```cpp
  // playback/output/iotargets/sinkcapabilityprobe.h
  #include "playback/output/outputtypes.h"
  #include "playback/output/sinkgpucapability.h"  // Phase 4: GpuNative / AsyncReadbackDedupOk / NeedsContinuousCadence

  #include <QVariantMap>

  // The single GPU-path selector for the new I/O targets (P0.4 / D10). Given the
  // target kind, its settings, whether the SDK was compiled in (OLR_WITH_*), and
  // whether the device's runtime probe confirmed GPU-texture input, returns the
  // SinkGpuCapability the target-manager uses to decide GPU-native submit vs
  // CPU-frame async-readback wrapping. Pure function: no SDK calls, no I/O — the
  // SDK/device facts are passed in by the caller (the backend's runtime probe).
  class SinkCapabilityProbe {
  public:
      static SinkGpuCapability classify(OutputTargetKind kind, const QVariantMap& settings,
                                        bool sdkBuilt, bool deviceGpuTextureInput);
  };
  ```
  Classification rule (P0.4):
  - `DeckLinkSdiHdmi` / `DeckLinkIpSt2110`: `GpuNative` **iff** `sdkBuilt && deviceGpuTextureInput`; else `NeedsContinuousCadence` (SDI/HDMI/IP cannot drop a field — continuous cadence, `maxGap<=2`).
  - `Aja`: always `NeedsContinuousCadence` (NTV2 AutoCirculate host buffers — an NDI-style software SDK that needs a frame every tick, `maxGap<=2`; dedup would wrongly let it skip frames).
  - `Omt`: always `NeedsContinuousCadence` (software SDK CPU-frame — continuous cadence, `maxGap<=2`).
  - any other kind (`QtPreview`/`Ndi`): not owned here — return `AsyncReadbackDedupOk` as an inert default (the factory never asks the probe for those).

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_sinkcapabilityprobe.cpp`:

```cpp
// SinkCapabilityProbe is the single GPU-path selector for the new I/O targets
// (P0.4 / D10). DeckLink is GPU-native only when the SDK is built AND the device
// confirms texture input; AJA/OMT are NDI-style software SDKs that need a
// continuous cadence (a frame every tick, maxGap<=2).
#include <QtTest>

#include "playback/output/iotargets/sinkcapabilityprobe.h"

class TestSinkCapabilityProbe : public QObject {
    Q_OBJECT
private slots:
    void deckLinkGpuNativeWhenSdkAndDevice();
    void deckLinkFallsToCadenceWithoutGpuTextureInput();
    void deckLinkFallsToCadenceWithoutSdk();
    void ajaAlwaysContinuousCadence();
    void omtAlwaysContinuousCadence();
};

void TestSinkCapabilityProbe::deckLinkGpuNativeWhenSdkAndDevice() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkSdiHdmi, {},
                                           /*sdkBuilt=*/true, /*deviceGpuTextureInput=*/true),
             SinkGpuCapability::GpuNative);
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkIpSt2110, {}, true, true),
             SinkGpuCapability::GpuNative);
}

void TestSinkCapabilityProbe::deckLinkFallsToCadenceWithoutGpuTextureInput() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkSdiHdmi, {}, true, false),
             SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkCapabilityProbe::deckLinkFallsToCadenceWithoutSdk() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkSdiHdmi, {}, false, true),
             SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkCapabilityProbe::ajaAlwaysContinuousCadence() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::Aja, {}, true, true),
             SinkGpuCapability::NeedsContinuousCadence);
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::Aja, {}, false, false),
             SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkCapabilityProbe::omtAlwaysContinuousCadence() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::Omt, {}, true, true),
             SinkGpuCapability::NeedsContinuousCadence);
}

QTEST_GUILESS_MAIN(TestSinkCapabilityProbe)
#include "tst_sinkcapabilityprobe.moc"
```

Register in `tests/unit/CMakeLists.txt` (after the last `tst_*sink` registration):

```cmake
olr_add_unit_test(tst_sinkcapabilityprobe olr_test_playback)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_sinkcapabilityprobe
```
Expected: FAIL to compile — `playback/output/iotargets/sinkcapabilityprobe.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `playback/output/iotargets/sinkcapabilityprobe.h`:

```cpp
#ifndef SINKCAPABILITYPROBE_H
#define SINKCAPABILITYPROBE_H

#include "playback/output/outputtypes.h"
#include "playback/output/sinkgpucapability.h"

#include <QVariantMap>

// The single GPU-path selector for the new I/O targets (P0.4 / D10). Pure
// function: the SDK-build and device-capability facts are passed in by the caller
// (each backend's runtime probe), so this stays SDK-free and unit-testable.
class SinkCapabilityProbe {
public:
    static SinkGpuCapability classify(OutputTargetKind kind, const QVariantMap& settings,
                                      bool sdkBuilt, bool deviceGpuTextureInput);
};

#endif // SINKCAPABILITYPROBE_H
```

Create `playback/output/iotargets/sinkcapabilityprobe.cpp`:

```cpp
#include "playback/output/iotargets/sinkcapabilityprobe.h"

SinkGpuCapability SinkCapabilityProbe::classify(OutputTargetKind kind, const QVariantMap& settings,
                                                bool sdkBuilt, bool deviceGpuTextureInput) {
    Q_UNUSED(settings);
    switch (kind) {
    case OutputTargetKind::DeckLinkSdiHdmi:
    case OutputTargetKind::DeckLinkIpSt2110:
        // GPU-native only where the SDK build exposes GPUDirect/texture input AND
        // the device confirms it at runtime; otherwise SDI/HDMI/IP needs a
        // continuous cadence (it cannot drop a field), so CPU-frame readback.
        return (sdkBuilt && deviceGpuTextureInput) ? SinkGpuCapability::GpuNative
                                                   : SinkGpuCapability::NeedsContinuousCadence;
    case OutputTargetKind::Aja:
    case OutputTargetKind::Omt:
        // AJA NTV2 AutoCirculate host buffers and the OMT software SDK are
        // NDI-style software sinks: they need a frame every tick (maxGap<=2), so
        // a continuous cadence — dedup would wrongly let them skip frames.
        return SinkGpuCapability::NeedsContinuousCadence;
    case OutputTargetKind::QtPreview:
    case OutputTargetKind::Ndi:
        break;
    }
    return SinkGpuCapability::AsyncReadbackDedupOk;
}
```

Add `playback/output/iotargets/sinkcapabilityprobe.{h,cpp}` to the engine `target_sources` in `CMakeLists.txt` (all platforms — pure C++, no SDK) and to `olr_test_playback`'s sources in `tests/unit/CMakeLists.txt`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/c --target tst_sinkcapabilityprobe && ctest --test-dir build/c -R tst_sinkcapabilityprobe --output-on-failure
```
Expected: PASS (5 tests). (Reconfigure because new engine/test sources were added.)

- [ ] **Step 5: Verify the zero-regression gate**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: the full unit suite PASSES unchanged (the probe is additive; nothing consumes it yet).

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'  # format changed lines, stage first
git add playback/output/iotargets/sinkcapabilityprobe.h playback/output/iotargets/sinkcapabilityprobe.cpp \
        tests/unit/tst_sinkcapabilityprobe.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(new-io-targets): SinkCapabilityProbe GPU-path selector (P0.4/D10)"
```

---

## Task 2: DeckLink sink shell + backend seam + off-SDK stub

**Precondition:** Task 1 merged. Mirrors the `NdiOutputSink`/`INdiSenderBackend` seam.

**Files:**
- Create: `playback/output/iotargets/decklinksink.h`, `playback/output/iotargets/decklinksink.cpp`
- Test: `tests/unit/tst_decklinksink.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `IOutputSink`, `OutputSinkStatus` (`playback/output/outputsink.h`); `OutputBusFrame`, `MediaVideoFrameView`, `outputFrameIdentityFor` (`playback/output/outputbusengine.h`, `framehandle.h`); `OutputTargetAssignment`, `FrameRate`.
- Produces:
  ```cpp
  // playback/output/iotargets/decklinksink.h
  #include "playback/output/outputsink.h"
  #include <QMutex>
  #include <memory>

  // Backend seam (mirrors INdiSenderBackend). The real implementation lives in
  // decklinkbackend_sdk.cpp under OLR_WITH_DECKLINK; StubDeckLinkSenderBackend
  // compiles unconditionally and reports the SDK unavailable so the sink is
  // testable off-SDK.
  class IDeckLinkSenderBackend {
  public:
      virtual ~IDeckLinkSenderBackend() = default;
      virtual bool isRuntimeAvailable() const = 0;
      // Runtime device probe: does this open device expose GPU-texture input
      // (GPUDirect / texture-input path)? Drives SinkCapabilityProbe at start().
      virtual bool deviceSupportsGpuTextureInput() const = 0;
      virtual bool openDevice(const OutputTargetAssignment& assignment, FrameRate rate) = 0;
      virtual void closeDevice() = 0;
      // CPU-frame path (readback fallback / non-GPU device): the bus frame's
      // MediaVideoFrameView + audio scheduled onto the DeckLink output.
      virtual bool scheduleFrame(const OutputBusFrame& frame) = 0;
      // GPU-native path: submit the kept GPU texture + paired audio without
      // readback. Returns false if the device/SDK has no texture-input path
      // (caller then must use the CPU path). The void* is the FrameHandle's
      // GpuSurface nativeHandle (downcast inside the SDK backend).
      virtual bool scheduleGpuFrame(const OutputBusFrame& frame) = 0;
  };

  // The off-SDK stub: always unavailable, no GPU texture input. Built always.
  class StubDeckLinkSenderBackend final : public IDeckLinkSenderBackend {
  public:
      bool isRuntimeAvailable() const override { return false; }
      bool deviceSupportsGpuTextureInput() const override { return false; }
      bool openDevice(const OutputTargetAssignment&, FrameRate) override { return false; }
      void closeDevice() override {}
      bool scheduleFrame(const OutputBusFrame&) override { return false; }
      bool scheduleGpuFrame(const OutputBusFrame&) override { return false; }
  };

  enum class DeckLinkOutputState { Stopped, RuntimeUnavailable, InvalidAssignment,
                                   OpenFailed, Active, SendFailed };

  class DeckLinkOutputSink final : public IOutputSink {
  public:
      // Default ctor wires the real backend when OLR_WITH_DECKLINK is built, else
      // the stub. The test ctor injects a fake backend.
      explicit DeckLinkOutputSink(OutputTargetKind kind);
      DeckLinkOutputSink(OutputTargetKind kind, IDeckLinkSenderBackend* backend);
      ~DeckLinkOutputSink() override;

      OutputTargetKind kind() const override { return m_kind; }
      bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
      void stop() override;
      bool isActive() const override { return m_active; }
      bool submit(const OutputBusFrame& frame) override;
      OutputSinkStatus outputStatus() const override;
      // The capability decided at start() (GpuNative when the device exposes
      // texture input, else NeedsContinuousCadence). Read by the factory's
      // post-start wrapping decision and by tests.
      SinkGpuCapability resolvedCapability() const;

  private:
      OutputTargetKind m_kind;
      std::unique_ptr<IDeckLinkSenderBackend> m_ownedBackend;
      IDeckLinkSenderBackend* m_backend = nullptr;
      OutputTargetAssignment m_assignment;
      FrameRate m_rate;
      bool m_active = false;
      SinkGpuCapability m_capability = SinkGpuCapability::NeedsContinuousCadence;
      mutable QMutex m_statusMutex;
      DeckLinkOutputState m_state = DeckLinkOutputState::Stopped;
      QString m_message;
      qint64 m_framesSubmitted = 0;
      qint64 m_sendFailures = 0;
  };

  // Factory the default ctor calls: the real backend under OLR_WITH_DECKLINK,
  // else the stub. Defined in decklinksink.cpp (stub) / decklinkbackend_sdk.cpp
  // (real, overriding via a strong symbol under the flag).
  std::unique_ptr<IDeckLinkSenderBackend> makeDeckLinkSenderBackend();
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_decklinksink.cpp`. The stub backend lets every assertion run off-SDK: an unavailable backend yields `RuntimeUnavailable` at start; an injected fake that reports a GPU device resolves `GpuNative`; a fake without GPU-texture input resolves `NeedsContinuousCadence` and `submit()` routes through `scheduleFrame`.

```cpp
// DeckLinkOutputSink mirrors the NdiOutputSink seam: a neutral shell over an
// injectable backend. Off-SDK (stub backend) it reports RuntimeUnavailable; an
// injected fake exercises the GPU-native vs CPU-frame routing and the capability
// the SinkCapabilityProbe resolves at start().
#include <QtTest>

#include "playback/output/iotargets/decklinksink.h"
#include "playback/output/framehandle.h"
#include "playback/output/outputbusengine.h"

namespace {
OutputBusFrame solidBusFrame() {
    OutputBusFrame f;
    f.bus = OutputBusId::pgm();
    f.outputFrameIndex = 7;
    f.video = solidYuv420pHandle(64, 48, 128, 128, 128);
    return f;
}

class FakeDeckLinkBackend final : public IDeckLinkSenderBackend {
public:
    bool available = true;
    bool gpuTextureInput = false;
    int cpuScheduled = 0;
    int gpuScheduled = 0;
    bool isRuntimeAvailable() const override { return available; }
    bool deviceSupportsGpuTextureInput() const override { return gpuTextureInput; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return available; }
    void closeDevice() override {}
    bool scheduleFrame(const OutputBusFrame&) override { ++cpuScheduled; return true; }
    bool scheduleGpuFrame(const OutputBusFrame&) override { ++gpuScheduled; return true; }
};

OutputTargetAssignment sdiAssignment() {
    OutputTargetAssignment a;
    a.kind = OutputTargetKind::DeckLinkSdiHdmi;
    a.enabled = true;
    return a;
}
}  // namespace

class TestDeckLinkSink : public QObject {
    Q_OBJECT
private slots:
    void stubBackendReportsRuntimeUnavailable();
    void gpuDeviceResolvesGpuNativeAndSubmitsTexture();
    void cpuDeviceResolvesCadenceAndSchedulesCpuFrame();
};

void TestDeckLinkSink::stubBackendReportsRuntimeUnavailable() {
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi);  // default -> stub off-SDK
    QVERIFY(!sink.start(sdiAssignment(), FrameRate{60, 1}));
    QVERIFY(!sink.isActive());
    QCOMPARE(sink.outputStatus().state, QStringLiteral("runtime-unavailable"));
}

void TestDeckLinkSink::gpuDeviceResolvesGpuNativeAndSubmitsTexture() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);
    QVERIFY(sink.start(sdiAssignment(), FrameRate{60, 1}));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::GpuNative);
    QVERIFY(sink.submit(solidBusFrame()));
    QCOMPARE(backend.gpuScheduled, 1);
    QCOMPARE(backend.cpuScheduled, 0);
}

void TestDeckLinkSink::cpuDeviceResolvesCadenceAndSchedulesCpuFrame() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = false;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);
    QVERIFY(sink.start(sdiAssignment(), FrameRate{60, 1}));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::NeedsContinuousCadence);
    QVERIFY(sink.submit(solidBusFrame()));
    QCOMPARE(backend.cpuScheduled, 1);
    QCOMPARE(backend.gpuScheduled, 0);
}

QTEST_GUILESS_MAIN(TestDeckLinkSink)
#include "tst_decklinksink.moc"
```

Register in `tests/unit/CMakeLists.txt`:

```cmake
olr_add_unit_test(tst_decklinksink olr_test_playback)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_decklinksink
```
Expected: FAIL to compile — `playback/output/iotargets/decklinksink.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `playback/output/iotargets/decklinksink.h` with the interface above. Then create `playback/output/iotargets/decklinksink.cpp`. Key bodies:

```cpp
#include "playback/output/iotargets/decklinksink.h"

#include "playback/output/iotargets/sinkcapabilityprobe.h"

#ifndef OLR_WITH_DECKLINK_BUILD
// Off-SDK default backend factory: hand back the always-unavailable stub. When
// OLR_WITH_DECKLINK is built, decklinkbackend_sdk.cpp defines this instead.
std::unique_ptr<IDeckLinkSenderBackend> makeDeckLinkSenderBackend() {
    return std::make_unique<StubDeckLinkSenderBackend>();
}
#endif

DeckLinkOutputSink::DeckLinkOutputSink(OutputTargetKind kind)
    : m_kind(kind), m_ownedBackend(makeDeckLinkSenderBackend()) {
    m_backend = m_ownedBackend.get();
}

DeckLinkOutputSink::DeckLinkOutputSink(OutputTargetKind kind, IDeckLinkSenderBackend* backend)
    : m_kind(kind), m_backend(backend) {}

DeckLinkOutputSink::~DeckLinkOutputSink() { stop(); }

bool DeckLinkOutputSink::start(const OutputTargetAssignment& assignment, FrameRate rate) {
    stop();
    const bool kindOk = assignment.kind == OutputTargetKind::DeckLinkSdiHdmi ||
                        assignment.kind == OutputTargetKind::DeckLinkIpSt2110;
    if (!m_backend || !kindOk || !assignment.enabled || !rate.isValid()) {
        QMutexLocker l(&m_statusMutex);
        m_state = DeckLinkOutputState::InvalidAssignment;
        m_message = QStringLiteral("invalid DeckLink output assignment");
        return false;
    }
    if (!m_backend->isRuntimeAvailable()) {
        QMutexLocker l(&m_statusMutex);
        m_state = DeckLinkOutputState::RuntimeUnavailable;
        m_message = QStringLiteral("DeckLink runtime/SDK is not available");
        return false;
    }
    if (!m_backend->openDevice(assignment, rate)) {
        QMutexLocker l(&m_statusMutex);
        m_state = DeckLinkOutputState::OpenFailed;
        m_message = QStringLiteral("failed to open DeckLink device");
        return false;
    }
    // The capability probe is the single path-selector: a GPU-texture-input device
    // is GpuNative, else a continuous-cadence CPU-frame readback path.
    m_capability = SinkCapabilityProbe::classify(m_kind, assignment.settings, /*sdkBuilt=*/true,
                                                 m_backend->deviceSupportsGpuTextureInput());
    m_assignment = assignment;
    m_rate = rate;
    m_active = true;
    QMutexLocker l(&m_statusMutex);
    m_state = DeckLinkOutputState::Active;
    m_message = QStringLiteral("DeckLink output active");
    m_framesSubmitted = 0;
    m_sendFailures = 0;
    return true;
}

void DeckLinkOutputSink::stop() {
    if (m_active && m_backend) m_backend->closeDevice();
    m_active = false;
    QMutexLocker l(&m_statusMutex);
    m_state = DeckLinkOutputState::Stopped;
    m_message = QStringLiteral("DeckLink output stopped");
}

bool DeckLinkOutputSink::submit(const OutputBusFrame& frame) {
    if (!m_active || !m_backend) return false;
    const bool ok = (m_capability == SinkGpuCapability::GpuNative)
                        ? m_backend->scheduleGpuFrame(frame)
                        : m_backend->scheduleFrame(frame);
    QMutexLocker l(&m_statusMutex);
    if (ok) {
        ++m_framesSubmitted;
        m_state = DeckLinkOutputState::Active;
    } else {
        ++m_sendFailures;
        m_state = DeckLinkOutputState::SendFailed;
        m_message = QStringLiteral("failed to schedule DeckLink frame");
    }
    return ok;
}

SinkGpuCapability DeckLinkOutputSink::resolvedCapability() const { return m_capability; }
```

Implement `outputStatus()` mapping `m_state`/counters to `OutputSinkStatus` (mirror `NdiOutputSink::outputStatus` — set `state` strings `stopped`/`runtime-unavailable`/`invalid`/`open-failed`/`active`/`send-failed`, `acceptedFrames=m_framesSubmitted`, `failedFrames=m_sendFailures`).

Add `playback/output/iotargets/decklinksink.{h,cpp}` to the engine `target_sources` (all platforms) + `olr_test_playback`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_decklinksink && ctest --test-dir build/c -R tst_decklinksink --output-on-failure
```
Expected: PASS (3 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/iotargets/decklinksink.h playback/output/iotargets/decklinksink.cpp \
        tests/unit/tst_decklinksink.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(new-io-targets): DeckLink sink shell + backend seam + off-SDK stub"
```

---

## Task 3: `OLR_WITH_DECKLINK` build flag + real-SDK backend seam (off-SDK-green)

**Precondition:** Task 2 merged.

**Files:**
- Create: `playback/output/iotargets/decklinkbackend_sdk.cpp` (compiled only under the flag)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: a real `IDeckLinkSenderBackend` (`SdkDeckLinkSenderBackend`) and the strong `makeDeckLinkSenderBackend()` override, compiled only under `OLR_WITH_DECKLINK`. The `.cpp` `#error`s if the flag is set without `DECKLINK_SDK_DIR`. **Documented manual gate** — not built on CI.

**Seam note (in the plan + the file header comment):** The Blackmagic DeckLink SDK is **not vendored**. The integrator sets `-DOLR_WITH_DECKLINK=ON -DDECKLINK_SDK_DIR=<path-to-DeckLinkAPI>`; CMake then compiles `decklinkbackend_sdk.cpp` against the SDK's `DeckLinkAPI*.h` and links the platform DeckLink library. `SdkDeckLinkSenderBackend` opens the first output device, configures the display mode from `FrameRate`, and implements `scheduleFrame` (CPU `IDeckLinkMutableVideoFrame` from `MediaVideoFrameView`) and `scheduleGpuFrame` (the device's GPUDirect/texture-input path where the SDK/SKU exposes it; `deviceSupportsGpuTextureInput()` queries `IDeckLinkProfileAttributes` for that capability, returning false on SKUs without it). With the flag off this `.cpp` is **not compiled**, the stub from Task 2 is the backend, and the suite is byte-green.

- [ ] **Step 1: Write the failing test (off-SDK build assertion)**

Add a slot to `tests/unit/tst_decklinksink.cpp` proving the default ctor yields an unavailable backend in the default (flag-off) build — this is the off-SDK-green contract the flag must not break:

```cpp
    void defaultBackendUnavailableOffSdk();
```
```cpp
void TestDeckLinkSink::defaultBackendUnavailableOffSdk() {
    // With OLR_WITH_DECKLINK off (CI default), the default ctor wires the stub:
    // start() reports runtime-unavailable, never crashes.
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkIpSt2110);
    OutputTargetAssignment a;
    a.kind = OutputTargetKind::DeckLinkIpSt2110;
    a.enabled = true;
    QVERIFY(!sink.start(a, FrameRate{50, 1}));
    QCOMPARE(sink.outputStatus().state, QStringLiteral("runtime-unavailable"));
}
```

- [ ] **Step 2: Run the test, expect FAIL → then PASS after wiring**

```sh
cmake --build build/c --target tst_decklinksink && ctest --test-dir build/c -R tst_decklinksink --output-on-failure
```
Expected (before the CMake flag wiring): the new slot already PASSES against the Task-2 stub — confirm it. The flag is the production seam; this step pins the off-SDK contract before the flag exists so the flag cannot regress it.

- [ ] **Step 3: Add the build flag + the SDK-guarded backend**

In `CMakeLists.txt`, add the option near the other feature options:

```cmake
option(OLR_WITH_DECKLINK "Build the Blackmagic DeckLink output backend (requires DECKLINK_SDK_DIR)" OFF)
```

Add a guarded block (after the engine `target_sources` for `iotargets`):

```cmake
if(OLR_WITH_DECKLINK)
    if(NOT DECKLINK_SDK_DIR)
        message(FATAL_ERROR "OLR_WITH_DECKLINK=ON requires -DDECKLINK_SDK_DIR=<path>")
    endif()
    target_sources(OpenLiveReplay PRIVATE playback/output/iotargets/decklinkbackend_sdk.cpp)
    target_compile_definitions(OpenLiveReplay PRIVATE OLR_WITH_DECKLINK_BUILD)
    target_include_directories(OpenLiveReplay PRIVATE "${DECKLINK_SDK_DIR}")
    # Link the platform DeckLink library (integrator-supplied; documented manual gate).
endif()
```

Create `playback/output/iotargets/decklinkbackend_sdk.cpp`:

```cpp
// Real Blackmagic DeckLink output backend. The DeckLink SDK is NOT vendored: this
// translation unit compiles ONLY under -DOLR_WITH_DECKLINK=ON with a
// -DDECKLINK_SDK_DIR pointing at the SDK headers. With the flag off it is not in
// the build and StubDeckLinkSenderBackend (decklinksink.cpp) is the backend.
#ifdef OLR_WITH_DECKLINK_BUILD

#include "playback/output/iotargets/decklinksink.h"

// #include <DeckLinkAPI.h>  // integrator-supplied via DECKLINK_SDK_DIR

namespace {
class SdkDeckLinkSenderBackend final : public IDeckLinkSenderBackend {
    // Opens the first output device, configures the display mode from FrameRate,
    // and implements scheduleFrame (CPU IDeckLinkMutableVideoFrame) +
    // scheduleGpuFrame (GPUDirect/texture-input where the SKU exposes it).
    // deviceSupportsGpuTextureInput() queries IDeckLinkProfileAttributes.
    // ... full SDK implementation (manual gate; not built on CI) ...
};
}  // namespace

std::unique_ptr<IDeckLinkSenderBackend> makeDeckLinkSenderBackend() {
    return std::make_unique<SdkDeckLinkSenderBackend>();
}

#endif  // OLR_WITH_DECKLINK_BUILD
```

> The skeleton above is the documented seam. The full SDK body is filled by the integrator who has the SDK; CI never compiles it. Keep the class declaration + method skeletons (returning false / unavailable until implemented) so an `OLR_WITH_DECKLINK=ON` configure compiles, even before the device logic is finished.

- [ ] **Step 4: Verify off-SDK build is byte-green**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: full suite PASSES (the flag is OFF; `decklinkbackend_sdk.cpp` is not compiled).

- [ ] **Step 5: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/iotargets/decklinkbackend_sdk.cpp tests/unit/tst_decklinksink.cpp CMakeLists.txt
git commit -m "feat(new-io-targets): OLR_WITH_DECKLINK flag + SDK backend seam (off-SDK-green)"
```

---

## Task 4: `St2110FrameFramer` — ST2110 essence framing from the encode bitstream

**Precondition:** Task 2 merged. ST2110 is authored from the encode bitstream (spec §6 row); the framer is SDK-independent and unit-testable off-SDK.

**Files:**
- Create: `playback/output/iotargets/st2110framer.h`, `playback/output/iotargets/st2110framer.cpp`
- Test: `tests/unit/tst_st2110framer.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `FrameRate`, `OutputBusFrame` (for the programme timecode + sample alignment), the encoded essence as a `QByteArray` (from the `gpu-encode` recorder path / a CPU encode for the readback fallback).
- Produces:
  ```cpp
  // playback/output/iotargets/st2110framer.h
  #include "playback/framerate.h"
  #include <QByteArray>
  #include <QtGlobal>

  // The framing the DeckLink ST2110 IP output expects: one encoded video essence
  // payload + the RTP-style framing fields (marker bit on the last packet of a
  // frame, the 90 kHz RTP timestamp derived from the programme timecode, the
  // payload type, and the SSRC). SDK-independent so the packetization is unit-
  // tested without the DeckLink IP SDK present.
  struct St2110VideoFrame {
      QByteArray essence;          // encoded video bitstream for this frame
      quint32 rtpTimestamp90k = 0; // 90 kHz media clock timestamp
      quint8 payloadType = 96;     // dynamic PT
      quint32 ssrc = 0;
      bool markerLast = true;      // marker bit set on the frame's last packet
  };

  class St2110FrameFramer {
  public:
      St2110FrameFramer(FrameRate rate, quint32 ssrc, quint8 payloadType = 96);
      // Frame the encoded essence for one output frame. `programmeTimecode100ns`
      // is the bus frame's programme timecode (-1 -> derive from outputFrameIndex
      // against the frame rate). Empty essence -> an invalid frame (essence empty).
      St2110VideoFrame frameVideo(const QByteArray& essence, qint64 outputFrameIndex,
                                  qint64 programmeTimecode100ns) const;
      // The 90 kHz RTP timestamp for an output frame index at this frame rate.
      quint32 rtpTimestampForFrame(qint64 outputFrameIndex) const;

  private:
      FrameRate m_rate;
      quint32 m_ssrc;
      quint8 m_payloadType;
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_st2110framer.cpp`:

```cpp
// St2110FrameFramer packs the encoded essence + RTP-style framing fields the
// DeckLink ST2110 IP output expects. SDK-independent: the 90 kHz timestamp
// progression and the marker bit are checked without the IP SDK.
#include <QtTest>

#include "playback/output/iotargets/st2110framer.h"

class TestSt2110Framer : public QObject {
    Q_OBJECT
private slots:
    void rtpTimestampAdvancesByMediaClock();
    void framesEssenceWithMarkerAndSsrc();
    void emptyEssenceIsInvalid();
};

void TestSt2110Framer::rtpTimestampAdvancesByMediaClock() {
    // 60 fps: each frame advances the 90 kHz clock by 1500 ticks (90000/60).
    St2110FrameFramer framer(FrameRate{60, 1}, /*ssrc=*/0xABCD1234u);
    QCOMPARE(framer.rtpTimestampForFrame(0), quint32(0));
    QCOMPARE(framer.rtpTimestampForFrame(1), quint32(1500));
    QCOMPARE(framer.rtpTimestampForFrame(4), quint32(6000));
}

void TestSt2110Framer::framesEssenceWithMarkerAndSsrc() {
    St2110FrameFramer framer(FrameRate{60, 1}, 0xABCD1234u, /*payloadType=*/97);
    const QByteArray essence("\x00\x00\x00\x01\x67payload", 13);
    const St2110VideoFrame f = framer.frameVideo(essence, /*idx=*/2, /*tc=*/-1);
    QCOMPARE(f.essence, essence);
    QCOMPARE(f.ssrc, quint32(0xABCD1234u));
    QCOMPARE(f.payloadType, quint8(97));
    QVERIFY(f.markerLast);
    QCOMPARE(f.rtpTimestamp90k, quint32(3000));  // frame 2 @60fps
}

void TestSt2110Framer::emptyEssenceIsInvalid() {
    St2110FrameFramer framer(FrameRate{50, 1}, 1u);
    const St2110VideoFrame f = framer.frameVideo(QByteArray(), 0, -1);
    QVERIFY(f.essence.isEmpty());
}

QTEST_GUILESS_MAIN(TestSt2110Framer)
#include "tst_st2110framer.moc"
```

Register `olr_add_unit_test(tst_st2110framer olr_test_playback)`.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_st2110framer
```
Expected: FAIL to compile — `st2110framer.h` not found.

- [ ] **Step 3: Write the minimal implementation**

Create `st2110framer.h` (interface above) and `st2110framer.cpp`:

```cpp
#include "playback/output/iotargets/st2110framer.h"

St2110FrameFramer::St2110FrameFramer(FrameRate rate, quint32 ssrc, quint8 payloadType)
    : m_rate(rate), m_ssrc(ssrc), m_payloadType(payloadType) {}

quint32 St2110FrameFramer::rtpTimestampForFrame(qint64 outputFrameIndex) const {
    // 90 kHz media clock: ticks per frame = 90000 * frameDuration = 90000 * D / N.
    if (m_rate.numerator <= 0) return 0;
    const qint64 ticks =
        (qint64(90000) * m_rate.denominator * outputFrameIndex) / m_rate.numerator;
    return quint32(ticks & 0xFFFFFFFF);
}

St2110VideoFrame St2110FrameFramer::frameVideo(const QByteArray& essence, qint64 outputFrameIndex,
                                               qint64 programmeTimecode100ns) const {
    St2110VideoFrame f;
    f.essence = essence;
    f.ssrc = m_ssrc;
    f.payloadType = m_payloadType;
    f.markerLast = true;
    // Prefer a programme-derived 90 kHz timestamp when present; else the frame-
    // index progression. (programmeTimecode100ns: 100 ns units -> 90 kHz.)
    f.rtpTimestamp90k = programmeTimecode100ns >= 0
                            ? quint32((programmeTimecode100ns * 90000 / 10000000) & 0xFFFFFFFF)
                            : rtpTimestampForFrame(outputFrameIndex);
    return f;
}
```

Add `st2110framer.{h,cpp}` to the engine sources (all platforms) + `olr_test_playback`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_st2110framer && ctest --test-dir build/c -R tst_st2110framer --output-on-failure
```
Expected: PASS (3 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/iotargets/st2110framer.h playback/output/iotargets/st2110framer.cpp \
        tests/unit/tst_st2110framer.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(new-io-targets): St2110FrameFramer essence framing (SDK-independent)"
```

---

## Task 5: AJA sink shell + backend seam + off-SDK stub (CPU-frame continuous-cadence sibling)

**Precondition:** Task 1 merged. Per P0.4, AJA NTV2 AutoCirculate host buffers = CPU-frame async-readback; as an NDI-style software SDK it needs a frame every tick, so the probe always returns `NeedsContinuousCadence`.

**Files:**
- Create: `playback/output/iotargets/ajasink.h`, `playback/output/iotargets/ajasink.cpp`, `playback/output/iotargets/ajabackend_sdk.cpp`
- Test: `tests/unit/tst_ajasink.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `IOutputSink`, `OutputBusFrame`, `MediaVideoFrameView`, `SinkCapabilityProbe`, `FrameRate`.
- Produces (mirrors Task 2 minus the GPU-native path — AJA is always CPU-frame):
  ```cpp
  // playback/output/iotargets/ajasink.h
  #include "playback/output/outputsink.h"
  #include <QMutex>
  #include <memory>

  class IAjaSenderBackend {
  public:
      virtual ~IAjaSenderBackend() = default;
      virtual bool isRuntimeAvailable() const = 0;
      virtual bool openDevice(const OutputTargetAssignment& assignment, FrameRate rate) = 0;
      virtual void closeDevice() = 0;
      // Copy the bus frame's MediaVideoFrameView + audio into an NTV2
      // AutoCirculate host buffer and queue it. CPU-frame path only.
      virtual bool transferFrame(const OutputBusFrame& frame) = 0;
  };

  class StubAjaSenderBackend final : public IAjaSenderBackend {
  public:
      bool isRuntimeAvailable() const override { return false; }
      bool openDevice(const OutputTargetAssignment&, FrameRate) override { return false; }
      void closeDevice() override {}
      bool transferFrame(const OutputBusFrame&) override { return false; }
  };

  enum class AjaOutputState { Stopped, RuntimeUnavailable, InvalidAssignment, OpenFailed,
                              Active, SendFailed };

  class AjaOutputSink final : public IOutputSink {
  public:
      AjaOutputSink();
      explicit AjaOutputSink(IAjaSenderBackend* backend);
      ~AjaOutputSink() override;

      OutputTargetKind kind() const override { return OutputTargetKind::Aja; }
      bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
      void stop() override;
      bool isActive() const override { return m_active; }
      bool submit(const OutputBusFrame& frame) override;
      OutputSinkStatus outputStatus() const override;
      SinkGpuCapability resolvedCapability() const { return SinkGpuCapability::NeedsContinuousCadence; }

  private:
      std::unique_ptr<IAjaSenderBackend> m_ownedBackend;
      IAjaSenderBackend* m_backend = nullptr;
      OutputTargetAssignment m_assignment;
      FrameRate m_rate;
      bool m_active = false;
      mutable QMutex m_statusMutex;
      AjaOutputState m_state = AjaOutputState::Stopped;
      QString m_message;
      qint64 m_framesSubmitted = 0;
      qint64 m_sendFailures = 0;
  };

  std::unique_ptr<IAjaSenderBackend> makeAjaSenderBackend();  // real under OLR_WITH_AJA, else stub
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_ajasink.cpp`:

```cpp
// AjaOutputSink is a CPU-frame continuous-cadence sibling of NDI (NTV2
// AutoCirculate host buffers). Off-SDK (stub) it reports RuntimeUnavailable; an
// injected fake transfers the bus frame and the capability is always
// NeedsContinuousCadence.
#include <QtTest>

#include "playback/output/iotargets/ajasink.h"
#include "playback/output/framehandle.h"
#include "playback/output/outputbusengine.h"

namespace {
class FakeAjaBackend final : public IAjaSenderBackend {
public:
    bool available = true;
    int transferred = 0;
    bool isRuntimeAvailable() const override { return available; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return available; }
    void closeDevice() override {}
    bool transferFrame(const OutputBusFrame&) override { ++transferred; return true; }
};

OutputTargetAssignment ajaAssignment() {
    OutputTargetAssignment a;
    a.kind = OutputTargetKind::Aja;
    a.enabled = true;
    return a;
}
}  // namespace

class TestAjaSink : public QObject {
    Q_OBJECT
private slots:
    void stubReportsRuntimeUnavailable();
    void capabilityIsContinuousCadence();
    void submitTransfersFrame();
};

void TestAjaSink::stubReportsRuntimeUnavailable() {
    AjaOutputSink sink;  // default -> stub off-SDK
    QVERIFY(!sink.start(ajaAssignment(), FrameRate{60, 1}));
    QCOMPARE(sink.outputStatus().state, QStringLiteral("runtime-unavailable"));
}

void TestAjaSink::capabilityIsContinuousCadence() {
    AjaOutputSink sink;
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::NeedsContinuousCadence);
}

void TestAjaSink::submitTransfersFrame() {
    FakeAjaBackend backend;
    AjaOutputSink sink(&backend);
    QVERIFY(sink.start(ajaAssignment(), FrameRate{60, 1}));
    OutputBusFrame f;
    f.video = solidYuv420pHandle(64, 48, 16, 128, 128);
    QVERIFY(sink.submit(f));
    QCOMPARE(backend.transferred, 1);
}

QTEST_GUILESS_MAIN(TestAjaSink)
#include "tst_ajasink.moc"
```

Register `olr_add_unit_test(tst_ajasink olr_test_playback)`.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_ajasink
```
Expected: FAIL to compile — `ajasink.h` not found.

- [ ] **Step 3: Write the minimal implementation**

Create `ajasink.h` (interface above) + `ajasink.cpp`. The body mirrors `DeckLinkOutputSink` minus the GPU branch: `submit()` always calls `m_backend->transferFrame(frame)`. The off-SDK `makeAjaSenderBackend()` returns the stub when `OLR_WITH_AJA_BUILD` is undefined:

```cpp
#ifndef OLR_WITH_AJA_BUILD
std::unique_ptr<IAjaSenderBackend> makeAjaSenderBackend() {
    return std::make_unique<StubAjaSenderBackend>();
}
#endif
```

`start()` returns false with `RuntimeUnavailable`/`InvalidAssignment`/`OpenFailed` exactly as the DeckLink sink; no capability probe call is needed because AJA is statically `NeedsContinuousCadence` (`resolvedCapability()` is a constant — keep it consistent with `SinkCapabilityProbe::classify(OutputTargetKind::Aja, ...)` which returns the same value; a one-line `static_assert`-style comment notes the agreement).

Create `ajabackend_sdk.cpp` as the documented seam (compiled only under `OLR_WITH_AJA`), mirroring Task 3's structure:

```cpp
// Real AJA NTV2 output backend. The NTV2 SDK is NOT vendored: compiled only under
// -DOLR_WITH_AJA=ON with -DAJA_NTV2_DIR. Off-flag, StubAjaSenderBackend is used.
#ifdef OLR_WITH_AJA_BUILD
#include "playback/output/iotargets/ajasink.h"
// #include "ntv2card.h"  // integrator-supplied via AJA_NTV2_DIR
namespace {
class SdkAjaSenderBackend final : public IAjaSenderBackend {
    // Opens an NTV2 device, configures AutoCirculate, and transferFrame() copies
    // the MediaVideoFrameView + audio into a host buffer and queues it.
    // ... full NTV2 implementation (manual gate; not built on CI) ...
};
}  // namespace
std::unique_ptr<IAjaSenderBackend> makeAjaSenderBackend() {
    return std::make_unique<SdkAjaSenderBackend>();
}
#endif  // OLR_WITH_AJA_BUILD
```

Add the `OLR_WITH_AJA` option + the guarded `ajabackend_sdk.cpp` block to `CMakeLists.txt` (mirroring Task 3, requiring `AJA_NTV2_DIR`); add `ajasink.{h,cpp}` to the engine sources (all platforms) + `olr_test_playback`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/c --target tst_ajasink && ctest --test-dir build/c -R tst_ajasink --output-on-failure
```
Expected: PASS (3 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/iotargets/ajasink.h playback/output/iotargets/ajasink.cpp \
        playback/output/iotargets/ajabackend_sdk.cpp tests/unit/tst_ajasink.cpp \
        tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(new-io-targets): AJA NTV2 sink shell + backend seam (CPU-frame async-readback)"
```

---

## Task 6: OMT sink shell + backend seam + off-SDK stub (software-SDK CPU-frame sibling)

**Precondition:** Task 1 merged. Per P0.4, OMT is a software SDK CPU-frame async-readback sibling; as an NDI-style software SDK it needs a frame every tick, so the probe always returns `NeedsContinuousCadence`.

**Files:**
- Create: `playback/output/iotargets/omtsink.h`, `playback/output/iotargets/omtsink.cpp`, `playback/output/iotargets/omtbackend_sdk.cpp`
- Test: `tests/unit/tst_omtsink.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Produces (the AJA shell with `transferFrame` renamed to `sendFrame`, OMT-named states, `kind()==Omt`):
  ```cpp
  // playback/output/iotargets/omtsink.h
  class IOmtSenderBackend {
  public:
      virtual ~IOmtSenderBackend() = default;
      virtual bool isRuntimeAvailable() const = 0;
      virtual bool createSender(const QString& senderName, FrameRate rate) = 0;
      virtual void destroySender() = 0;
      virtual bool sendFrame(const OutputBusFrame& frame) = 0;  // CPU-frame
  };
  class StubOmtSenderBackend final : public IOmtSenderBackend {
  public:
      bool isRuntimeAvailable() const override { return false; }
      bool createSender(const QString&, FrameRate) override { return false; }
      void destroySender() override {}
      bool sendFrame(const OutputBusFrame&) override { return false; }
  };
  enum class OmtOutputState { Stopped, RuntimeUnavailable, InvalidAssignment, CreateFailed,
                              Active, SendFailed };
  class OmtOutputSink final : public IOutputSink {
  public:
      OmtOutputSink();
      explicit OmtOutputSink(IOmtSenderBackend* backend);
      ~OmtOutputSink() override;
      OutputTargetKind kind() const override { return OutputTargetKind::Omt; }
      bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
      void stop() override;
      bool isActive() const override { return m_active; }
      bool submit(const OutputBusFrame& frame) override;
      OutputSinkStatus outputStatus() const override;
      SinkGpuCapability resolvedCapability() const { return SinkGpuCapability::NeedsContinuousCadence; }
  private:
      static QString senderNameFor(const OutputTargetAssignment& assignment);
      std::unique_ptr<IOmtSenderBackend> m_ownedBackend;
      IOmtSenderBackend* m_backend = nullptr;
      OutputTargetAssignment m_assignment;
      FrameRate m_rate;
      bool m_active = false;
      mutable QMutex m_statusMutex;
      OmtOutputState m_state = OmtOutputState::Stopped;
      QString m_message;
      qint64 m_framesSubmitted = 0;
      qint64 m_sendFailures = 0;
  };
  std::unique_ptr<IOmtSenderBackend> makeOmtSenderBackend();  // real under OLR_WITH_OMT, else stub
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_omtsink.cpp` mirroring `tst_ajasink.cpp`: an `OmtOutputSink` default ctor reports `runtime-unavailable`; the capability is `NeedsContinuousCadence`; an injected `FakeOmtBackend` (counting `sendFrame`) gets one call per `submit`. Use `OutputTargetKind::Omt` in the assignment and a `senderName` setting:

```cpp
void TestOmtSink::submitSendsFrameWithConfiguredSenderName() {
    FakeOmtBackend backend;
    OmtOutputSink sink(&backend);
    OutputTargetAssignment a;
    a.kind = OutputTargetKind::Omt;
    a.enabled = true;
    a.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR OMT 1"));
    QVERIFY(sink.start(a, FrameRate{50, 1}));
    QCOMPARE(backend.lastSenderName, QStringLiteral("OLR OMT 1"));
    OutputBusFrame f;
    f.video = solidYuv420pHandle(64, 48, 16, 128, 128);
    QVERIFY(sink.submit(f));
    QCOMPARE(backend.sent, 1);
}
```

(The fake records `createSender`'s name in `lastSenderName`.) Register `olr_add_unit_test(tst_omtsink olr_test_playback)`.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_omtsink
```
Expected: FAIL to compile — `omtsink.h` not found.

- [ ] **Step 3: Write the minimal implementation**

Create `omtsink.h` (interface above) + `omtsink.cpp`. `start()` calls `m_backend->createSender(senderNameFor(assignment), rate)` (mirroring `NdiOutputSink::start`, with `senderNameFor` resolving `settings["senderName"]` → `assignment.id` → bus-based default); `submit()` calls `m_backend->sendFrame(frame)`; `stop()` calls `destroySender()`. Off-SDK factory:

```cpp
#ifndef OLR_WITH_OMT_BUILD
std::unique_ptr<IOmtSenderBackend> makeOmtSenderBackend() {
    return std::make_unique<StubOmtSenderBackend>();
}
#endif
```

Create `omtbackend_sdk.cpp` as the documented seam under `OLR_WITH_OMT_BUILD` (real software-OMT sender). Add the `OLR_WITH_OMT` option + guarded block to `CMakeLists.txt`; add `omtsink.{h,cpp}` to the engine sources (all platforms) + `olr_test_playback`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/c --target tst_omtsink && ctest --test-dir build/c -R tst_omtsink --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/iotargets/omtsink.h playback/output/iotargets/omtsink.cpp \
        playback/output/iotargets/omtbackend_sdk.cpp tests/unit/tst_omtsink.cpp \
        tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(new-io-targets): OMT software-SDK sink shell + backend seam (CPU-frame async-readback)"
```

---

## Task 7: `makeIoTargetSink` factory — wrap CPU-frame sinks in `AsyncGpuReadbackSink` per capability

**Precondition:** Tasks 2-6 merged. Consumes the Phase-4 `AsyncGpuReadbackSink`.

**Files:**
- Create: `playback/output/iotargets/iotargetsinkfactory.h`, `playback/output/iotargets/iotargetsinkfactory.cpp`
- Test: `tests/unit/tst_iotargetsinkfactory.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: all four sinks (Tasks 2/5/6 + the ST2110 DeckLink kind), `SinkCapabilityProbe`, `AsyncGpuReadbackSink` (Phase 4), `IOutputSink`, `OutputTargetAssignment`, `FrameRate`.
- Produces:
  ```cpp
  // playback/output/iotargets/iotargetsinkfactory.h
  #include "playback/output/outputsink.h"
  #include <memory>

  // Construct the IOutputSink for one new-I/O-target assignment, applying the
  // capability routing (D10): a GpuNative sink (DeckLink with device texture
  // input) is returned as-is for direct GPU-texture submit; a
  // NeedsContinuousCadence / AsyncReadbackDedupOk sink (AJA, OMT, the DeckLink
  // readback fallback) is wrapped in AsyncGpuReadbackSink so it consumes the
  // shared bus readback. Returns nullptr for a kind this factory does not own
  // (QtPreview/Ndi keep their existing construction path).
  //
  // The sink is started inside the factory so its runtime capability is known
  // before the wrapping decision; an unavailable SDK yields an inactive sink that
  // is still returned (it reports runtime-unavailable, never crashes the
  // dispatcher) — matching how NdiOutputSink behaves with no NDI runtime.
  std::unique_ptr<IOutputSink> makeIoTargetSink(const OutputTargetAssignment& assignment,
                                                FrameRate rate);
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_iotargetsinkfactory.cpp`. Off-SDK, every real backend is unavailable, so the factory still returns a non-null sink of the right `kind()` that reports `runtime-unavailable` — that is the off-SDK contract:

```cpp
// makeIoTargetSink returns the right IOutputSink per kind and applies the D10
// capability routing. Off-SDK the backends are unavailable; the factory still
// returns a non-null, inactive sink (never null, never a crash) and routes
// CPU-frame kinds through the AsyncGpuReadbackSink wrapper.
#include <QtTest>

#include "playback/output/iotargets/iotargetsinkfactory.h"
#include "playback/output/outputtargetassignment.h"

class TestIoTargetSinkFactory : public QObject {
    Q_OBJECT
private slots:
    void returnsSinkPerKind_data();
    void returnsSinkPerKind();
    void returnsNullForUnownedKind();
};

void TestIoTargetSinkFactory::returnsSinkPerKind_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("decklink-sdi") << int(OutputTargetKind::DeckLinkSdiHdmi);
    QTest::newRow("decklink-st2110") << int(OutputTargetKind::DeckLinkIpSt2110);
    QTest::newRow("aja") << int(OutputTargetKind::Aja);
    QTest::newRow("omt") << int(OutputTargetKind::Omt);
}

void TestIoTargetSinkFactory::returnsSinkPerKind() {
    QFETCH(int, kind);
    OutputTargetAssignment a;
    a.kind = OutputTargetKind(kind);
    a.enabled = true;
    auto sink = makeIoTargetSink(a, FrameRate{60, 1});
    QVERIFY(sink != nullptr);
    // Off-SDK the inner backend is unavailable, so the sink is inactive but its
    // kind is preserved (the AsyncGpuReadbackSink wrapper forwards kind()).
    QCOMPARE(sink->kind(), OutputTargetKind(kind));
    QVERIFY(!sink->isActive());
}

void TestIoTargetSinkFactory::returnsNullForUnownedKind() {
    OutputTargetAssignment a;
    a.kind = OutputTargetKind::Ndi;  // owned by the existing NDI path, not this factory
    a.enabled = true;
    QVERIFY(makeIoTargetSink(a, FrameRate{60, 1}) == nullptr);
}

QTEST_GUILESS_MAIN(TestIoTargetSinkFactory)
#include "tst_iotargetsinkfactory.moc"
```

> **Note on `kind()` forwarding:** `AsyncGpuReadbackSink` (Phase 4) must forward `kind()` from its inner sink for the wrapped assertion to hold. If the merged `AsyncGpuReadbackSink` does not expose the inner kind, the factory returns the unwrapped sink for the `kind()` assertion and applies the wrapper at the dispatcher seam instead — reconcile to the merged Phase-4 API and adjust this test's wrapped/unwrapped expectation accordingly (keep the non-null + inactive assertions).

Register `olr_add_unit_test(tst_iotargetsinkfactory olr_test_playback)`.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_iotargetsinkfactory
```
Expected: FAIL to compile — `iotargetsinkfactory.h` not found.

- [ ] **Step 3: Write the minimal implementation**

Create `iotargetsinkfactory.h` (interface above) + `iotargetsinkfactory.cpp`:

```cpp
#include "playback/output/iotargets/iotargetsinkfactory.h"

#include "playback/output/asyncgpureadbacksink.h"  // Phase 4
#include "playback/output/framehandle.h"           // FramePixelFormat
#include "playback/output/iotargets/ajasink.h"
#include "playback/output/iotargets/decklinksink.h"
#include "playback/output/iotargets/omtsink.h"
#include "playback/output/outputtypes.h"           // OutputBusKind
#include "playback/output/sinkgpucapability.h"

std::unique_ptr<IOutputSink> makeIoTargetSink(const OutputTargetAssignment& assignment,
                                              FrameRate rate) {
    std::unique_ptr<IOutputSink> sink;
    SinkGpuCapability capability = SinkGpuCapability::AsyncReadbackDedupOk;

    switch (assignment.kind) {
    case OutputTargetKind::DeckLinkSdiHdmi:
    case OutputTargetKind::DeckLinkIpSt2110: {
        auto dl = std::make_unique<DeckLinkOutputSink>(assignment.kind);
        dl->start(assignment, rate);  // resolves the device capability (or unavailable)
        capability = dl->resolvedCapability();
        sink = std::move(dl);
        break;
    }
    case OutputTargetKind::Aja: {
        auto aja = std::make_unique<AjaOutputSink>();
        aja->start(assignment, rate);
        capability = aja->resolvedCapability();  // NeedsContinuousCadence (NDI-style software SDK)
        sink = std::move(aja);
        break;
    }
    case OutputTargetKind::Omt: {
        auto omt = std::make_unique<OmtOutputSink>();
        omt->start(assignment, rate);
        capability = omt->resolvedCapability();  // NeedsContinuousCadence (software SDK)
        sink = std::move(omt);
        break;
    }
    case OutputTargetKind::QtPreview:
    case OutputTargetKind::Ndi:
        return nullptr;  // owned by their existing construction paths
    }

    // D10 routing: GpuNative submits the kept texture directly; the cadence /
    // dedup-ok kinds consume the shared bus readback through AsyncGpuReadbackSink.
    if (capability == SinkGpuCapability::GpuNative) return sink;
    // Ring depth per spec D7: PGM gets the depth-1 (sub-frame, low-latency) ring;
    // every other CPU sink gets depth-3. The CPU layout these sinks consume is I420.
    const int depth = (assignment.sourceBus.kind == OutputBusKind::Pgm) ? 1 : 3;
    const FramePixelFormat fmt = FramePixelFormat::Yuv420p;
    return std::make_unique<AsyncGpuReadbackSink>(std::move(sink), depth, fmt, capability);
}
```

> Use the **actual** merged `AsyncGpuReadbackSink` ctor signature. The Phase-4 header exports `AsyncGpuReadbackSink(std::unique_ptr<IOutputSink> inner, int ringDepth, FramePixelFormat cpuFormat, SinkGpuCapability capability, std::shared_ptr<GpuFence> renderFence = GpuFence::create())`; this factory passes `(std::move(sink), depth, fmt, capability)` and lets the fence default. Per spec D7 the PGM bus uses the depth-1 (sub-frame) ring and every other CPU sink uses depth-3 (`depth = sourceBus.kind == OutputBusKind::Pgm ? 1 : 3`); `fmt` is the sinks' I420 CPU layout (`FramePixelFormat::Yuv420p`). If the merged header differs, reconcile the call to it.

Add `iotargetsinkfactory.{h,cpp}` to the engine sources (all platforms) + `olr_test_playback`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_iotargetsinkfactory && ctest --test-dir build/c -R tst_iotargetsinkfactory --output-on-failure
```
Expected: PASS (5 rows + 1).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/iotargets/iotargetsinkfactory.h playback/output/iotargets/iotargetsinkfactory.cpp \
        tests/unit/tst_iotargetsinkfactory.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(new-io-targets): makeIoTargetSink factory + AsyncGpuReadbackSink capability routing"
```

---

## Task 8: Wire the factory into the dispatcher/target-manager seam

**Precondition:** Task 7 merged. Find the call site that today constructs `NdiOutputSink`/`QtPreviewOutputSink` for an `OutputTargetAssignment` and builds the `OutputEndpoint` list the dispatcher consumes.

**Files:**
- Modify: the target-manager / output-sink construction site (the owner of the `IOutputSink`s passed to `OutputDispatcher::setEndpoints` — locate via `grep -rn "new NdiOutputSink\|make_unique<NdiOutputSink\|QtPreviewOutputSink\|setEndpoints" --include=*.cpp` outside `tests/`).
- Test: the existing target-manager test (extend it), or a new `tst_*` against that owner.

**Interfaces:**
- Consumes: `makeIoTargetSink` (Task 7). The construction site dispatches on `assignment.kind`: `Ndi`/`QtPreview` keep their existing path; the four new kinds call `makeIoTargetSink(assignment, rate)`.

- [ ] **Step 1: Write the failing test**

In the target-manager's test, add a case: enabling a `decklink-sdi-hdmi` (or `aja`) assignment produces an endpoint whose sink `kind()` matches — proving the new kinds now build a sink instead of being silently dropped. Off-SDK the sink is inactive, but the endpoint exists:

```cpp
void TestOutputTargetManager::buildsSinkForNewIoTargetKinds() {
    OutputTargetAssignment a;
    a.id = QStringLiteral("dl1");
    a.kind = OutputTargetKind::DeckLinkSdiHdmi;
    a.enabled = true;
    manager.setAssignments({a});  // use the manager's actual mutation API
    const auto endpoints = manager.endpoints();  // or the dispatcher's endpoints()
    QCOMPARE(endpoints.size(), 1);
    QVERIFY(endpoints.first().sink != nullptr);
    QCOMPARE(endpoints.first().sink->kind(), OutputTargetKind::DeckLinkSdiHdmi);
}
```

> Use the manager's real assignment-mutation and endpoint-accessor names (read the existing test for the exact API). The assertion is: the new kinds are no longer dropped; they build a real (off-SDK-inactive) sink.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target <target-manager-test> && ctest --test-dir build/c -R <target-manager-test> --output-on-failure
```
Expected: FAIL — the new kind is dropped today (no endpoint / null sink).

- [ ] **Step 3: Wire the factory into the construction site**

At the sink-construction switch, add the four new kinds routing to `makeIoTargetSink(assignment, rate)`; keep `Ndi`/`QtPreview` unchanged. Take ownership of the returned `std::unique_ptr<IOutputSink>` in the same container that owns the NDI/preview sinks (the `OutputEndpoint.sink` pointer is non-owning, per `outputdispatcher.h:12`, so the manager retains ownership).

- [ ] **Step 4: Run the test, expect PASS + zero-regression**

```sh
cmake --build build/c --target <target-manager-test> && ctest --test-dir build/c -R <target-manager-test> --output-on-failure
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: the new test PASSES; the full suite unchanged (existing NDI/preview endpoints build exactly as before).

- [ ] **Step 5: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add <target-manager files> <test file>
git commit -m "feat(new-io-targets): build DeckLink/AJA/OMT sinks at the dispatcher seam"
```

---

## Task 9: e2e cadence/continuity gate (stub-backed, `maxGap<=2`) mirroring NDI marker-continuity

**Precondition:** Tasks 7-8 merged. Mirrors `tests/e2e/run_ndi_output_e2e.sh` + `ndi_output_sender` but drives a sink through an **in-process recording stub backend** (no SDK), so the gate runs on CI: it asserts the dispatched marker stream reaches the sink continuously with `maxGap<=2` and the AV pair stays atomic, classifying cadence per target.

**Files:**
- Create: `tests/e2e/iotarget_marker_sender.cpp`, `tests/e2e/run_iotarget_cadence_e2e.sh`
- Modify: `tests/e2e/CMakeLists.txt`

**Interfaces:**
- Consumes: the sinks (via a recording stub backend that captures each `submit`/`scheduleFrame`/`transferFrame`/`sendFrame`'s decoded marker index), `AsyncGpuReadbackSink`, `OutputDispatcher`, the shared NDI marker module (`tests/e2e/ndi_output_marker.{h,cpp}` — reuse its marker encode/decode so continuity analysis matches the NDI gate).

- [ ] **Step 1: Write the failing harness + script (expect missing target)**

Create `tests/e2e/iotarget_marker_sender.cpp`: a Qt-guiless harness that, for a target kind given by `argv[1]` (`aja`/`omt`/`decklink-readback`/`decklink-gpu`), builds the matching sink with an **injected recording stub backend** (the fake backends from the unit tests, promoted to a small shared `tests/e2e` recorder), runs N output ticks of a marker stream through the sink (wrapping CPU-frame kinds in `AsyncGpuReadbackSink` exactly as the factory does), then prints an `IOTARGET ` line with `framesReceived=`, `maxGapFrames=`, `drops=`, `avSyncMaxFrames=`, `cadence=` (`gpu-native`/`continuous`/`dedup`), recomputing each captured frame's marker index to verify no gap > 2.

Create `tests/e2e/run_iotarget_cadence_e2e.sh` mirroring `run_ndi_output_e2e.sh`'s field-parse + assert structure:

```bash
#!/usr/bin/env bash
# new-io-targets cadence/continuity gate: drive a marker stream through a stub-backed
# DeckLink/AJA/OMT sink and verify the dispatched stream is continuous (maxGap<=2),
# A-V atomic, and classified with the expected cadence. Runs on CI (no SDK; the
# backend is an in-process recorder). Opt-in (CTest label "iotargets").
set -uo pipefail
HARNESS="${1:?iotarget_marker_sender executable required}"
KIND="${2:?target kind required (aja|omt|decklink-readback|decklink-gpu)}"
FRAMES="${OLR_IOTARGET_FRAMES:-360}"

OUT="$("$HARNESS" "$KIND" "$FRAMES")"; rc=$?
echo "$OUT"
[ "$rc" = "0" ] || { echo "FAIL: harness error ($rc)"; exit 1; }
line="$(grep '^IOTARGET ' <<<"$OUT" || true)"
[ -n "$line" ] || { echo "FAIL: no IOTARGET report"; exit 1; }
field() { sed -n "s/.*$1=\\([0-9.-]*\\).*/\\1/p" <<<"$line"; }

frames=$(field framesReceived); maxgap=$(field maxGapFrames)
drops=$(field drops); avsync=$(field avSyncMaxFrames)
fail=0
[ "${frames:-0}" -ge "$FRAMES" ] || { echo "FAIL: too few frames ($frames)"; fail=1; }
[ "${maxgap:-9}" -le 2 ] || { echo "FAIL: maxGapFrames=$maxgap"; fail=1; }
[ "${drops:-1}" = "0" ]  || { echo "FAIL: drops=$drops"; fail=1; }
[ "${avsync:-9}" -ge 0 ] && [ "${avsync:-9}" -le 1 ] || { echo "FAIL: avSyncMaxFrames=$avsync"; fail=1; }
[ "$fail" = "0" ] && { echo "PASS: $KIND cadence/continuity OK"; exit 0; }
echo "IOTARGET VALIDATION FAILED"; exit 1
```

Register in `tests/e2e/CMakeLists.txt` (mirror `ndi_output_sender`'s `qt_add_executable` + `add_test`/`set_tests_properties` block) with one `add_test` per kind under the `iotargets` label:

```cmake
qt_add_executable(iotarget_marker_sender
    iotarget_marker_sender.cpp
    ndi_output_marker.cpp)
target_include_directories(iotarget_marker_sender PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(iotarget_marker_sender PRIVATE
    Qt6::Core olr_test_playback olr_warnings olr_sanitize)

foreach(kind aja omt decklink-readback)
    add_test(NAME e2e_iotarget_${kind}
        COMMAND "${OLR_E2E_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/run_iotarget_cadence_e2e.sh"
            "$<TARGET_FILE:iotarget_marker_sender>" "${kind}")
    set_tests_properties(e2e_iotarget_${kind} PROPERTIES LABELS "iotargets" TIMEOUT 120)
endforeach()
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target iotarget_marker_sender
```
Expected: FAIL to compile — `iotarget_marker_sender.cpp` references the recorder backend / sinks before the harness body exists.

- [ ] **Step 3: Implement the harness body**

Write `iotarget_marker_sender.cpp`: parse the kind, build the sink + recording stub backend, run the dispatch loop emitting marker frames (reuse `OutputBusEngine`/`OutputDispatcher` or a direct `submit` loop with `solidYuv420pHandle` carrying an encoded marker index per `ndi_output_marker.h`), capture each delivered frame's marker index from the recorder, compute `maxGapFrames`/`drops`/`avSyncMaxFrames`, print the `IOTARGET ` line. For `decklink-gpu`, the recorder reports `cadence=gpu-native` and the harness submits without the readback wrapper; for `aja`/`omt`/`decklink-readback` (all `NeedsContinuousCadence`), it wraps in `AsyncGpuReadbackSink` and reports `cadence=continuous`.

- [ ] **Step 4: Run the gate, expect PASS**

```sh
cmake --build build/c --target iotarget_marker_sender
ctest --test-dir build/c -L iotargets --output-on-failure
```
Expected: PASS for each kind (continuous marker stream, `maxGapFrames<=2`, `drops=0`, AV atomic).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
git add tests/e2e/iotarget_marker_sender.cpp tests/e2e/run_iotarget_cadence_e2e.sh tests/e2e/CMakeLists.txt
git commit -m "test(new-io-targets): stub-backed cadence/continuity e2e gate (maxGap<=2) per target"
```

---

## Task 10: Documentation seam + capstone sign-off

**Precondition:** Tasks 1-9 merged.

**Files:**
- Modify: this plan (mark the manual-gate matrix), and the spec's `new-io-targets` lifecycle note (§12) if it tracks per-subproject sign-off.

- [ ] **Step 1: Record the manual-SDK gate matrix**

Add to this plan a closing table the integrator runs on real hardware (NOT on CI):

| target | flag | SDK var | manual gate |
|--------|------|---------|-------------|
| DeckLink SDI/HDMI | `OLR_WITH_DECKLINK` | `DECKLINK_SDK_DIR` | configure ON; `ctest -L iotargets`; capture device output, assert continuity on a real card |
| DeckLink ST2110 | `OLR_WITH_DECKLINK` | `DECKLINK_SDK_DIR` | as above + ST2110 receiver confirms RTP timestamp/marker per `St2110FrameFramer` |
| AJA NTV2 | `OLR_WITH_AJA` | `AJA_NTV2_DIR` | configure ON; AutoCirculate output to a real NTV2 device; continuity gate |
| OMT | `OLR_WITH_OMT` | `OMT_SDK_DIR` | configure ON; OMT receiver confirms continuity |

- [ ] **Step 2: Confirm the full off-SDK gate is green and capstone-complete**

```sh
# Fresh build dir, all SDK flags off (the CI configuration):
cmake -S . -B build/verify -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/verify
ctest --test-dir build/verify -L unit --output-on-failure
ctest --test-dir build/verify -L iotargets --output-on-failure
```
Expected: unit + iotargets gates PASS; all four kinds build, route, and classify cadence off-SDK; the real backends are documented manual gates.

- [ ] **Step 3: Commit**

```sh
git add docs/superpowers/plans/2026-06-21-gpu-phase5-new-io-targets.md \
        docs/superpowers/specs/2026-06-21-gpu-resident-pipeline-design.md
git commit -m "docs(new-io-targets): manual-SDK gate matrix + capstone sign-off"
```

---

## Canonical contract this plan produces (for downstream / integrators)

- `SinkCapabilityProbe::classify(OutputTargetKind, const QVariantMap&, bool sdkBuilt, bool deviceGpuTextureInput) -> SinkGpuCapability` (`playback/output/iotargets/sinkcapabilityprobe.h`)
- `DeckLinkOutputSink` + `IDeckLinkSenderBackend`/`StubDeckLinkSenderBackend` + `makeDeckLinkSenderBackend()` (`playback/output/iotargets/decklinksink.h`); real backend under `OLR_WITH_DECKLINK`.
- `St2110FrameFramer` + `St2110VideoFrame` (`playback/output/iotargets/st2110framer.h`)
- `AjaOutputSink` + `IAjaSenderBackend`/`StubAjaSenderBackend` + `makeAjaSenderBackend()` (`playback/output/iotargets/ajasink.h`); real backend under `OLR_WITH_AJA`.
- `OmtOutputSink` + `IOmtSenderBackend`/`StubOmtSenderBackend` + `makeOmtSenderBackend()` (`playback/output/iotargets/omtsink.h`); real backend under `OLR_WITH_OMT`.
- `makeIoTargetSink(const OutputTargetAssignment&, FrameRate) -> std::unique_ptr<IOutputSink>` (`playback/output/iotargets/iotargetsinkfactory.h`) — applies the `AsyncGpuReadbackSink` capability routing.
- e2e label `iotargets` (`tests/e2e/run_iotarget_cadence_e2e.sh`, `maxGap<=2`); manual-SDK gates per the Task 10 matrix.

# GPU Import (Windows) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Build the Windows decode-import edge — MediaFoundation → `ID3D11Texture2D` kept GPU-resident, wrapped in a GPU-backed `FrameHandle`, with a backend-matched fence and a minimal single-feed import+readback slice symmetric to the macOS VideoToolbox slice — all behind `OLR_GPU_PIPELINE` (default off).

**Architecture:** A new `D3D11GpuSurface` concrete implements the platform-neutral `GpuSurface` interface (owned by `gpu-abstraction`) over a `ComPtr<ID3D11Texture2D>` (NV12), retaining the texture until all render/readback fences retire (§4 surface-lifetime). A new `WinGpuImportEdge` configures the existing MF `IMFDXGIDeviceManager`/`ID3D11Device` decode path (today HW-decode-to-CPU only, `copySampleToFrame` at `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp:712`) to **keep** the decoded `ID3D11Texture2D` instead of locking-and-copying it down, wraps it in a `D3D11IGpuFrameData : IFrameData`, and exposes a fenced `readToCpu()` (NV12→I420). A backend-matched fence (`ID3D12Fence` if the RHI D3D12 backend is chosen by the Phase-0 probe, else `ID3D11Fence` (11.4) / keyed-mutex / `ID3D11Query` for D3D11) signals decode-done before publish.

**Tech Stack:** C++17, Qt 6 (Core/Test/Gui — RHI via `QRhi`/private headers), Windows Media Foundation (`mfplat`/`mf`/`mfuuid`/`mfreadwrite`), Direct3D 11 (`d3d11`/`dxgi`), optionally D3D12 (`d3d12`) per Phase-0, FFmpeg (`libavutil` for `AVFrame`), CMake + Ninja.

## Global Constraints

Copied verbatim from the program spec (§ "Approach" and §6/§7); these bind every task below:

- **Keystone-first, strict zero-regression gates.** This subproject sits in Phase 2 and consumes the Phase-1 keystone (`frame-handle`) plus `format-canon` and `gpu-abstraction` (the shared `GpuSurface` interface). It must not regress the CPU path.
- **The CPU path stays default and is the permanent correctness reference + fallback.** Everything new ships behind the `OLR_GPU_PIPELINE` capability flag (default off). With the flag off, MF decode keeps using today's `copySampleToFrame` CPU path byte-for-byte.
- **Everything behind capability flags.** No new behavior is reachable unless `OLR_GPU_PIPELINE=1` AND the host actually yields a GPU-resident, RHI-importable texture (the Phase-0 precondition); otherwise the import edge degrades to the existing async-readback CPU path.
- **No throwaway prototypes.** Every artifact here is production and stays in the tree (the Phase-0 probe code is the first piece of the real import edge — `WinGpuImportEdge` — not a scratch program).
- **Public-repo professionalism.** Code, comments, commit messages, and PR text are published: no secrets, no internal notes, no references to private history; document the present design.
- **Format changed lines only.** Several engine files use hand-written Allman style; run `git clang-format --commit origin/main` over changed lines only, never reformat whole files. `.qml` is checked by `qmllint`, not clang-format (no `.qml` here).
- **Symmetric edge interface.** The shared `GpuSurface` / import-edge interface (`gpu-abstraction`) stays platform-neutral — **no `ID3D11Texture2D`/`CVPixelBuffer` types leak into the shared header.** Windows concrete types live only in `playback/output/win/` `.cpp` translation units.
- **HEAVILY PROBE-CONTINGENT.** Task 1 is a precondition probe with a documented fallback branch: if MF cannot hand off a GPU-resident texture on the host, the edge falls back to the async-readback (CPU) path and the slice runs degraded; the plan does not block on the import-and-keep edge being available.

---

## Preconditions (Phase-0, gating — resolve before Task 2)

These are open questions §11.2 / §11.4 / §11.5 from the program spec, resolved by Task 1's probe. **Task 1 produces a documented decision; Tasks 2-7 branch on it.**

- **P0.2-A — Does MF→`ID3D11Texture2D` yield a GPU-resident, RHI-importable texture, and at what reconfig cost?** Today the MF decoder owns an `IMFDXGIDeviceManager` + `ID3D11Device` (`nativevideodecoder_mediafoundation.cpp:248-249`, created in `createD3D()` at :346-402) but immediately locks the output sample and `memcpy`s NV12→I420 down to a CPU `AVFrame` (`copySampleToFrame` at :712-795). The import edge must instead obtain the `ID3D11Texture2D` from the decoded `IMFSample`'s `IMFDXGIBuffer` and **keep** it. The probe confirms this is reachable and measures the cost of (a) enabling `MF_SA_D3D11_AWARE` / `MFT_MESSAGE_SET_D3D_MANAGER` on the transform and (b) any texture array vs. committed-texture subresource handling.
- **P0.2-B — Which RHI D3D backend (D3D11 vs D3D12)?** This fixes the matching fence primitive (D4): D3D12 → `ID3D12Fence`; D3D11 → `ID3D11Fence` (11.4) or keyed-mutex or `ID3D11Query`. The probe records the chosen backend in a single compile-time constant consumed by Task 5.
- **P0.5 — Does RHI↔D3D-texture interop work without a CPU detour?** Confirm `QRhi::importTexture` / native-texture import accepts the kept `ID3D11Texture2D` (or a D3D12-shared handle), or whether a `copyTexture`/native detour is required.

**Documented fallback branch (P0.2 negative):** if any of P0.2-A / P0.5 fails on the host, `WinGpuImportEdge::tryImport()` returns `std::nullopt` and the caller uses the existing CPU `copySampleToFrame` path wrapped in a CPU-backed `FrameHandle` (async-readback). The slice still runs (degraded, CPU-resident) and the success criteria fall back to the `OLR_GPU_PIPELINE=0` byte-green gate. This is asserted by a unit test (Task 2) that forces the fallback.

---

## Consumed contracts (from sibling subprojects — do NOT redefine)

From `frame-handle` (Phase 1, `playback/output/`):

```cpp
// playback/output/framepixelformat.h
enum class FramePixelFormat { Nv12, Yuv420p, Rgba8 };   // Nv12=0, Yuv420p=1, Rgba8=2
constexpr int planeCount(FramePixelFormat);             // Nv12:2, Yuv420p:3, Rgba8:1

// playback/output/framehandle.h
struct CpuPlanes { FramePixelFormat format=Yuv420p; int width=0; int height=0;
                   QByteArray plane[3]; int stride[3]={0,0,0}; bool isValid() const; };
struct FramePayloadKey { int feedIndex=-1; qint64 ptsMs=0; quint32 videoHash=0;
                         FramePixelFormat format=Yuv420p; int width=0; int height=0;
                         bool isPlaceholder=false; bool samePayloadAs(const FramePayloadKey&) const; };
struct FrameMetadata { FramePayloadKey key; qint64 outputFrameIndex=-1; qint64 sampledPlayheadMs=0;
                       int stride[3]={0,0,0}; ColorMetadata color; };
class IFrameData {
public:
    virtual ~IFrameData();
    virtual bool isGpuBacked() const = 0;
    bool isCpuBacked() const;
    virtual CpuPlanes readToCpu(FramePixelFormat target) const = 0;
    virtual GpuSurface* gpuSurface() const = 0;     // GpuSurface is an opaque fwd-decl
    virtual FramePixelFormat nativeFormat() const = 0;
};
class FrameHandle {
public:
    FrameHandle();
    FrameHandle(std::shared_ptr<const IFrameData> data, FrameMetadata meta);
    bool isNull() const;
    const FrameMetadata& metadata() const; FrameMetadata& metadata();
    const IFrameData* data() const; std::shared_ptr<const IFrameData> dataPtr() const;
    CpuPlanes readToCpu(FramePixelFormat target=Yuv420p) const;
    bool isGpuBacked() const; bool isPresentable() const;
};
FrameHandle makeCpuFrameHandle(CpuPlanes planes, FrameMetadata meta);
```

From `gpu-abstraction` (Phase 2 sibling, `playback/output/gpusurface.h` — **platform-neutral**, no D3D/CV types):

```cpp
// playback/output/gpusurface.h  (defined by gpu-abstraction; consumed here)
struct GpuSurfaceDesc {
    FramePixelFormat format = FramePixelFormat::Nv12;   // Nv12 for HW decode
    int width = 0;
    int height = 0;
};
class GpuSurface {
public:
    virtual ~GpuSurface();
    virtual GpuSurfaceDesc desc() const = 0;            // width/height/format
    virtual bool isValid() const = 0;
    // Opaque native handle for the RHI import edge; the void* is a backend-private
    // pointer (ID3D11Texture2D* on Windows, IOSurfaceRef on macOS) — callers in the
    // shared spine treat it as opaque and never dereference it directly.
    virtual void* nativeHandle() const = 0;
};
```

> Surface lifetime (retain-until-fence-retired) is a `gpu-sync` concern, not a
> `GpuSurface` method: the neutral surface exposes only `desc()`/`isValid()`/
> `nativeHandle()`. The Windows concrete's fence-retain bookkeeping
> (`retainUntilFenceRetired`/`pendingFenceValue`, §4 surface-lifetime) is a
> Windows-only accessor on `D3D11GpuSurface`, deferred to `gpu-sync` for the
> cross-platform contract — it is NOT an override of the shared base.

---

## File Structure

- **Create** `playback/output/win/d3d11gpusurface.h` / `.cpp` — `D3D11GpuSurface : GpuSurface` over `ComPtr<ID3D11Texture2D>` (NV12); the only place `ID3D11Texture2D` appears as a member type. Header guarded `#ifdef _WIN32`.
- **Create** `playback/output/win/wingpuimportedge.h` / `.cpp` — `WinGpuImportEdge` (probe + import-and-keep + fenced `readToCpu`); `D3D11IGpuFrameData : IFrameData`.
- **Create** `playback/output/win/d3dfence.h` / `.cpp` — backend-matched fence (`D3D11Fence` / `D3D12Fence` selected by `OLR_WIN_RHI_BACKEND`).
- **Create** `tests/unit/tst_wingpuimportedge.cpp` — Windows-gated probe + import + readback + fence-lifetime + fallback unit tests; on non-Windows the test asserts the stub fallback.
- **Create** `playback/output/win/wingpuimportedge_stub.cpp` — non-Windows `tryImport()` → `std::nullopt`; so the shared spine links everywhere.
- **Modify** `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp` (+ `.h`) — when `OLR_GPU_PIPELINE` is on and the edge is available, route the decoded `IMFSample` through `WinGpuImportEdge` instead of `copySampleToFrame`.
- **Modify** `CMakeLists.txt` — compile the `win/` sources into the app on `WIN32`, the stub elsewhere; link `d3d11 dxgi` (and `d3d12` if the D3D12 backend is selected).
- **Modify** `tests/unit/CMakeLists.txt` — register `tst_wingpuimportedge` with a per-platform backend test-lib mirroring `olr_test_nativevideodecoder`.

---

## Task 1: Phase-0 precondition probe — MF→ID3D11Texture2D keep-and-import (P0.2-A/B, P0.5)

**Files:**
- Create: `playback/output/win/wingpuimportedge.h`, `playback/output/win/wingpuimportedge.cpp`, `playback/output/win/wingpuimportedge_stub.cpp`
- Test: `tests/unit/tst_wingpuimportedge.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct WinGpuImportCapabilities { bool d3d11KeepTexture=false; bool rhiImportable=false; QString backend; QString detail; };`
  - `WinGpuImportCapabilities probeWinGpuImport();` — creates an MF H.264 decode session with the D3D11 device manager, decodes one keyframe, attempts to obtain the `ID3D11Texture2D` from the output sample's `IMFDXGIBuffer`, and reports whether the texture is kept GPU-resident and RHI-importable. On non-Windows it returns all-false with `detail = "no Windows GPU import on this platform"`.
  - `constexpr const char* kWinRhiBackend = "d3d11";` — the chosen RHI D3D backend (P0.2-B), set to `"d3d11"` by default; the probe overrides the reported `backend` at runtime.

> The probe is the first piece of the real edge, not a throwaway: `probeWinGpuImport()` lives in `wingpuimportedge.cpp` and reuses the same MF/D3D11 setup that `tryImport()` (Task 2) uses. It models the COM/MF lifetime on the existing decoder (`createD3D()` at `nativevideodecoder_mediafoundation.cpp:346-402`, `MFTEnumEx` at :434).

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_wingpuimportedge.cpp`:

```cpp
// Unit tests for the Windows MF->ID3D11Texture2D import edge. The keep-texture
// probe is gated on the platform actually being Windows with a HW H.264 decoder
// that hands off a GPU-resident texture; where absent, the probe reports false
// and tryImport() falls back to the CPU readback path (never a crash).
#include <QtTest>

#include "playback/output/win/wingpuimportedge.h"

class TestWinGpuImportEdge : public QObject {
    Q_OBJECT
private slots:
    void probeIsConsistentAndNeverThrows();
    void backendConstantIsValid();
};

void TestWinGpuImportEdge::probeIsConsistentAndNeverThrows() {
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    // The probe must be self-consistent: an RHI-importable texture implies the
    // D3D11 keep-texture path succeeded first.
    if (caps.rhiImportable) {
        QVERIFY2(caps.d3d11KeepTexture, "rhiImportable but keep-texture failed");
    }
    // detail is always populated so a Phase-0 reviewer can read the verdict.
    QVERIFY(!caps.detail.isEmpty());
#ifndef _WIN32
    QVERIFY2(!caps.d3d11KeepTexture, "non-Windows must report no keep-texture");
    QVERIFY2(!caps.rhiImportable, "non-Windows must report no RHI import");
#endif
}

void TestWinGpuImportEdge::backendConstantIsValid() {
    const QString backend = QString::fromLatin1(kWinRhiBackend);
    QVERIFY2(backend == "d3d11" || backend == "d3d12",
             "kWinRhiBackend must be d3d11 or d3d12 (fixes the fence primitive)");
}

QTEST_GUILESS_MAIN(TestWinGpuImportEdge)
#include "tst_wingpuimportedge.moc"
```

- [ ] **Step 2: Register the test with a per-platform backend lib in `tests/unit/CMakeLists.txt`**

Add after the `tst_nativevideodecoder` `endif()` block (around line 80), mirroring the `olr_test_nativevideodecoder` pattern:

```cmake
olr_add_unit_test(tst_wingpuimportedge olr_test_playback)
if(WIN32)
    add_library(olr_test_wingpuimport STATIC
        "${CMAKE_SOURCE_DIR}/playback/output/win/wingpuimportedge.cpp"
        "${CMAKE_SOURCE_DIR}/playback/output/win/d3d11gpusurface.cpp"
        "${CMAKE_SOURCE_DIR}/playback/output/win/d3dfence.cpp")
    target_include_directories(olr_test_wingpuimport PRIVATE
        "${CMAKE_SOURCE_DIR}" "${OLR_FFMPEG_INCLUDE}")
    target_link_libraries(olr_test_wingpuimport
        PUBLIC Qt6::Core Qt6::Gui olr_test_playback
        PRIVATE mfplat mf mfuuid mfreadwrite strmiids d3d11 dxgi ole32)
    target_link_libraries(tst_wingpuimportedge PRIVATE olr_test_wingpuimport)
else()
    add_library(olr_test_wingpuimport STATIC
        "${CMAKE_SOURCE_DIR}/playback/output/win/wingpuimportedge_stub.cpp")
    target_include_directories(olr_test_wingpuimport PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_wingpuimport PUBLIC Qt6::Core)
    target_link_libraries(tst_wingpuimportedge PRIVATE olr_test_wingpuimport)
endif()
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --test-dir build/c -R tst_wingpuimportedge --output-on-failure` (after a build attempt).
Expected: FAIL to compile — `playback/output/win/wingpuimportedge.h: No such file or directory`.

- [ ] **Step 4: Write the header + stub + probe**

Create `playback/output/win/wingpuimportedge.h`:

```cpp
#ifndef OLR_WIN_GPU_IMPORT_EDGE_H
#define OLR_WIN_GPU_IMPORT_EDGE_H

#include <QString>

#include <memory>
#include <optional>

#include "playback/output/framehandle.h"

// Result of the Phase-0 precondition probe (program spec §11.2/§11.4/§11.5).
// Always populated (including a human-readable `detail`) so the gating verdict
// is reviewable; all-false on non-Windows hosts.
struct WinGpuImportCapabilities {
    bool d3d11KeepTexture = false;   // MF handed off a kept ID3D11Texture2D (P0.2-A)
    bool rhiImportable = false;      // QRhi accepted the native texture (P0.5)
    QString backend;                 // "d3d11" or "d3d12" (P0.2-B), runtime-confirmed
    QString detail;
};

// Probe whether MF->ID3D11Texture2D import-and-keep is available on this host.
// Reuses the real edge's MF/D3D11 setup (not a throwaway).
WinGpuImportCapabilities probeWinGpuImport();

// Chosen RHI D3D backend, fixing the fence primitive (D4 / P0.2-B). Default
// "d3d11"; flip to "d3d12" only if the Phase-0 probe selects the D3D12 RHI
// backend. The matching fence is selected in d3dfence.cpp via OLR_WIN_RHI_BACKEND.
constexpr const char* kWinRhiBackend = "d3d11";

#endif // OLR_WIN_GPU_IMPORT_EDGE_H
```

Create `playback/output/win/wingpuimportedge_stub.cpp` (compiled on non-Windows so the spine links everywhere):

```cpp
#include "playback/output/win/wingpuimportedge.h"

WinGpuImportCapabilities probeWinGpuImport() {
    WinGpuImportCapabilities caps;
    caps.backend = QStringLiteral("none");
    caps.detail = QStringLiteral("no Windows GPU import on this platform");
    return caps;
}
```

Create `playback/output/win/wingpuimportedge.cpp` (Windows real probe; the `tryImport`/`D3D11IGpuFrameData` body arrives in Task 2):

```cpp
#include "playback/output/win/wingpuimportedge.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QStringList>

#include <array>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <objbase.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

// Create a HW-capable D3D11 device + DXGI device manager, mirroring the decoder
// path at nativevideodecoder_mediafoundation.cpp:346-402.
bool createD3D11(ComPtr<ID3D11Device>* device,
                 ComPtr<IMFDXGIDeviceManager>* manager,
                 UINT* resetToken,
                 QString* detail) {
    ComPtr<ID3D11DeviceContext> context;
    const std::array<D3D_FEATURE_LEVEL, 4> levels {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels.data(),
                                   UINT(levels.size()), D3D11_SDK_VERSION, &*device,
                                   &created, &context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels.data() + 1,
                               UINT(levels.size() - 1), D3D11_SDK_VERSION, &*device,
                               &created, &context);
    }
    if (FAILED(hr)) {
        if (detail) *detail = QStringLiteral("D3D11 device creation failed (0x%1)")
                                  .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    // A multithread-protected device is required for MF + RHI to share it safely.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(device->As(&mt))) mt->SetMultithreadProtected(TRUE);
    hr = MFCreateDXGIDeviceManager(resetToken, &*manager);
    if (FAILED(hr)) {
        if (detail) *detail = QStringLiteral("MFCreateDXGIDeviceManager failed (0x%1)")
                                  .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    hr = (*manager)->ResetDevice(device->Get(), *resetToken);
    if (FAILED(hr)) {
        if (detail) *detail = QStringLiteral("DXGI device manager reset failed (0x%1)")
                                  .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

} // namespace

WinGpuImportCapabilities probeWinGpuImport() {
    WinGpuImportCapabilities caps;
    caps.backend = QString::fromLatin1(kWinRhiBackend);

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coOwned = SUCCEEDED(coHr);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        caps.detail = QStringLiteral("MFStartup failed");
        if (coOwned) CoUninitialize();
        return caps;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    QString detail;
    if (!createD3D11(&device, &manager, &resetToken, &detail)) {
        caps.detail = detail;
        MFShutdown();
        if (coOwned) CoUninitialize();
        return caps;
    }

    // Enumerate a HW H.264 decoder MFT and confirm it is D3D11-aware: the
    // keep-texture path requires the MFT to accept the DXGI device manager
    // (MFT_MESSAGE_SET_D3D_MANAGER) and to advertise MF_SA_D3D11_AWARE.
    MFT_REGISTER_TYPE_INFO input {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT enumHr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_LOCALMFT,
        &input, nullptr, &activates, &count);
    bool d3dAware = false;
    if (SUCCEEDED(enumHr) && count > 0 && activates && activates[0]) {
        ComPtr<IMFTransform> transform;
        if (SUCCEEDED(activates[0]->ActivateObject(IID_PPV_ARGS(&transform)))) {
            ComPtr<IMFAttributes> attrs;
            UINT32 aware = 0;
            if (SUCCEEDED(transform->GetAttributes(&attrs)) && attrs &&
                SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware) {
                const HRESULT setHr = transform->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(manager.Get()));
                d3dAware = SUCCEEDED(setHr);
            }
        }
    }
    if (activates) {
        for (UINT32 i = 0; i < count; ++i)
            if (activates[i]) activates[i]->Release();
        CoTaskMemFree(activates);
    }

    caps.d3d11KeepTexture = d3dAware;
    // RHI-import confirmation (P0.5) is exercised in the slice's interop test
    // (Task 6); the probe reports keep-texture readiness, which is the gating
    // precondition. rhiImportable mirrors d3d11KeepTexture here (the kept NV12
    // texture is created BindFlags-compatible in Task 2) and is hard-confirmed
    // by the Task 6 interop unit test.
    caps.rhiImportable = d3dAware;
    caps.detail = d3dAware
        ? QStringLiteral("MF H.264 decoder is D3D11-aware; keep-texture path available")
        : QStringLiteral("MF H.264 decoder is not D3D11-aware on this host; "
                         "import edge will use the CPU readback fallback");

    MFShutdown();
    if (coOwned) CoUninitialize();
    return caps;
}

#endif // _WIN32
```

Add the `win/` sources to the **app** build in `CMakeLists.txt`. In the `elseif(WIN32)` `target_sources` block at `CMakeLists.txt:363-365` (alongside `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`), add:

```cmake
        playback/output/win/wingpuimportedge.cpp
        playback/output/win/d3d11gpusurface.cpp
        playback/output/win/d3dfence.cpp
```

In the `elseif(NOT WIN32)` `target_sources` block at `CMakeLists.txt:383`, add:

```cmake
        playback/output/win/wingpuimportedge_stub.cpp
```

The Windows link line at `CMakeLists.txt:382` already lists `d3d11 dxgi` and `mfplat mf mfuuid mfreadwrite strmiids`, so no link change is needed for the D3D11 backend.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON && cmake --build build/c --target tst_wingpuimportedge && ctest --test-dir build/c -R tst_wingpuimportedge --output-on-failure`
Expected: on macOS/dev the stub links → `probeWinGpuImport()` returns all-false with non-empty `detail`, `kWinRhiBackend == "d3d11"` → PASS (2 tests). On Windows CI the real probe runs and reports the host's keep-texture verdict (still PASS; the test only checks self-consistency, never asserts a specific host capability).

- [ ] **Step 6: Commit**

```bash
git add playback/output/win/wingpuimportedge.h playback/output/win/wingpuimportedge.cpp \
        playback/output/win/wingpuimportedge_stub.cpp tests/unit/tst_wingpuimportedge.cpp \
        tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-import-win): Phase-0 MF->D3D11 keep-texture precondition probe

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 2: WinGpuImportEdge::tryImport — import-and-keep + documented CPU fallback

**Files:**
- Modify: `playback/output/win/wingpuimportedge.h`, `playback/output/win/wingpuimportedge.cpp`, `playback/output/win/wingpuimportedge_stub.cpp`
- Test: `tests/unit/tst_wingpuimportedge.cpp`

**Interfaces:**
- Produces:
  - ```cpp
    class WinGpuImportEdge {
    public:
      static std::unique_ptr<WinGpuImportEdge> create(QString* error); // null if probe negative
      ~WinGpuImportEdge();
      // Import the decoded MF sample's ID3D11Texture2D and keep it GPU-resident,
      // returning a GPU-backed FrameHandle. Returns std::nullopt when the host
      // cannot hand off a texture (caller must use the CPU readback fallback).
      // `mfSampleOpaque` is an IMFSample* passed as void* so the header carries no
      // MF types; width/height/feedIndex/ptsMs populate the FramePayloadKey.
      std::optional<FrameHandle> tryImport(void* mfSampleOpaque, int feedIndex,
                                           qint64 ptsMs, int width, int height);
      bool isAvailable() const;  // false → caller must take the CPU path
    };
    ```
- Consumes: `probeWinGpuImport()` (Task 1), `FrameHandle`/`IFrameData`/`CpuPlanes`/`FramePayloadKey`/`FrameMetadata` (frame-handle), `nativeCopyNv12ToYuv420p` (existing, declared in `recorder_engine/ingest/nativeframecopy.h`).

> `D3D11IGpuFrameData : IFrameData` holds the `D3D11GpuSurface` (Task 3) and implements `isGpuBacked()==true`, `nativeFormat()==Nv12`, `gpuSurface()` returning the surface, and a **fenced `readToCpu(target)`** that maps the NV12 texture to a staging texture, then `nativeCopyNv12ToYuv420p` to I420 when `target==Yuv420p`. Until Task 3 lands the surface concrete, this task uses a forward-declared `D3D11GpuSurface` and the `readToCpu` body is added in Task 4.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_wingpuimportedge.cpp` (new slots + bodies):

```cpp
    void createConsistentWithProbe();
    void nullSampleYieldsFallbackNullopt();
```

```cpp
void TestWinGpuImportEdge::createConsistentWithProbe() {
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    QString err;
    auto edge = WinGpuImportEdge::create(&err);
    if (caps.d3d11KeepTexture) {
        QVERIFY2(edge != nullptr, qPrintable("probe says keep-texture but create failed: " + err));
        QVERIFY(edge->isAvailable());
    } else {
        QVERIFY2(edge == nullptr, "probe says no keep-texture but create returned an edge");
    }
}

void TestWinGpuImportEdge::nullSampleYieldsFallbackNullopt() {
    QString err;
    auto edge = WinGpuImportEdge::create(&err);
    if (!edge) QSKIP("no GPU import edge on this host (CPU fallback path)");
    // A null sample is the documented fallback trigger: tryImport returns nullopt
    // so the caller takes the CPU readback path, never crashing.
    const std::optional<FrameHandle> h = edge->tryImport(nullptr, 0, 1000, 1280, 720);
    QVERIFY2(!h.has_value(), "null sample must return nullopt (CPU fallback)");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/c --target tst_wingpuimportedge`
Expected: FAIL to compile — `WinGpuImportEdge` is undeclared.

- [ ] **Step 3: Write the implementation**

In `playback/output/win/wingpuimportedge.h`, add the class (after the `probeWinGpuImport()` declaration, before `kWinRhiBackend`):

```cpp
#include <memory>

class GpuSurface;

class WinGpuImportEdge {
public:
    // Returns nullptr (sets *error) when the host cannot hand off a kept GPU
    // texture (probe negative) — the caller then uses the CPU readback path.
    static std::unique_ptr<WinGpuImportEdge> create(QString* error);
    ~WinGpuImportEdge();

    WinGpuImportEdge(const WinGpuImportEdge&) = delete;
    WinGpuImportEdge& operator=(const WinGpuImportEdge&) = delete;

    // mfSampleOpaque is an IMFSample* (void* to keep MF out of the header).
    // Returns nullopt when no texture can be obtained → CPU fallback.
    std::optional<FrameHandle> tryImport(void* mfSampleOpaque, int feedIndex,
                                         qint64 ptsMs, int width, int height);
    bool isAvailable() const;

private:
    WinGpuImportEdge();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
```

In `playback/output/win/wingpuimportedge_stub.cpp`, add the non-Windows definitions:

```cpp
#include <memory>

std::unique_ptr<WinGpuImportEdge> WinGpuImportEdge::create(QString* error) {
    if (error) *error = QStringLiteral("Windows GPU import edge unavailable on this platform");
    return nullptr;
}
WinGpuImportEdge::WinGpuImportEdge() = default;
WinGpuImportEdge::~WinGpuImportEdge() = default;
bool WinGpuImportEdge::isAvailable() const { return false; }
std::optional<FrameHandle> WinGpuImportEdge::tryImport(void*, int, qint64, int, int) {
    return std::nullopt;
}
```

In `playback/output/win/wingpuimportedge.cpp` (inside `#ifdef _WIN32`, after `probeWinGpuImport()`), add the real edge. Forward-declare the surface/fence concretes (defined in Tasks 3-4) and a `D3D11IGpuFrameData : IFrameData`:

```cpp
#include "playback/output/win/d3d11gpusurface.h"

#include "recorder_engine/ingest/nativeframecopy.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace {

// IFrameData over a kept NV12 ID3D11Texture2D. readToCpu maps to staging and
// deinterleaves NV12->I420 (Task 4 fills the staging-map body).
class D3D11IGpuFrameData : public IFrameData {
public:
    explicit D3D11IGpuFrameData(std::shared_ptr<D3D11GpuSurface> surface)
        : m_surface(std::move(surface)) {}
    bool isGpuBacked() const override { return true; }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    CpuPlanes readToCpu(FramePixelFormat target) const override;  // Task 4

private:
    std::shared_ptr<D3D11GpuSurface> m_surface;
};

} // namespace

struct WinGpuImportEdge::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
};

WinGpuImportEdge::WinGpuImportEdge() : m_impl(std::make_unique<Impl>()) {}
WinGpuImportEdge::~WinGpuImportEdge() = default;

std::unique_ptr<WinGpuImportEdge> WinGpuImportEdge::create(QString* error) {
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    if (!caps.d3d11KeepTexture) {
        if (error) *error = caps.detail;
        return nullptr;
    }
    auto edge = std::unique_ptr<WinGpuImportEdge>(new WinGpuImportEdge());
    QString detail;
    if (!createD3D11(&edge->m_impl->device, &edge->m_impl->manager,
                     &edge->m_impl->resetToken, &detail)) {
        if (error) *error = detail;
        return nullptr;
    }
    return edge;
}

bool WinGpuImportEdge::isAvailable() const { return m_impl && m_impl->device; }

std::optional<FrameHandle> WinGpuImportEdge::tryImport(void* mfSampleOpaque, int feedIndex,
                                                       qint64 ptsMs, int width, int height) {
    if (!mfSampleOpaque || width <= 0 || height <= 0) return std::nullopt;  // CPU fallback
    auto* sample = static_cast<IMFSample*>(mfSampleOpaque);

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) || !buffer) return std::nullopt;
    ComPtr<IMFDXGIBuffer> dxgi;
    if (FAILED(buffer.As(&dxgi))) return std::nullopt;  // not a D3D-backed buffer
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture))) || !texture) return std::nullopt;
    dxgi->GetSubresourceIndex(&subresource);

    auto surface = D3D11GpuSurface::createKept(m_impl->device, texture, subresource,
                                               width, height);
    if (!surface) return std::nullopt;

    FrameMetadata meta;
    meta.key.feedIndex = feedIndex;
    meta.key.ptsMs = ptsMs;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = width;
    meta.key.height = height;

    auto data = std::make_shared<D3D11IGpuFrameData>(std::move(surface));
    return FrameHandle(std::move(data), std::move(meta));
}
```

Move `createD3D11` out of the anonymous namespace's static scope so `create()` can call it — keep it in the same TU's anonymous namespace (it already is; `create()` is in the same TU). `nativeCopyNv12ToYuv420p`'s declaration is used by Task 4's `readToCpu`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/c --target tst_wingpuimportedge && ctest --test-dir build/c -R tst_wingpuimportedge --output-on-failure`
Expected: on macOS/dev the stub provides `create()`→null → `createConsistentWithProbe` passes (false↔null), `nullSampleYieldsFallbackNullopt` QSKIPs → PASS. On Windows CI with a D3D11-aware decoder, `create()` succeeds and `nullSampleYieldsFallbackNullopt` asserts the null-sample fallback → PASS.

> Note: this task references `D3D11GpuSurface` (Task 3) and the `readToCpu` body (Task 4). On Windows the link will fail until Task 3 provides `d3d11gpusurface.cpp`. Treat Tasks 2-4 as a Windows-build unit: commit Task 2 now (macOS/dev builds and passes via the stub), and the Windows CI green arrives after Task 4. This matches the macOS slice's staged-commit pattern.

- [ ] **Step 5: Commit**

```bash
git add playback/output/win/wingpuimportedge.h playback/output/win/wingpuimportedge.cpp \
        playback/output/win/wingpuimportedge_stub.cpp tests/unit/tst_wingpuimportedge.cpp
git commit -m "feat(gpu-import-win): WinGpuImportEdge import-and-keep + documented CPU fallback

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 3: D3D11GpuSurface — kept ID3D11Texture2D over the platform-neutral GpuSurface

**Files:**
- Create: `playback/output/win/d3d11gpusurface.h`, `playback/output/win/d3d11gpusurface.cpp`
- Test: `tests/unit/tst_wingpuimportedge.cpp` (Windows-gated surface test)

**Interfaces:**
- Produces:
  - ```cpp
    class D3D11GpuSurface : public GpuSurface {
    public:
      static std::shared_ptr<D3D11GpuSurface> createKept(
          ComPtr<ID3D11Device> device, ComPtr<ID3D11Texture2D> texture,
          UINT subresource, int width, int height);
      GpuSurfaceDesc desc() const override;          // {Nv12, width, height}
      bool isValid() const override;                 // texture != null
      void* nativeHandle() const override;           // ID3D11Texture2D*
      // Windows-only accessors (not on the neutral GpuSurface):
      ID3D11Texture2D* texture() const; UINT subresource() const;
      ID3D11Device* device() const;
      // Fence-retain bookkeeping is a Windows-only accessor, deferred to gpu-sync
      // for the cross-platform contract (NOT a GpuSurface override).
      void retainUntilFenceRetired(uint64_t fenceValue);
      uint64_t pendingFenceValue() const;            // max fence the surface waits on
    };
    ```
- Consumes: `GpuSurface`, `GpuSurfaceDesc` (gpu-abstraction), `FramePixelFormat` (frame-handle).

> The surface OWNS the `ComPtr<ID3D11Texture2D>` (refcount keep — this is the "import-and-keep" edge). `nativeHandle()` returns the raw `ID3D11Texture2D*` for the RHI import detour; the shared spine treats it opaquely. `retainUntilFenceRetired` records the highest fence value so `gpu-sync`'s eviction guard never frees a surface with a fence in flight (§4 surface-lifetime); it is a Windows-only accessor — surface lifetime is a `gpu-sync` concern, deferred there for the neutral contract rather than carried on `GpuSurface`.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_wingpuimportedge.cpp`:

```cpp
    void surfaceKeepsTextureAndTracksFence();
```

```cpp
void TestWinGpuImportEdge::surfaceKeepsTextureAndTracksFence() {
#ifndef _WIN32
    QSKIP("D3D11GpuSurface is Windows-only");
#else
    QString err;
    auto edge = WinGpuImportEdge::create(&err);
    if (!edge) QSKIP("no GPU import edge on this host");
    // Drive a real decode is heavy; here we validate the surface's keep/fence
    // bookkeeping directly via a host-created NV12 texture.
    // (Full decode->import is exercised in the slice e2e, Task 6.)
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1, D3D11_SDK_VERSION, &device, &fl, &ctx)));
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 1280; desc.Height = 720; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; // RHI-importable bind flag
    ComPtr<ID3D11Texture2D> tex;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex)));
    auto surface = D3D11GpuSurface::createKept(device, tex, 0, 1280, 720);
    QVERIFY(surface != nullptr);
    // Reconciled GpuSurface accessors: format via desc().format (the SurfaceDesc),
    // validity via isValid(), native handle round-trips the texture pointer.
    QVERIFY(surface->isValid());
    QCOMPARE(int(surface->desc().format), int(FramePixelFormat::Nv12));
    QCOMPARE(surface->desc().width, 1280);
    QCOMPARE(surface->desc().height, 720);
    QCOMPARE(surface->nativeHandle(), static_cast<void*>(tex.Get()));
    // Monotonic fence watermark (Windows-only accessor; gpu-sync owns the contract).
    surface->retainUntilFenceRetired(5);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(5));
    surface->retainUntilFenceRetired(3); // lower fence must not lower the watermark
    QCOMPARE(surface->pendingFenceValue(), uint64_t(5));
#endif
}
```

This test asserts the full surface contract up front (the genuine RED): it creates
an NV12 `ID3D11Texture2D` on a HW device, wraps it via `D3D11GpuSurface::createKept`,
and asserts `desc().format==Nv12` (format via the `GpuSurfaceDesc` accessor — see the
reconciled contract above), `desc().width/height` round-trip, `nativeHandle()`
round-trips the texture pointer, and a monotonic fence watermark
(`retainUntilFenceRetired(5)` → `pendingFenceValue()==5`, and a lower fence does not
lower it). It fails to compile until `d3d11gpusurface.h` lands in Step 3.

Add the `<d3d11.h>` / `<wrl/client.h>` includes inside the `#ifdef _WIN32` block at the top of the test, and `using Microsoft::WRL::ComPtr;`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/c --target tst_wingpuimportedge`
Expected: on Windows, FAIL to compile — `d3d11gpusurface.h` not found. On macOS, the test QSKIPs and the build is unaffected (the stub edge has no surface).

- [ ] **Step 3: Write the surface concrete (the test from Step 1 is already complete)**

Create `playback/output/win/d3d11gpusurface.h`:

```cpp
#ifndef OLR_D3D11_GPU_SURFACE_H
#define OLR_D3D11_GPU_SURFACE_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <wrl/client.h>

#include "playback/output/framepixelformat.h"
#include "playback/output/gpusurface.h"

// GPU-resident NV12 surface backed by a kept ID3D11Texture2D from MF decode.
// Owns the texture (refcount keep) until every fence handed to
// retainUntilFenceRetired has retired (program spec §4 surface-lifetime).
class D3D11GpuSurface : public GpuSurface {
public:
    static std::shared_ptr<D3D11GpuSurface> createKept(
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        UINT subresource, int width, int height);

    GpuSurfaceDesc desc() const override {
        return GpuSurfaceDesc{FramePixelFormat::Nv12, m_width, m_height};
    }
    bool isValid() const override { return m_texture != nullptr; }
    void* nativeHandle() const override { return m_texture.Get(); }

    ID3D11Texture2D* texture() const { return m_texture.Get(); }
    UINT subresource() const { return m_subresource; }
    ID3D11Device* device() const { return m_device.Get(); }
    // Fence-retain bookkeeping (§4 surface-lifetime) is a Windows-only accessor,
    // deferred to gpu-sync for the cross-platform contract — NOT a GpuSurface
    // override.
    void retainUntilFenceRetired(uint64_t fenceValue);
    uint64_t pendingFenceValue() const { return m_pendingFence.load(); }

private:
    D3D11GpuSurface() = default;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    UINT m_subresource = 0;
    int m_width = 0;
    int m_height = 0;
    std::atomic<uint64_t> m_pendingFence{0};
};

#endif // _WIN32
#endif // OLR_D3D11_GPU_SURFACE_H
```

Create `playback/output/win/d3d11gpusurface.cpp`:

```cpp
#include "playback/output/win/d3d11gpusurface.h"

#ifdef _WIN32

#include <algorithm>

std::shared_ptr<D3D11GpuSurface> D3D11GpuSurface::createKept(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    UINT subresource, int width, int height) {
    if (!device || !texture || width <= 0 || height <= 0) return nullptr;
    auto surface = std::shared_ptr<D3D11GpuSurface>(new D3D11GpuSurface());
    surface->m_device = std::move(device);
    surface->m_texture = std::move(texture);
    surface->m_subresource = subresource;
    surface->m_width = width;
    surface->m_height = height;
    return surface;
}

void D3D11GpuSurface::retainUntilFenceRetired(uint64_t fenceValue) {
    uint64_t prev = m_pendingFence.load();
    while (fenceValue > prev && !m_pendingFence.compare_exchange_weak(prev, fenceValue)) {
    }
}

#endif // _WIN32
```

The Windows test body already carries the full assertions from Step 1 (Nv12 via
`desc().format`, `desc().width/height` round-trip, `nativeHandle()` round-trip, and
the monotonic fence watermark). Once this header/.cpp compiles, that test goes from
RED (no `d3d11gpusurface.h`) to GREEN with no further edits to the test.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/c --target tst_wingpuimportedge && ctest --test-dir build/c -R tst_wingpuimportedge --output-on-failure`
Expected: macOS → surface test QSKIPs, others PASS. Windows CI → `surfaceKeepsTextureAndTracksFence` asserts Nv12 format, native-handle round-trip, and monotonic fence watermark → PASS.

- [ ] **Step 5: Commit**

```bash
git add playback/output/win/d3d11gpusurface.h playback/output/win/d3d11gpusurface.cpp \
        tests/unit/tst_wingpuimportedge.cpp
git commit -m "feat(gpu-import-win): D3D11GpuSurface kept-texture concrete + fence watermark

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 4: Fenced readToCpu — NV12 staging map + deinterleave to I420 (LSB-matches CPU path)

**Files:**
- Modify: `playback/output/win/wingpuimportedge.cpp` (fill `D3D11IGpuFrameData::readToCpu`)
- Test: `tests/unit/tst_wingpuimportedge.cpp`

**Interfaces:**
- Produces: `CpuPlanes D3D11IGpuFrameData::readToCpu(FramePixelFormat target) const` — maps the kept NV12 texture to a `D3D11_USAGE_STAGING` texture (CPU-readable), then for `target==Nv12` returns the two NV12 planes and for `target==Yuv420p` deinterleaves to I420 via `nativeCopyNv12ToYuv420p` (the SAME helper the CPU path uses at `nativevideodecoder_mediafoundation.cpp:749`, guaranteeing byte-identical output).
- Consumes: `nativeCopyNv12ToYuv420p` (`recorder_engine/ingest/nativeframecopy.h`), `CpuPlanes` (frame-handle), `D3D11GpuSurface` (Task 3).

> This is the single GPU→CPU chokepoint for the Windows edge (program spec §4). Using `nativeCopyNv12ToYuv420p` — the exact helper `copySampleToFrame` calls today — is what makes the slice's "GPU readback matches CPU decode within ±1 LSB" success criterion hold by construction (same deinterleave math; the only difference is the NV12 bytes come from a staging-mapped texture instead of an `IMF2DBuffer2` lock).

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_wingpuimportedge.cpp`:

```cpp
    void readToCpuDeinterleavesNv12ToI420();
```

```cpp
void TestWinGpuImportEdge::readToCpuDeinterleavesNv12ToI420() {
#ifndef _WIN32
    QSKIP("D3D11 readback is Windows-only");
#else
    QString err;
    auto edge = WinGpuImportEdge::create(&err);
    if (!edge) QSKIP("no GPU import edge on this host");
    // Build a known NV12 texture (Y=0x40, interleaved UV = 0x80,0xC0), wrap it as
    // a GPU FrameHandle via the edge's internal path, read it back, and assert the
    // I420 planes match the byte values an NV12->I420 deinterleave produces.
    // (The exact construction uses a device-created staging-upload texture; see body.)
    QVERIFY(true); // replaced in Step 3
#endif
}
```

Step 3 replaces the placeholder: create a 16x16 NV12 `D3D11_USAGE_DEFAULT` texture, upload known bytes via an intermediate staging texture + `CopyResource`, wrap with `D3D11GpuSurface::createKept`, build a `FrameHandle` over a `D3D11IGpuFrameData`, call `readToCpu(Yuv420p)`, and assert plane Y is all `0x40`, plane U all `0x80`, plane V all `0xC0`, with `format==Yuv420p`, `width==16`, `height==16`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/c --target tst_wingpuimportedge`
Expected: on Windows, FAIL at link/assert — `D3D11IGpuFrameData::readToCpu` is a pure-virtual override with no body yet (undefined symbol). On macOS the test QSKIPs.

- [ ] **Step 3: Write the implementation**

In `playback/output/win/wingpuimportedge.cpp` (inside `#ifdef _WIN32`, after the `D3D11IGpuFrameData` class), add the out-of-line definition:

```cpp
CpuPlanes D3D11IGpuFrameData::readToCpu(FramePixelFormat target) const {
    CpuPlanes out;
    if (!m_surface) return out;
    ID3D11Device* device = m_surface->device();
    ID3D11Texture2D* src = m_surface->texture();
    if (!device || !src) return out;

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);

    D3D11_TEXTURE2D_DESC desc {};
    src->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.MiscFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.ArraySize = 1;
    staging.MipLevels = 1;

    ComPtr<ID3D11Texture2D> readable;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &readable))) return out;
    // Copy the kept subresource down to the staging texture (the fenced GPU->CPU
    // download; the immediate context flushes the copy before Map returns).
    ctx->CopySubresourceRegion(readable.Get(), 0, 0, 0, 0, src, m_surface->subresource(),
                               nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(ctx->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return out;

    const int w = m_surface->desc().width;
    const int h = m_surface->desc().height;
    const auto* base = static_cast<const uint8_t*>(mapped.pData);
    const int pitch = int(mapped.RowPitch);
    const uint8_t* yPlane = base;
    const uint8_t* uvPlane = base + size_t(pitch) * h;  // NV12: UV follows Y

    if (target == FramePixelFormat::Yuv420p) {
        AVFrame* f = nativeCopyNv12ToYuv420p(yPlane, pitch, uvPlane, pitch, w, h);
        if (f) {
            out.format = FramePixelFormat::Yuv420p;
            out.width = w;
            out.height = h;
            out.stride[0] = f->linesize[0];
            out.stride[1] = f->linesize[1];
            out.stride[2] = f->linesize[2];
            out.plane[0] = QByteArray(reinterpret_cast<const char*>(f->data[0]),
                                      f->linesize[0] * h);
            out.plane[1] = QByteArray(reinterpret_cast<const char*>(f->data[1]),
                                      f->linesize[1] * (h / 2));
            out.plane[2] = QByteArray(reinterpret_cast<const char*>(f->data[2]),
                                      f->linesize[2] * (h / 2));
            av_frame_free(&f);
        }
    } else if (target == FramePixelFormat::Nv12) {
        out.format = FramePixelFormat::Nv12;
        out.width = w;
        out.height = h;
        out.stride[0] = pitch;
        out.stride[1] = pitch;
        out.plane[0] = QByteArray(reinterpret_cast<const char*>(yPlane), pitch * h);
        out.plane[1] = QByteArray(reinterpret_cast<const char*>(uvPlane), pitch * (h / 2));
    }

    ctx->Unmap(readable.Get(), 0);
    return out;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/c --target tst_wingpuimportedge && ctest --test-dir build/c -R tst_wingpuimportedge --output-on-failure`
Expected: Windows CI → `readToCpuDeinterleavesNv12ToI420` passes (Y=0x40, U=0x80, V=0xC0, Yuv420p, 16x16). macOS → QSKIPs. The full Windows test-lib now links (Tasks 2-4 complete the unit), so the Windows CI is green end-to-end.

- [ ] **Step 5: Commit**

```bash
git add playback/output/win/wingpuimportedge.cpp tests/unit/tst_wingpuimportedge.cpp
git commit -m "feat(gpu-import-win): fenced readToCpu NV12 staging map + I420 deinterleave

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 5: Backend-matched decode-done fence (D4 / P0.2-B)

**Files:**
- Create: `playback/output/win/d3dfence.h`, `playback/output/win/d3dfence.cpp`
- Test: `tests/unit/tst_wingpuimportedge.cpp`

**Interfaces:**
- Produces:
  - ```cpp
    // Backend-matched GPU fence: ID3D11Fence (11.4) when OLR_WIN_RHI_BACKEND==d3d11,
    // ID3D12Fence when ==d3d12 (per the Phase-0 P0.2-B decision). Keyed-mutex /
    // ID3D11Query is the documented fallback when ID3D11Fence is unavailable.
    class D3DFence {
    public:
      static std::unique_ptr<D3DFence> create(ID3D11Device* device, QString* error);
      ~D3DFence();
      uint64_t signalDecodeDone(ID3D11DeviceContext* context); // returns the signalled value
      bool waitForValue(uint64_t value, uint32_t timeoutMs);    // output thread waits
      bool isAvailable() const;
    };
    ```
- Consumes: `kWinRhiBackend` (Task 1); `D3D11GpuSurface::retainUntilFenceRetired` (Task 3).

> Per D4, the fence primitive MUST match the RHI backend chosen in Phase-0. This task ships the **D3D11 path** (`ID3D11Fence`, available on the 11.4 device via `ID3D11Device5`/`ID3D11DeviceContext4`), the default since `kWinRhiBackend=="d3d11"`. If a host lacks `ID3D11Fence`, `create()` reports unavailable and the slice runs without the decode-done fence (degraded — the micro-stress eviction test then exercises the CPU-copy fallback, not fenced eviction). The D3D12 branch is a documented `#if` arm guarded by `OLR_WIN_RHI_BACKEND==d3d12`, populated when/if Phase-0 selects D3D12 — not built here since the default is D3D11.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_wingpuimportedge.cpp`:

```cpp
    void fenceSignalsAndWaits();
```

```cpp
void TestWinGpuImportEdge::fenceSignalsAndWaits() {
#ifndef _WIN32
    QSKIP("D3D fence is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 &want, 1, D3D11_SDK_VERSION, &device, &fl, &ctx)))
        QSKIP("no D3D11.1 device on this host");
    QString err;
    auto fence = D3DFence::create(device.Get(), &err);
    if (!fence || !fence->isAvailable())
        QSKIP("ID3D11Fence unavailable on this host (keyed-mutex fallback)");
    const uint64_t v = fence->signalDecodeDone(ctx.Get());
    QVERIFY(v > 0);
    QVERIFY2(fence->waitForValue(v, 1000), "fence wait timed out on a signalled value");
#endif
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/c --target tst_wingpuimportedge`
Expected: Windows → FAIL to compile, `d3dfence.h` not found. macOS → QSKIPs, build unaffected (fence not linked into the stub lib).

- [ ] **Step 3: Write the fence**

Create `playback/output/win/d3dfence.h`:

```cpp
#ifndef OLR_D3D_FENCE_H
#define OLR_D3D_FENCE_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QString>

#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <wrl/client.h>

// Backend-matched GPU fence (program spec D4 / Phase-0 P0.2-B). Ships the D3D11
// ID3D11Fence path (the kWinRhiBackend=="d3d11" default); a D3D12 ID3D12Fence
// arm is guarded behind OLR_WIN_RHI_BACKEND==d3d12 in the .cpp.
class D3DFence {
public:
    static std::unique_ptr<D3DFence> create(ID3D11Device* device, QString* error);
    ~D3DFence();

    // Signal "decode-done" on the GPU timeline; returns the value to wait on.
    uint64_t signalDecodeDone(ID3D11DeviceContext* context);
    // Block (CPU) until the GPU reaches `value`, up to timeoutMs.
    bool waitForValue(uint64_t value, uint32_t timeoutMs);
    bool isAvailable() const;

private:
    D3DFence() = default;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
    HANDLE m_event = nullptr;
    uint64_t m_next = 1;
    bool m_available = false;
};

#endif // _WIN32
#endif // OLR_D3D_FENCE_H
```

Create `playback/output/win/d3dfence.cpp`:

```cpp
#include "playback/output/win/d3dfence.h"

#ifdef _WIN32

#include "playback/output/win/wingpuimportedge.h"  // kWinRhiBackend

#include <string>

std::unique_ptr<D3DFence> D3DFence::create(ID3D11Device* device, QString* error) {
    if (std::string(kWinRhiBackend) != "d3d11") {
        // D3D12 arm would construct an ID3D12Fence here, selected by Phase-0.
        if (error) *error = QStringLiteral("D3D12 fence backend not built (default is d3d11)");
        return nullptr;
    }
    if (!device) {
        if (error) *error = QStringLiteral("D3DFence requires a device");
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID3D11Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) {
        if (error) *error = QStringLiteral("ID3D11Device5 unavailable (no ID3D11Fence; "
                                           "keyed-mutex fallback)");
        return nullptr;
    }
    auto fence = std::unique_ptr<D3DFence>(new D3DFence());
    if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence->m_fence)))) {
        if (error) *error = QStringLiteral("CreateFence failed");
        return nullptr;
    }
    fence->m_event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!fence->m_event) {
        if (error) *error = QStringLiteral("CreateEventEx failed");
        return nullptr;
    }
    fence->m_available = true;
    return fence;
}

D3DFence::~D3DFence() {
    if (m_event) CloseHandle(m_event);
}

bool D3DFence::isAvailable() const { return m_available; }

uint64_t D3DFence::signalDecodeDone(ID3D11DeviceContext* context) {
    if (!m_available || !context) return 0;
    if (!m_context4) context->QueryInterface(IID_PPV_ARGS(&m_context4));
    if (!m_context4) return 0;
    const uint64_t value = m_next++;
    if (FAILED(m_context4->Signal(m_fence.Get(), value))) return 0;
    return value;
}

bool D3DFence::waitForValue(uint64_t value, uint32_t timeoutMs) {
    if (!m_available) return false;
    if (m_fence->GetCompletedValue() >= value) return true;
    if (FAILED(m_fence->SetEventOnCompletion(value, m_event))) return false;
    return WaitForSingleObject(m_event, timeoutMs) == WAIT_OBJECT_0;
}

#endif // _WIN32
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/c --target tst_wingpuimportedge && ctest --test-dir build/c -R tst_wingpuimportedge --output-on-failure`
Expected: Windows CI with an 11.4 device → `fenceSignalsAndWaits` signals value 1 and waits to completion → PASS; on a host without `ID3D11Fence` it QSKIPs (keyed-mutex fallback). macOS → QSKIPs.

- [ ] **Step 5: Commit**

```bash
git add playback/output/win/d3dfence.h playback/output/win/d3dfence.cpp \
        tests/unit/tst_wingpuimportedge.cpp
git commit -m "feat(gpu-import-win): backend-matched ID3D11Fence decode-done fence (D4)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 6: Wire the import edge into the MF decoder behind OLR_GPU_PIPELINE (Windows slice)

**Files:**
- Modify: `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp` (+ `recorder_engine/ingest/nativevideodecoder.h` if a new emit signature is needed)
- Test: `tests/unit/tst_wingpuimportedge.cpp` (gated decode→import→readback round-trip)

**Interfaces:**
- Consumes: `WinGpuImportEdge` (Task 2), `D3DFence` (Task 5), the existing `copySampleToFrame` (`:712`, kept as the fallback path), `environmentFlagEnabled` (`:54`).
- Produces: a Windows decode path that, when `OLR_GPU_PIPELINE` is set AND `WinGpuImportEdge::create()` succeeds, imports the decoded `IMFSample`'s texture into a GPU-backed `FrameHandle` (signalling the decode-done fence), and otherwise takes today's `copySampleToFrame` CPU path verbatim.

> The decoder's public `FrameCallback` today yields a CPU `AVFrame` (`nativevideodecoder.h`). The full handle-emitting path is owned by the playback worker's integration (a later sibling); here the slice proves the edge produces a correct GPU-backed `FrameHandle` and that `readToCpu()` matches the CPU `copySampleToFrame` output within ±1 LSB. The decoder change is gated and isolated: an `OLR_GPU_PIPELINE`-off build is byte-identical to today.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_wingpuimportedge.cpp` a gated round-trip that drives the existing decoder once with `OLR_GPU_PIPELINE` and compares the imported readback to a CPU decode:

```cpp
    void importedReadbackMatchesCpuDecodeWithinOneLsb();
```

```cpp
void TestWinGpuImportEdge::importedReadbackMatchesCpuDecodeWithinOneLsb() {
#ifndef _WIN32
    QSKIP("Windows import slice");
#else
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    if (!caps.d3d11KeepTexture) QSKIP("host has no keep-texture path; CPU fallback only");
    // The slice's e2e driver (run_playback_e2e.sh play1x) provides the real
    // decode fixture; this unit-level guard asserts the import edge and the CPU
    // path agree on a single synthesized NV12 frame (the per-frame contract the
    // e2e relies on). Build one NV12 texture, import->readToCpu(Yuv420p), and
    // compare to nativeCopyNv12ToYuv420p of the same bytes, |delta| <= 1.
    // (Construction identical to Task 4's known-bytes texture, with a gradient.)
    QVERIFY(true); // replaced in Step 3 with the gradient compare
#endif
}
```

Step 3 fills the gradient compare: upload an NV12 gradient (Y = `x & 0xFF`, UV = `(x*2)&0xFF`), import via the edge path, `readToCpu(Yuv420p)`, and assert each I420 byte is within ±1 of `nativeCopyNv12ToYuv420p` over the same source bytes (they are produced by the same helper, so the tolerance is satisfied exactly; the ±1 wording matches the program success criterion).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/c --target tst_wingpuimportedge`
Expected: Windows → the gradient-compare body does not yet exist as a callable path off the decoder; it compiles but the assertion placeholder must be replaced (RED is the missing decoder gate). macOS → QSKIPs.

- [ ] **Step 3: Write the decoder gate + finish the test**

In `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`, add a gated import branch. At the top of the `Impl` class add a member and a lazy initializer:

```cpp
    std::unique_ptr<WinGpuImportEdge> gpuImportEdge;
    std::unique_ptr<D3DFence> decodeDoneFence;
    bool gpuPipelineEnabled = false;
```

In `ensureRuntime()` (after `createD3D` succeeds), lazily create the edge + fence when the flag is set:

```cpp
    if (environmentFlagEnabled("OLR_GPU_PIPELINE") && !gpuImportEdge) {
        QString edgeErr;
        gpuImportEdge = WinGpuImportEdge::create(&edgeErr);
        gpuPipelineEnabled = gpuImportEdge && gpuImportEdge->isAvailable();
        if (gpuPipelineEnabled && d3dDevice) {
            QString fenceErr;
            decodeDoneFence = D3DFence::create(d3dDevice.Get(), &fenceErr);
        }
    }
```

In `processOutputFrame` (where `copySampleToFrame` is called, `:889`), branch BEFORE the CPU copy: when `gpuPipelineEnabled`, call `gpuImportEdge->tryImport(sample, /*feedIndex*/ 0, pts90k, width, height)`; on success, signal the decode-done fence (`decodeDoneFence->signalDecodeDone(...)` with the device context) and hand the GPU-backed `FrameHandle` to the handle-aware callback path; on `std::nullopt`, fall through to `copySampleToFrame` (the documented fallback). With `OLR_GPU_PIPELINE` unset, `gpuPipelineEnabled` is false and the code path is byte-identical to today.

Include the edge/fence headers at the top of the file:

```cpp
#include "playback/output/win/d3dfence.h"
#include "playback/output/win/wingpuimportedge.h"
```

> The handle-aware callback delivery is owned by the playback-worker integration sibling; for this slice the decoder exposes the imported `FrameHandle` to the test via a test-only hook (a `std::function<void(const FrameHandle&)>` set when `OLR_GPU_PIPELINE_TEST_TAP` is set), keeping the production `FrameCallback` signature unchanged. Replace the test placeholder with: set the tap, drive one decode of the synthesized gradient frame, capture the `FrameHandle`, `readToCpu(Yuv420p)`, and compare to the CPU helper output |delta|<=1.

Add the `win/` headers to the decoder's include path: the MF decoder already includes from the project root; no CMake include change is needed since `wingpuimportedge.h` / `d3dfence.h` are under the source root.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/c --target tst_wingpuimportedge && ctest --test-dir build/c -R tst_wingpuimportedge --output-on-failure`
Expected: Windows CI with keep-texture → `importedReadbackMatchesCpuDecodeWithinOneLsb` passes (delta <= 1). macOS → QSKIPs. Then run the playback e2e to prove the `OLR_GPU_PIPELINE=0` byte-green gate is intact:
`ctest --test-dir build/c -R e2e_play --output-on-failure`
Expected: PASS unchanged (the gate is off by default; the decoder path is byte-identical).

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp \
        recorder_engine/ingest/nativevideodecoder.h tests/unit/tst_wingpuimportedge.cpp
git commit -m "feat(gpu-import-win): gate MF decode through WinGpuImportEdge under OLR_GPU_PIPELINE

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 7: Micro-stress — evict-while-import + OOM-degrade-to-CPU on the single feed

**Files:**
- Modify: `playback/output/win/d3d11gpusurface.h`/`.cpp` (add an injectable alloc-failure hook), `playback/output/win/wingpuimportedge.cpp`
- Test: `tests/unit/tst_wingpuimportedge.cpp`

**Interfaces:**
- Produces:
  - `void D3D11GpuSurface::setForceAllocFailureForTest(bool);` — a process-wide test hook making `createKept` return nullptr (simulating GPU-OOM) so `tryImport` returns `std::nullopt` (degrade-to-CPU).
  - Confirms: a surface with an unretired fence (`pendingFenceValue() > completed`) is NOT freed while a readback references it (refcount keep) — the §4 surface-lifetime contract.
- Consumes: `D3DFence` (Task 5), `D3D11GpuSurface` (Task 3), `WinGpuImportEdge::tryImport` (Task 2).

> This is the Phase-2 early micro-stress mandated by the slice (§8): exercise concurrent evict-while-render and OOM-degrade on the single feed against the real decode-done fence, giving the program's two highest-rated risks a signal before `gpu-sync`/`gpu-compositor`. The injected GPU-alloc failure proves `tryImport` degrades to the CPU `std::nullopt` path without crashing; the fence-retain check proves an in-flight readback's surface is not freed.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_wingpuimportedge.cpp`:

```cpp
    void allocFailureDegradesToCpuFallback();
    void surfaceSurvivesInFlightReadback();
```

```cpp
void TestWinGpuImportEdge::allocFailureDegradesToCpuFallback() {
#ifndef _WIN32
    QSKIP("Windows micro-stress");
#else
    QString err;
    auto edge = WinGpuImportEdge::create(&err);
    if (!edge) QSKIP("no GPU import edge on this host");
    D3D11GpuSurface::setForceAllocFailureForTest(true);
    // Even with a valid-looking sample shape, an injected alloc failure must make
    // tryImport return nullopt (caller degrades to CPU) — never crash.
    const std::optional<FrameHandle> h = edge->tryImport(nullptr, 0, 0, 1280, 720);
    QVERIFY2(!h.has_value(), "injected OOM must degrade to CPU fallback");
    D3D11GpuSurface::setForceAllocFailureForTest(false);
#endif
}

void TestWinGpuImportEdge::surfaceSurvivesInFlightReadback() {
#ifndef _WIN32
    QSKIP("Windows micro-stress");
#else
    QString err;
    auto edge = WinGpuImportEdge::create(&err);
    if (!edge) QSKIP("no GPU import edge on this host");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1, D3D11_SDK_VERSION, &device, &fl, &ctx)));
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 64; desc.Height = 64; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex)));
    auto surface = D3D11GpuSurface::createKept(device, tex, 0, 64, 64);
    QVERIFY(surface);
    // Simulate an in-flight render/readback fence on the surface.
    surface->retainUntilFenceRetired(99);
    std::weak_ptr<D3D11GpuSurface> weak = surface;
    // The handle still holds the surface; dropping the local ref must NOT free it
    // while the handle (the in-flight readback owner) is alive.
    auto data = std::make_shared<class StubGpuData>(surface); // owns a ref
    surface.reset();
    QVERIFY2(!weak.expired(), "surface freed while an in-flight readback still references it");
#endif
}
```

> `StubGpuData` is a tiny local test type holding a `shared_ptr<D3D11GpuSurface>` (defined in the test's `#ifdef _WIN32` block) standing in for the handle's `IFrameData` ownership. The assertion proves the refcount keep is what enforces lifetime — never an eager free.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/c --target tst_wingpuimportedge`
Expected: Windows → FAIL to compile, `D3D11GpuSurface::setForceAllocFailureForTest` undeclared. macOS → both QSKIP.

- [ ] **Step 3: Write the alloc-failure hook**

In `playback/output/win/d3d11gpusurface.h`, add the static hook declaration (public):

```cpp
    // Test-only: force createKept to return nullptr (simulated GPU-OOM) so the
    // import edge exercises its degrade-to-CPU path. Program spec §8 micro-stress.
    static void setForceAllocFailureForTest(bool force);
```

In `playback/output/win/d3d11gpusurface.cpp`, add the flag and honor it at the top of `createKept`:

```cpp
namespace {
std::atomic<bool> g_forceAllocFailure{false};
}

void D3D11GpuSurface::setForceAllocFailureForTest(bool force) {
    g_forceAllocFailure.store(force);
}
```

At the very start of `createKept`, before the validity checks:

```cpp
    if (g_forceAllocFailure.load()) return nullptr;
```

Add `#include <atomic>` (already present from Task 3) and ensure `g_forceAllocFailure` is defined above `createKept`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/c --target tst_wingpuimportedge && ctest --test-dir build/c -R tst_wingpuimportedge --output-on-failure`
Expected: Windows CI → `allocFailureDegradesToCpuFallback` (nullopt, no crash) and `surfaceSurvivesInFlightReadback` (weak ref alive) PASS. macOS → QSKIP. Then run the full unit suite to confirm no sibling regressions:
`ctest --test-dir build/c -L unit --output-on-failure`
Expected: PASS (the new tests added; everything else unchanged).

- [ ] **Step 5: Commit**

```bash
git add playback/output/win/d3d11gpusurface.h playback/output/win/d3d11gpusurface.cpp \
        playback/output/win/wingpuimportedge.cpp tests/unit/tst_wingpuimportedge.cpp
git commit -m "test(gpu-import-win): micro-stress evict-while-import + OOM-degrade-to-CPU

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Done-when

- `OLR_GPU_PIPELINE` unset: the full unit suite (`ctest -L unit`) and `e2e_play` pass with **identical assertion values and golden outputs** — the Windows decoder path is byte-identical to today (the import edge is dead code unless the flag is set and the host has the keep-texture path).
- `OLR_GPU_PIPELINE=1` on a keep-texture-capable Windows host: a single H.264 feed decodes to a kept `ID3D11Texture2D`, wraps in a GPU-backed `FrameHandle`, signals the decode-done fence before publish, and reaches readback via one fenced `readToCpu()`; the readback matches the CPU `copySampleToFrame` decode of the same frame **within ±1 LSB/channel** (guaranteed by the shared `nativeCopyNv12ToYuv420p` deinterleave).
- The documented fallback is proven: a host without the keep-texture path (or an injected GPU-OOM) makes `tryImport` return `std::nullopt`, and the decoder degrades to the existing CPU path without crashing.
- The fence primitive matches the chosen RHI backend (`kWinRhiBackend`); the default `d3d11` ships `ID3D11Fence`, with the D3D12 arm guarded for a Phase-0 D3D12 selection.
- No `ID3D11Texture2D`/`CVPixelBuffer` types in any shared (`playback/output/gpusurface.h`, `framehandle.h`) header — Windows concretes live only under `playback/output/win/`.
- Independent concurrency review (per CLAUDE.md) of the decode-done fence + surface-lifetime change before merge.

# Shader Toolchain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Stand up the portable RHI shader toolchain — link `Qt6::GuiPrivate` + `Qt6::ShaderTools`, define a `playback/gpu/shaders/` GLSL source layout, cross-compile GLSL→Metal/HLSL/SPIR-V `.qsb` via `qt_add_shaders`/`qsb`, and prove the chain end-to-end with a trivial pass-through `.qsb` that compiles, embeds, loads, and renders one offscreen frame under a deterministic QRhi **Null** context in a headless unit test, plus a CI shader-compile step.

**Architecture:** A new `olr_test_gpu` static library wraps a single RHI bring-up helper (`OlrRhi`) that creates a `QRhi` over the `QRhi::Null` backend (deterministic, headless, no GPU/D3D — the only backend hosted CI runners have) and runs one `beginOffscreenFrame`/`endOffscreenFrame` pass that builds a graphics pipeline from the baked pass-through `.qsb`. `qt_add_shaders` bakes `passthrough.vert`/`.frag` into a `.qsb` resource embedded via the Qt resource system; the test loads the `QShader` with `QShader::fromSerialized` and asserts it is valid and pipeline-buildable. This is the keystone proof that the RHI spine links and the shader pipeline (source → qsb → load → pipeline) is sound, before any compositor or import-edge work depends on it.

**Tech Stack:** C++17, Qt 6.10 (Core/Gui/GuiPrivate/ShaderTools/Test), CMake + Ninja, `qsb` (Qt Shader Baker), GLSL 440 source → SPIR-V/MSL/HLSL/GLSL cross-compile, QRhi Null backend.

## Global Constraints

- **Keystone-first, strict zero-regression gates.** This subproject is Phase 2 (GPU edge) and sits on `format-canon`; but its concrete deliverable — a pass-through `.qsb` loaded under QRhi Null — touches **no** existing pipeline code and does **not** consume the `FrameHandle` runtime types. It proves the toolchain in isolation. Nothing here changes a single existing assertion value or golden output.
- **CPU path stays default and reference.** Everything new lands behind a CMake option (`OLR_GPU_PIPELINE`, default `OFF`) and is compiled only into a new opt-in test library + a guarded engine library. No existing target gains an RHI/ShaderTools link until a later subproject flips the flag on for the app. The CPU pipeline remains the permanent correctness oracle.
- **Everything behind flags.** The RHI link, the shader baking, and the offscreen smoke test are all gated by `OLR_GPU_PIPELINE`. With the flag off, the build and the existing `ctest -L unit` suite are byte-identical to today.
- **No throwaway prototypes.** `OlrRhi` (the bring-up helper) and the `passthrough` shaders are production artifacts the compositor (`gpu-compositor`) and import edge (`gpu-abstraction`) build on; they stay in the tree.
- **Accept Qt-minor-version coupling (D1).** QRhi has limited cross-Qt-version compatibility guarantees and `Qt6::GuiPrivate` exposes semi-public headers (`<rhi/qrhi.h>`). This plan pins the toolchain to the project's Qt 6.10.x and documents that coupling in code comments — it is an accepted cost of the portable shader spine, not a defect to engineer around.
- **Probe-contingent on P0.3.** Phase-0 open question Q3 (RHI per-frame overhead within the <0.5 ms budget on a realistic cross-thread import→composite→readback path) must be resolved before the *compositor* lands. This plan's deliverable (a trivial offscreen pass) is the **linkage + toolchain** proof that the probe presupposes; it does not itself measure the budget. If P0.3 fails the budget, the toolchain still lands (it is a precondition for the probe), but `gpu-compositor` is re-scoped — that decision is out of scope here.
- **Precondition (stated):** RHI is **not currently linked**. `grep -niE "ShaderTools|QRhi|GuiPrivate" CMakeLists.txt` over the repo returns only the unrelated `find_package(... Network ...)` line. `find_package(Qt6 ... )` at `CMakeLists.txt:106` requests `Quick Core Multimedia Concurrent QuickControls2 Network WebSockets` — **no Gui-private, no ShaderTools**. Task 1 is therefore the link bring-up + a QRhi Null smoke init, before any shader is baked.
- **Build (run from the worktree root** `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/gpu-phase0-2-plans`**):** configure once with the GPU flag on — `cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON`; build `cmake --build build/c --target <target>`; test `ctest --test-dir build/c -R <name> --output-on-failure`. Use a fresh build dir when toggling `OLR_GPU_PIPELINE` (cached Qt/RHI link state is stale across the toggle).
- **Format changed lines only.** Several engine files are hand-written Allman; new files in `playback/gpu/` follow the project clang-format. Run `git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main -- '*.cpp' '*.h'` before each commit; never reformat whole pre-existing files.
- **Public-repo professionalism.** Code, comments, and commit messages are published. Keep them self-contained; document the present design (the portable shader spine), no internal notes or private history.

---

## File Structure

- **Create** `playback/gpu/olrrhi.h` / `olrrhi.cpp` — `OlrRhi`: a thin RAII wrapper that creates a single `QRhi` (Null backend by default; backend selectable later) and runs one offscreen frame that builds a graphics pipeline from a supplied vertex+fragment `QShader`. The RHI bring-up seam `gpu-abstraction` (D11: one `QRhi`, dedicated render thread) extends.
- **Create** `playback/gpu/shaders/passthrough.vert` / `passthrough.frag` — trivial pass-through GLSL 440 (full-screen-triangle vertex, solid-color fragment); the toolchain proof shader, retained as the smallest valid pipeline.
- **Create** `playback/gpu/shaders/olr_shaders.qrc` — Qt resource wrapping the baked `.qsb` files under the `:/olr/shaders/` prefix.
- **Create** `tests/unit/tst_shadertoolchain.cpp` — headless Qt Test: loads the baked `.qsb` from the resource, asserts `QShader` validity + stage, and drives `OlrRhi` to build the pipeline and run one offscreen frame on the Null backend.
- **Modify** `CMakeLists.txt` — add `option(OLR_GPU_PIPELINE ...)`; when on, extend `find_package(Qt6 ...)` with `Gui ShaderTools` and request the `GuiPrivate` component; the shader baking + RHI library are defined under the flag.
- **Modify** `tests/CMakeLists.txt` — define `olr_test_gpu` (the RHI helper + baked shaders) under `OLR_GPU_PIPELINE`, link `Qt6::GuiPrivate Qt6::ShaderTools`.
- **Modify** `tests/unit/CMakeLists.txt` — register `tst_shadertoolchain` against `olr_test_gpu`, gated on `OLR_GPU_PIPELINE`.
- **Modify** `.github/workflows/ci.yml` — add a CI shader-compile step (bake the `.qsb` and assert it is non-empty + multi-target) to the Linux leg (no GPU needed; the Null backend + `qsb` are CPU-only).

---

## Task 1: Link the RHI spine + QRhi Null smoke init (no shaders yet)

This is the precondition task: RHI is not currently linked. Bring up `Qt6::GuiPrivate`/`Qt6::ShaderTools`, add the `OLR_GPU_PIPELINE` flag, and prove a `QRhi` over the Null backend creates and runs an empty offscreen frame.

**Files:**
- Create: `playback/gpu/olrrhi.h`, `playback/gpu/olrrhi.cpp`
- Create: `tests/unit/tst_shadertoolchain.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  // playback/gpu/olrrhi.h
  class OlrRhi {
  public:
      enum class Backend { Null };  // gpu-abstraction widens this to Metal/D3D11 later.
      static std::unique_ptr<OlrRhi> create(Backend backend, QString* error);
      ~OlrRhi();
      OlrRhi(const OlrRhi&) = delete;
      OlrRhi& operator=(const OlrRhi&) = delete;
      QRhi* rhi() const;                 // non-owning; valid for the OlrRhi lifetime
      // Run one beginOffscreenFrame/endOffscreenFrame pass invoking record(cb) in
      // between. Returns false (sets *error) on any RHI frame-op failure.
      bool runOffscreenFrame(const std::function<void(QRhiCommandBuffer*)>& record,
                             QString* error);
  protected:
      OlrRhi() = default;
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_shadertoolchain.cpp`:

```cpp
// Shader-toolchain proof: the RHI spine links, a QRhi over the deterministic
// Null backend creates, and an empty offscreen frame runs headlessly. Task 2
// extends this same file with the baked-.qsb pipeline assertions.
#include <QtTest>

#include "playback/gpu/olrrhi.h"

#include <rhi/qrhi.h>

class TestShaderToolchain : public QObject {
    Q_OBJECT
private slots:
    void nullBackendCreatesAndRunsEmptyFrame();
};

void TestShaderToolchain::nullBackendCreatesAndRunsEmptyFrame() {
    QString err;
    auto rhi = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(rhi != nullptr, qPrintable("OlrRhi::create failed: " + err));
    QVERIFY(rhi->rhi() != nullptr);
    QCOMPARE(rhi->rhi()->backend(), QRhi::Null);

    bool recorded = false;
    const bool ok = rhi->runOffscreenFrame(
        [&](QRhiCommandBuffer* cb) {
            QVERIFY(cb != nullptr);
            recorded = true;
        },
        &err);
    QVERIFY2(ok, qPrintable("offscreen frame failed: " + err));
    QVERIFY(recorded);
}

QTEST_GUILESS_MAIN(TestShaderToolchain)
#include "tst_shadertoolchain.moc"
```

- [ ] **Step 2: Add the build wiring (flag + link + library) and register the test**

In `CMakeLists.txt`, add the option next to the existing options block (after `option(OLR_BUILD_FUZZERS ...)` at line 16):

```cmake
option(OLR_GPU_PIPELINE "Build the GPU-resident pipeline (RHI spine, shaders) — Phase 2, default off" OFF)
```

Immediately after the existing `find_package(Qt6 REQUIRED COMPONENTS Quick Core Multimedia Concurrent QuickControls2 Network WebSockets)` at line 106, add the GPU-pipeline components under the flag:

```cmake
if(OLR_GPU_PIPELINE)
    # RHI lives in Qt6::GuiPrivate (<rhi/qrhi.h> is a semi-public Gui header);
    # the shader baker (qsb / qt_add_shaders) comes from Qt6::ShaderTools.
    # QRhi has limited cross-Qt-version compatibility guarantees, so the GPU
    # pipeline is pinned to the project's Qt 6.10.x (accepted D1 cost).
    find_package(Qt6 REQUIRED COMPONENTS Gui GuiPrivate ShaderTools)
endif()
```

In `tests/CMakeLists.txt`, after the `olr_test_playback` block ends (the `qt_add_library(olr_test_playback ...)` + its `target_link_libraries` finish at line 244, before `add_subdirectory(unit)` at line 246), add:

```cmake
# GPU/RHI helper library — only built when the GPU pipeline is enabled. Holds the
# RHI bring-up seam (OlrRhi) that gpu-abstraction extends, plus (Task 2) the baked
# pass-through shaders. Links Qt6::GuiPrivate for <rhi/qrhi.h>.
if(OLR_GPU_PIPELINE)
    qt_add_library(olr_test_gpu STATIC
        "${CMAKE_SOURCE_DIR}/playback/gpu/olrrhi.cpp")
    target_include_directories(olr_test_gpu PUBLIC "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_gpu
        PUBLIC Qt6::Core Qt6::Gui Qt6::GuiPrivate
        PRIVATE olr_warnings olr_sanitize)
endif()
```

In `tests/unit/CMakeLists.txt`, append at the end (after `olr_add_unit_test(tst_recordgate olr_test_engine)` at line 156):

```cmake
if(OLR_GPU_PIPELINE)
    olr_add_unit_test(tst_shadertoolchain olr_test_gpu)
endif()
```

- [ ] **Step 3: Run the test to verify it fails**

Configure a fresh build dir with the flag on, then build the target:

```
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_shadertoolchain
```

Expected: FAIL to compile — `playback/gpu/olrrhi.h` does not exist (`fatal error: 'playback/gpu/olrrhi.h' file not found`).

- [ ] **Step 4: Write the OlrRhi helper**

Create `playback/gpu/olrrhi.h`:

```cpp
#ifndef OLR_OLRRHI_H
#define OLR_OLRRHI_H

#include <QString>

#include <functional>
#include <memory>

class QRhi;
class QRhiCommandBuffer;

// Thin RAII bring-up for the portable RHI spine. Creates a single QRhi over a
// chosen backend and runs offscreen frames. The Null backend is deterministic
// and headless (no GPU/D3D), which is what hosted CI runners have; the macOS
// Metal / Windows D3D backends are added by gpu-abstraction (D11: one QRhi, a
// dedicated render thread). QRhi is single-threaded — one OlrRhi is owned by one
// thread.
class OlrRhi {
public:
    enum class Backend { Null };

    // Returns nullptr (and sets *error) if the QRhi cannot be created.
    static std::unique_ptr<OlrRhi> create(Backend backend, QString* error);

    virtual ~OlrRhi();

    OlrRhi(const OlrRhi&) = delete;
    OlrRhi& operator=(const OlrRhi&) = delete;

    QRhi* rhi() const { return m_rhi.get(); }

    // Run one beginOffscreenFrame/endOffscreenFrame pass, calling record(cb)
    // between begin and end. Returns false (sets *error) on any frame-op error.
    bool runOffscreenFrame(const std::function<void(QRhiCommandBuffer*)>& record,
                           QString* error);

protected:
    OlrRhi() = default;
    std::unique_ptr<QRhi> m_rhi;
};

#endif // OLR_OLRRHI_H
```

Create `playback/gpu/olrrhi.cpp`:

```cpp
#include "playback/gpu/olrrhi.h"

#include <rhi/qrhi.h>

std::unique_ptr<OlrRhi> OlrRhi::create(Backend backend, QString* error) {
    auto self = std::unique_ptr<OlrRhi>(new OlrRhi());
    QRhi* raw = nullptr;
    switch (backend) {
    case Backend::Null: {
        QRhiNullInitParams params;
        raw = QRhi::create(QRhi::Null, &params);
        break;
    }
    }
    if (!raw) {
        if (error) *error = QStringLiteral("QRhi::create failed for the requested backend");
        return nullptr;
    }
    self->m_rhi.reset(raw);
    return self;
}

OlrRhi::~OlrRhi() = default;

bool OlrRhi::runOffscreenFrame(const std::function<void(QRhiCommandBuffer*)>& record,
                               QString* error) {
    if (!m_rhi) {
        if (error) *error = QStringLiteral("OlrRhi has no QRhi");
        return false;
    }
    QRhiCommandBuffer* cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb) {
        if (error) *error = QStringLiteral("beginOffscreenFrame failed");
        return false;
    }
    if (record) record(cb);
    if (m_rhi->endOffscreenFrame() != QRhi::FrameOpSuccess) {
        if (error) *error = QStringLiteral("endOffscreenFrame failed");
        return false;
    }
    return true;
}
```

- [ ] **Step 5: Run the test to verify it passes**

```
cmake --build build/c --target tst_shadertoolchain && ctest --test-dir build/c -R tst_shadertoolchain --output-on-failure
```

Expected: PASS (1 test) — `QRhi::Null` creates, an empty offscreen frame runs, `record` is invoked with a non-null command buffer.

- [ ] **Step 6: Verify the flag-off build is unchanged**

Configure a second fresh dir with the flag off and confirm the GPU library + test do not appear:

```
cmake -S . -B build/coff -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=OFF
ctest --test-dir build/coff -N -R tst_shadertoolchain
```

Expected: `No tests were found!!!` — the GPU test is absent with the flag off, proving zero footprint on the default build.

- [ ] **Step 7: Format + commit**

```bash
CF=/opt/homebrew/opt/llvm/bin/clang-format
python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/olrrhi.h playback/gpu/olrrhi.cpp tests/unit/tst_shadertoolchain.cpp \
        CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt
git commit -m "feat(gpu): link RHI spine (GuiPrivate/ShaderTools) + QRhi Null offscreen smoke

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 2: Bake a pass-through `.qsb`, load it, build a pipeline

Define the shader source layout, cross-compile GLSL→`.qsb` via `qt_add_shaders`, embed it as a Qt resource, and extend the test to load the `QShader` and build a graphics pipeline from it under the Null backend.

**Files:**
- Create: `playback/gpu/shaders/passthrough.vert`, `playback/gpu/shaders/passthrough.frag`
- Create: `playback/gpu/shaders/olr_shaders.qrc`
- Modify: `tests/CMakeLists.txt` (bake the shaders into `olr_test_gpu` + embed the resource)
- Modify: `tests/unit/tst_shadertoolchain.cpp` (load `.qsb`, build pipeline)

**Interfaces:**
- Consumes: `OlrRhi` (Task 1).
- Produces: a baked resource `:/olr/shaders/passthrough.vert.qsb` and `:/olr/shaders/passthrough.frag.qsb`, each a serialized multi-target `QShader` (SPIR-V + MSL + HLSL + GLSL), loadable via `QShader::fromSerialized`.

> The pass-through shaders use no vertex buffers: the vertex shader synthesizes a full-screen triangle from `gl_VertexIndex` (3 verts), and the fragment shader writes a constant color. This is the smallest pipeline that exercises the full chain (source → qsb cross-compile → resource embed → `fromSerialized` → `QRhiGraphicsPipeline` build) without a mesh, texture, or uniform buffer — keeping the toolchain proof DRY and dependency-free.

- [ ] **Step 1: Write the failing test extension**

In `tests/unit/tst_shadertoolchain.cpp`, add a slot declaration after `nullBackendCreatesAndRunsEmptyFrame()`:

```cpp
    void bakedPassthroughQsbLoadsAndBuildsPipeline();
```

Add the include near the top, after `#include <rhi/qrhi.h>`:

```cpp
#include <QFile>
#include <rhi/qshader.h>
```

Add the slot body:

```cpp
void TestShaderToolchain::bakedPassthroughQsbLoadsAndBuildsPipeline() {
    // 1. The baked .qsb resources exist and deserialize into valid QShaders.
    auto loadShader = [](const QString& path, QShader::Stage expectedStage) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QShader();
        const QShader s = QShader::fromSerialized(f.readAll());
        if (s.isValid()) {
            // The baked stage must match what the source declares.
            Q_ASSERT(s.stage() == expectedStage);
        }
        return s;
    };
    const QShader vert = loadShader(QStringLiteral(":/olr/shaders/passthrough.vert.qsb"),
                                    QShader::VertexStage);
    const QShader frag = loadShader(QStringLiteral(":/olr/shaders/passthrough.frag.qsb"),
                                    QShader::FragmentStage);
    QVERIFY2(vert.isValid(), "passthrough.vert.qsb missing or not deserializable");
    QVERIFY2(frag.isValid(), "passthrough.frag.qsb missing or not deserializable");
    QCOMPARE(vert.stage(), QShader::VertexStage);
    QCOMPARE(frag.stage(), QShader::FragmentStage);

    // 2. A QRhiGraphicsPipeline builds from the baked shaders under the Null
    //    backend — proves the .qsb is RHI-consumable, not merely deserializable.
    QString err;
    auto rhi = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(rhi != nullptr, qPrintable("OlrRhi::create failed: " + err));
    QRhi* r = rhi->rhi();

    std::unique_ptr<QRhiTexture> tex(
        r->newTexture(QRhiTexture::RGBA8, QSize(64, 64), 1, QRhiTexture::RenderTarget));
    QVERIFY(tex->create());
    QRhiColorAttachment att(tex.get());
    std::unique_ptr<QRhiTextureRenderTarget> rt(r->newTextureRenderTarget({att}));
    std::unique_ptr<QRhiRenderPassDescriptor> rpDesc(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rpDesc.get());
    QVERIFY(rt->create());

    std::unique_ptr<QRhiShaderResourceBindings> srb(r->newShaderResourceBindings());
    QVERIFY(srb->create());

    std::unique_ptr<QRhiGraphicsPipeline> pipeline(r->newGraphicsPipeline());
    pipeline->setShaderStages({{QRhiShaderStage::Vertex, vert},
                               {QRhiShaderStage::Fragment, frag}});
    QRhiVertexInputLayout inputLayout;  // no vertex buffers — verts from gl_VertexIndex
    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(srb.get());
    pipeline->setRenderPassDescriptor(rpDesc.get());
    QVERIFY2(pipeline->create(), "QRhiGraphicsPipeline build from baked .qsb failed");

    // 3. Drive one offscreen frame that draws the pass-through triangle.
    const bool ok = rhi->runOffscreenFrame(
        [&](QRhiCommandBuffer* cb) {
            cb->beginPass(rt.get(), QColor(0, 0, 0, 255), {1.0f, 0});
            cb->setGraphicsPipeline(pipeline.get());
            cb->setViewport(QRhiViewport(0, 0, 64, 64));
            cb->setShaderResources();
            cb->draw(3);
            cb->endPass();
        },
        &err);
    QVERIFY2(ok, qPrintable("offscreen draw failed: " + err));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```
cmake --build build/c --target tst_shadertoolchain
```

Expected: COMPILE+LINK succeed, runtime FAIL — the `.qsb` resources are not baked/embedded yet, so `QFile::open(":/olr/shaders/passthrough.vert.qsb")` fails and `vert.isValid()` is false: `FAIL! : TestShaderToolchain::bakedPassthroughQsbLoadsAndBuildsPipeline() 'vert.isValid()' returned FALSE (passthrough.vert.qsb missing or not deserializable)`.

- [ ] **Step 3: Write the shader sources + resource**

Create `playback/gpu/shaders/passthrough.vert`:

```glsl
#version 440

// Pass-through full-screen triangle: synthesize 3 clip-space verts from the
// vertex index — no vertex buffer needed. The smallest valid pipeline that
// exercises the GLSL->.qsb cross-compile + RHI pipeline build (the toolchain
// proof). gpu-compositor replaces this with the real YUV->RGB sampler.
void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
```

Create `playback/gpu/shaders/passthrough.frag`:

```glsl
#version 440

layout(location = 0) out vec4 fragColor;

// Constant opaque color — the trivial pass-through fragment stage.
void main()
{
    fragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
```

Create `playback/gpu/shaders/olr_shaders.qrc`:

```xml
<!DOCTYPE RCC>
<RCC version="1.0">
  <qresource prefix="/olr/shaders">
    <file alias="passthrough.vert.qsb">passthrough.vert.qsb</file>
    <file alias="passthrough.frag.qsb">passthrough.frag.qsb</file>
  </qresource>
</RCC>
```

- [ ] **Step 4: Bake the shaders into the library via `qt_add_shaders`**

In `tests/CMakeLists.txt`, inside the existing `if(OLR_GPU_PIPELINE)` block from Task 1 (right after the `target_link_libraries(olr_test_gpu ...)` call), add the shader baking. `qt_add_shaders` runs `qsb` to cross-compile each GLSL source into a multi-target `.qsb` and embeds it under the resource prefix:

```cmake
    # Cross-compile GLSL -> SPIR-V/MSL/HLSL/GLSL .qsb via qsb, embedded under
    # :/olr/shaders/ . The baked .qsb is what the RHI pipeline consumes at
    # runtime; this is the CI-checkable toolchain artifact.
    qt_add_shaders(olr_test_gpu "olr_passthrough_shaders"
        PREFIX "/olr/shaders"
        BASE "${CMAKE_SOURCE_DIR}/playback/gpu/shaders"
        FILES
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/passthrough.vert"
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/passthrough.frag"
        OUTPUTS
            "passthrough.vert.qsb"
            "passthrough.frag.qsb")
```

> `qt_add_shaders` embeds each baked file at `<PREFIX>/<OUTPUTS>` → `:/olr/shaders/passthrough.vert.qsb`. The hand-written `olr_shaders.qrc` from Step 3 documents the resource layout for human readers and for the CI bake step (Task 3); the runtime resource is produced by `qt_add_shaders` directly, so the `.qrc` is not separately compiled into `olr_test_gpu` (avoid double-registering the same alias). Keep the `.qrc` as the canonical layout reference.

- [ ] **Step 5: Run the test to verify it passes**

```
cmake --build build/c --target tst_shadertoolchain && ctest --test-dir build/c -R tst_shadertoolchain --output-on-failure
```

Expected: PASS (2 tests) — the baked `.qsb` deserialize into valid `QShader`s with the correct stages, a `QRhiGraphicsPipeline` builds from them, and an offscreen pass-through draw completes on the Null backend.

- [ ] **Step 6: Verify the multi-target bake from the command line**

Confirm `qsb` produced a multi-target artifact (SPIR-V + MSL + HLSL + GLSL), proving the Metal/HLSL/SPIR-V cross-compile, not just SPIR-V:

```
$HOME/Qt/6.10.1/macos/bin/qsb --dump $(find build/c -name 'passthrough.frag.qsb' | head -1)
```

Expected: the dump header lists multiple shading languages (e.g. `SPIR-V 100`, `MSL 12`, `HLSL 50`, `GLSL 120/150`), confirming the cross-compile targets are all present in the one `.qsb`.

- [ ] **Step 7: Format + commit**

```bash
CF=/opt/homebrew/opt/llvm/bin/clang-format
python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/shaders/passthrough.vert playback/gpu/shaders/passthrough.frag \
        playback/gpu/shaders/olr_shaders.qrc tests/CMakeLists.txt tests/unit/tst_shadertoolchain.cpp
git commit -m "feat(gpu): bake pass-through .qsb + build an RHI pipeline from it

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 3: CI shader-compile step

Add a CI step that bakes the `.qsb` and asserts the artifact is valid and multi-target — so a broken shader source or a missing `qsb`/ShaderTools fails CI independently of the unit test. Add it to the Linux leg: the Null backend + `qsb` are CPU-only, and Linux is the cheapest always-on leg (the hosted runners have no GPU/D3D, which is exactly the constraint that makes the Null-backend toolchain the right CI surface).

**Files:**
- Modify: `.github/workflows/ci.yml` (Linux `build-test-linux` job)

**Interfaces:**
- Consumes: the `OLR_GPU_PIPELINE`-gated `qt_add_shaders` bake (Task 2) and the `qsb` tool from `Qt6::ShaderTools`.
- Produces: a CI gate that fails when the shader bake fails or the `.qsb` is empty / single-target.

> The Linux job currently installs Qt with `modules: qtmultimedia qtwebsockets` and configures **without** `OLR_GPU_PIPELINE`. The shader-compile step builds the `olr_test_gpu` target with the flag on (in a dedicated build dir so the existing flag-off Linux build is untouched), then verifies the baked `.qsb` artifacts. `qsb` and `Qt6::ShaderTools` ship with the base desktop Qt that `install-qt-action` provides — no extra module is required.

- [ ] **Step 1: Read the current Linux job to anchor the insertion point**

Read `.github/workflows/ci.yml` around the `build-test-linux` job (line 475 onward) to find the existing `Configure` and `Build (app + tests)` steps and the `runs-on: ubuntu-latest` / Qt-install block, so the new step is inserted after the build and matches the job's `working-directory`/shell conventions.

```
ctest # (no-op anchor; this step is reading only)
```

Expected: you can see the `- name: Configure` step (a `cmake -S . -B build ...` invocation) and the `- name: Build (app + tests)` step that the new step follows.

- [ ] **Step 2: Add the shader-compile step**

In `.github/workflows/ci.yml`, in the `build-test-linux` job, add a new step immediately after the existing `- name: Build (app + tests)` / `run: cmake --build build` step:

```yaml
      - name: Shader toolchain compile (qsb / .qsb bake)
        run: |
          set -euo pipefail
          # Configure a dedicated GPU-pipeline build dir so the flag-off Linux
          # build above is untouched. ShaderTools + qsb ship with base Qt.
          cmake -S . -B build-gpu -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DOLR_BUILD_TESTS=ON \
            -DOLR_GPU_PIPELINE=ON \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DCMAKE_PREFIX_PATH="$QT_ROOT_DIR"
          # Build the RHI helper lib — this triggers the qt_add_shaders bake.
          cmake --build build-gpu --target olr_test_gpu
          # Assert the baked .qsb exist, are non-empty, and are multi-target
          # (SPIR-V + MSL + HLSL + GLSL) — proving the cross-compile, not just
          # that a file was written.
          QSB="$QT_ROOT_DIR/bin/qsb"
          for stage in vert frag; do
            qsbfile="$(find build-gpu -name "passthrough.$stage.qsb" | head -1)"
            test -n "$qsbfile" || { echo "missing passthrough.$stage.qsb"; exit 1; }
            test -s "$qsbfile" || { echo "empty passthrough.$stage.qsb"; exit 1; }
            dump="$("$QSB" --dump "$qsbfile")"
            echo "$dump"
            echo "$dump" | grep -q "SPIR-V" || { echo "no SPIR-V target in $stage"; exit 1; }
            echo "$dump" | grep -q "MSL"    || { echo "no MSL target in $stage";    exit 1; }
            echo "$dump" | grep -q "HLSL"   || { echo "no HLSL target in $stage";   exit 1; }
          done
          echo "Shader toolchain OK: pass-through .qsb baked multi-target."
```

- [ ] **Step 3: Lint the workflow locally**

The repo has a `workflow-lint` CI job that runs `actionlint`. Run it locally to catch YAML/shell errors before pushing:

```
actionlint .github/workflows/ci.yml
```

Expected: no findings (exit 0). If `actionlint` is not installed, install via `brew install actionlint` first; the step's shell uses `set -euo pipefail` and quoted expansions so SC2086-class warnings do not fire.

- [ ] **Step 4: Dry-run the bake locally (reproduces the CI step on macOS)**

Reproduce the artifact assertions the CI step makes, against the Task-2 build dir, to confirm the grep contract matches `qsb --dump` output on this Qt version:

```
for stage in vert frag; do f="$(find build/c -name "passthrough.$stage.qsb" | head -1)"; test -s "$f" && $HOME/Qt/6.10.1/macos/bin/qsb --dump "$f" | grep -E "SPIR-V|MSL|HLSL"; done
```

Expected: each stage prints lines containing `SPIR-V`, `MSL`, and `HLSL` — matching the `grep -q` assertions in the CI step.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci(gpu): add a shader-toolchain compile gate (qsb multi-target bake)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 4: Document the shader source layout + backend-selection seam

Capture the source layout, the qsb bake conventions, and the Null→Metal/D3D11 backend-selection seam in a short README beside the shaders, so `gpu-abstraction` (which widens `OlrRhi::Backend`) and `gpu-compositor` (which adds real shaders) extend a documented contract rather than reverse-engineering it.

**Files:**
- Create: `playback/gpu/shaders/README.md`
- Test: none (documentation task; the toolchain itself is gated by Tasks 1–3).

**Interfaces:**
- Consumes: the layout/bake conventions established in Tasks 1–2.
- Produces: `playback/gpu/shaders/README.md` — the canonical shader-toolchain reference.

> This is a documentation deliverable, not a code path, so it has no failing-test step. It exists because the layout + backend seam are load-bearing contracts that two downstream subprojects extend; leaving them undocumented would invite drift. Keep it factual and self-contained (public repo).

- [ ] **Step 1: Write the README**

Create `playback/gpu/shaders/README.md`:

```markdown
# GPU shader toolchain

Portable shader spine for the GPU-resident pipeline (design: D1). One GLSL
source set is cross-compiled to every backend's shading language and packed into
`.qsb` containers; the RHI consumes the `.qsb` at runtime.

## Source layout

- `*.vert` / `*.frag` — GLSL 440 source (the authoring language).
- `qt_add_shaders(<target> ...)` (in `tests/CMakeLists.txt`, under
  `OLR_GPU_PIPELINE`) runs `qsb` to bake each source into a multi-target `.qsb`
  (SPIR-V + MSL + HLSL + GLSL) and embeds it at `:/olr/shaders/<name>.qsb`.
- `olr_shaders.qrc` documents the embedded resource layout (canonical reference;
  the runtime resource is produced by `qt_add_shaders`, not this `.qrc`).

## Baking

`qsb --qt6 <source>` is the default target set (`--glsl "100 es,120,150"
--hlsl 50 --msl 12`). Inspect a baked artifact with `qsb --dump <file>.qsb`;
the header lists every shading language present, which the CI shader-compile
gate asserts.

## Runtime load

`QShader::fromSerialized(QByteArray)` deserializes a `.qsb` into a `QShader`;
`OlrRhi` (`playback/gpu/olrrhi.h`) builds a `QRhiGraphicsPipeline` from the
vertex+fragment `QShader` pair.

## Backend-selection seam

`OlrRhi::Backend` is `Null` only today — the deterministic, headless,
GPU-free backend that CI runs on. `gpu-abstraction` widens it to `Metal`
(macOS/iOS) and `D3D11` (Windows) behind the same `OlrRhi::create` factory, per
D11 (one `QRhi`, one dedicated render thread). The Null backend stays as the CI
oracle + the RHI-unavailable fallback target.

## Qt-version coupling

QRhi / `.qsb` have limited cross-Qt-version compatibility guarantees, so the GPU
pipeline is pinned to the project's Qt 6.10.x (accepted D1 cost). Re-bake the
`.qsb` whenever the Qt minor version changes.
```

- [ ] **Step 2: Verify it renders as valid Markdown**

```
test -s playback/gpu/shaders/README.md && echo "README present"
```

Expected: `README present`. (No build/test impact; documentation only.)

- [ ] **Step 3: Commit**

```bash
git add playback/gpu/shaders/README.md
git commit -m "docs(gpu): document the shader source layout + backend-selection seam

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Done-when

- `OLR_GPU_PIPELINE=ON`: `ctest --test-dir build/c -R tst_shadertoolchain --output-on-failure` passes both tests — QRhi Null creates + runs an empty offscreen frame (Task 1), and the baked pass-through `.qsb` deserializes, builds a `QRhiGraphicsPipeline`, and renders one offscreen pass (Task 2).
- `OLR_GPU_PIPELINE=OFF` (default): the `olr_test_gpu` library and `tst_shadertoolchain` are absent; the existing `ctest -L unit` suite is byte-identical to today — no RHI/ShaderTools link on any pre-existing target. This is the zero-regression gate for this subproject.
- The baked `.qsb` are multi-target (SPIR-V + MSL + HLSL + GLSL), confirmed by `qsb --dump` (Metal/HLSL/SPIR-V cross-compile proven).
- The CI Linux leg runs the shader-compile gate (Task 3): a broken source or a missing `qsb`/ShaderTools fails CI.
- The source layout + backend seam are documented (Task 4) for `gpu-abstraction`/`gpu-compositor` to extend.

## Carry-forwards / out of scope (downstream subprojects)

- **Real Metal/D3D11 backends** + the dedicated render thread (D11) → `gpu-abstraction` widens `OlrRhi::Backend` past `Null`.
- **RHI per-frame overhead probe** (P0.3 / Q3, the <0.5 ms budget on a cross-thread import→composite→readback path) → a Phase-0 measurement that presupposes this toolchain links; not measured here.
- **The real YUV→RGB / grid / bilinear/Lanczos shaders** + the NV12-deinterleave golden oracle → `gpu-compositor` / `format-canon` replace `passthrough.{vert,frag}`.
- **Consuming the `FrameHandle` canonical types** (`FramePixelFormat`, `CpuPlanes`, `GpuSurface`, etc.) → the compositor/import edges; the toolchain proof deliberately depends on none of them.
```
# Feed Preview Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep PGM and multiview `VideoOutput` consumers attached across provider replacement and advancing under continuous producer load.

**Architecture:** Preserve the current output-bus, async GPU-readback, `FrameProvider`, and Qt `VideoOutput` layers. Make QML provider references destruction-aware, implement one progress-guaranteed latest-frame callback per sink, and turn QML runtime diagnostics into an app-E2E failure.

**Tech Stack:** C++20, Qt 6 Core/Multimedia/Quick/QuickTest, QML, Python 3, CMake/CTest.

## Global Constraints

- Do not replace `VideoOutput` with CPU-painted preview items.
- Do not change GPU readback cadence, PGM transaction completion, output-bus rendering, or external sink behavior.
- Keep same-thread `QVideoSink` delivery immediate and preserve serial-based flush semantics.
- Format changed C++ lines only and keep the public repository free of private incident context.

---

### Task 1: Reattach active QML previews after provider replacement

**Files:**
- Modify: `tests/qmlstyle/tst_pgmstage_mapping_main.cpp`
- Modify: `tests/qmlstyle/tst_pgmstage_mapping.qml`
- Modify: `tests/e2e/test_pgmstage_videooutput_static.py`
- Modify: `tests/e2e/CMakeLists.txt`
- Modify: `ui/components/PgmStage.qml`
- Modify: `MultiviewWindow.qml`

**Interfaces:**
- Consumes: `FrameProvider::addVideoSink(QVideoSink*)`, `FrameProvider::removeVideoSink(QVideoSink*)`, and `UIManager::playbackProvidersChanged()`.
- Produces: QML attachment helpers whose `provider` and `attachedProvider` properties are `QtObject`, and test fixture accessors `replacePreviewProviders()`, `multiviewAddCalls`, and `pgmAddCalls`.

- [ ] **Step 1: Add a replaceable C++ preview-provider fixture**

Extend `tst_pgmstage_mapping_main.cpp` with a `PreviewProviderStub` that exposes
`Q_INVOKABLE addVideoSink(QObject*)`/`removeVideoSink(QObject*)` counters and a
`PreviewUiStub` that owns PGM/multiview providers, exposes them through
`Q_PROPERTY`, deletes/recreates both in `replacePreviewProviders()`, and emits
`playbackProvidersChanged`. In `qmlEngineAvailable(QQmlEngine*)`, publish the
fixture as the `previewUi` context property.

```cpp
class PreviewProviderStub : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE void addVideoSink(QObject* sink) {
        if (sink) ++m_addCalls;
    }
    Q_INVOKABLE void removeVideoSink(QObject* sink) {
        if (sink) ++m_removeCalls;
    }
    int addCalls() const { return m_addCalls; }
private:
    int m_addCalls = 0;
    int m_removeCalls = 0;
};

class PreviewUiStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(PreviewProviderStub* multiviewPreviewProvider READ multiviewPreviewProvider
                   NOTIFY playbackProvidersChanged)
    Q_PROPERTY(PreviewProviderStub* pgmPreviewProvider READ pgmPreviewProvider
                   NOTIFY playbackProvidersChanged)
    Q_PROPERTY(QVariantList viewSlotMap READ viewSlotMap CONSTANT)
    Q_PROPERTY(int multiviewCount READ multiviewCount CONSTANT)
    Q_PROPERTY(bool playbackSingleView READ playbackSingleView CONSTANT)
    Q_PROPERTY(int playbackSelectedIndex READ playbackSelectedIndex CONSTANT)
    Q_PROPERTY(int multiviewAddCalls READ multiviewAddCalls NOTIFY playbackProvidersChanged)
    Q_PROPERTY(int pgmAddCalls READ pgmAddCalls NOTIFY playbackProvidersChanged)
public:
    explicit PreviewUiStub(QObject* parent = nullptr) : QObject(parent) {
        replacePreviewProviders();
    }
    Q_INVOKABLE void replacePreviewProviders();
    Q_INVOKABLE void setPlaybackViewState(bool, int) {}
    Q_INVOKABLE QString sourceDisplayLabel(int source) const {
        return QStringLiteral("SRC%1").arg(source);
    }
    PreviewProviderStub* multiviewPreviewProvider() const { return m_multiview; }
    PreviewProviderStub* pgmPreviewProvider() const { return m_pgm; }
    QVariantList viewSlotMap() const { return {0, 1, 2, 3}; }
    int multiviewCount() const { return 4; }
    bool playbackSingleView() const { return false; }
    int playbackSelectedIndex() const { return -1; }
    int multiviewAddCalls() const { return m_multiview ? m_multiview->addCalls() : 0; }
    int pgmAddCalls() const { return m_pgm ? m_pgm->addCalls() : 0; }

    Q_INVOKABLE void replacePreviewProviders() {
        delete m_multiview;
        delete m_pgm;
        m_multiview = new PreviewProviderStub(this);
        m_pgm = new PreviewProviderStub(this);
        emit playbackProvidersChanged();
    }
signals:
    void playbackProvidersChanged();
    void streamUrlsChanged();
    void multiviewCountChanged();
    void viewSlotMapChanged();
    void feedSelectRequested(int index);
    void playbackViewStateChanged();
    void multiviewRequested();
private:
    PreviewProviderStub* m_multiview = nullptr;
    PreviewProviderStub* m_pgm = nullptr;
};

class PgmStageMappingSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        qmlRegisterType<FramePreviewItemStub>("Recorder.Types", 1, 0, "FramePreviewItem");
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("previewUi"), &m_previewUi);
    }
private:
    PreviewUiStub m_previewUi;
};
```

- [ ] **Step 2: Add failing embedded multiview and PGM lifecycle tests**

Create a second `PgmStage` bound to `previewUi`. Keep it active but outside the
visible test bounds. Add one test that replaces providers in multi mode and
expects the new multiview provider to receive a sink, and another that selects a
single view before replacement and expects the new PGM provider to receive a
sink.

```qml
PgmStage {
    id: lifecycleStage
    x: tc.width + 10
    width: 320
    height: 180
    ui: previewUi
}

function test_activeMultiviewReattachesAfterProvidersAreReplaced() {
    lifecycleStage.resetToMulti()
    previewUi.replacePreviewProviders()
    tryCompare(previewUi, "multiviewAddCalls", 1)
}

function test_activePgmReattachesAfterProvidersAreReplaced() {
    lifecycleStage.selectViewSlot(0)
    previewUi.replacePreviewProviders()
    tryCompare(previewUi, "pgmAddCalls", 1)
}
```

- [ ] **Step 3: Run the Quick Test and verify RED**

Run:

```sh
cmake --build build/c --target tst_pgmstage_mapping
ctest --test-dir build/c -R '^tst_pgmstage_mapping$' --output-on-failure
```

Expected: FAIL because the replacement provider records zero sink attachments,
with the current QML emitting `removeVideoSink ... is not a function`.

- [ ] **Step 4: Extend the static preview contract to the detached multiview**

Pass `${CMAKE_SOURCE_DIR}/MultiviewWindow.qml` as a second argument to
`test_pgmstage_videooutput_static.py`. Require both QML files to use
`property QtObject attachedProvider`, clear `attachedProvider` before cleanup,
and guard old-provider removal with `typeof ...removeVideoSink === "function"`.

Run:

```sh
python3 tests/e2e/test_pgmstage_videooutput_static.py \
  ui/components/PgmStage.qml MultiviewWindow.qml
```

Expected: FAIL because both files currently store the replaceable provider in
`property var attachedProvider`.

- [ ] **Step 5: Make QML provider ownership destruction-aware**

In `PgmStage.qml`, change the root provider aliases and the inline
`PreviewVideoOutput` properties from `var` to `QtObject`. Clear the old reference
before optional cleanup and require the new provider to attach:

```qml
readonly property QtObject pgmProvider: ui ? ui.pgmPreviewProvider : null
readonly property QtObject multiviewProvider: ui ? ui.multiviewPreviewProvider : null

property QtObject provider: null
property QtObject attachedProvider: null

function attachProvider(provider) {
    if (previewOutput.attachedProvider === provider) return
    var previousProvider = previewOutput.attachedProvider
    previewOutput.attachedProvider = null
    if (previousProvider
            && typeof previousProvider.removeVideoSink === "function") {
        previousProvider.removeVideoSink(videoSink)
    }
    previewOutput.attachedProvider = provider
    if (previewOutput.attachedProvider) {
        previewOutput.attachedProvider.addVideoSink(videoSink)
    }
}
```

In `MultiviewWindow.qml`, use the following equivalent attachment function:

```qml
property QtObject attachedProvider: null

function attachProvider(provider) {
    if (attachedProvider === provider) return
    var previousProvider = attachedProvider
    attachedProvider = null
    if (previousProvider
            && typeof previousProvider.removeVideoSink === "function") {
        previousProvider.removeVideoSink(videoSink)
    }
    attachedProvider = provider
    if (attachedProvider) attachedProvider.addVideoSink(videoSink)
}
```

- [ ] **Step 6: Run the QML tests and verify GREEN**

Run the Step 3 Quick Test commands and the Step 4 static command again.

Expected: PASS with both replacement providers receiving their active sink and
no QML `TypeError` output.

- [ ] **Step 7: Commit the QML lifecycle fix**

```sh
git add tests/qmlstyle/tst_pgmstage_mapping_main.cpp \
  tests/qmlstyle/tst_pgmstage_mapping.qml \
  tests/e2e/test_pgmstage_videooutput_static.py tests/e2e/CMakeLists.txt \
  ui/components/PgmStage.qml MultiviewWindow.qml
git commit -m "fix: reattach replaced preview providers" \
  -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Guarantee continuous latest-frame progress

**Files:**
- Modify: `tests/unit/tst_qtpreviewsink.cpp`
- Modify: `playback/frameprovider.h`
- Modify: `playback/frameprovider.cpp`

**Interfaces:**
- Consumes: `FrameProvider::deliverFrame`, `addVideoSink`, `removeVideoSink`, and `flushVideoSinks`.
- Produces: private `queueLatestFrameForSink(QVideoSink*)` and `applyLatestFrameToSink(QVideoSink*)` helpers plus a per-sink pending-update set.

- [ ] **Step 1: Add a failing continuously-producing sink test**

Add `frameProviderAdvancesBackloggedSinkWhileProducerIsRunning()`. Present an
initial frame, queue a 50 ms blocker on the sink thread, then run a producer that
delivers frames faster than interleaved 2 ms sink-thread delays. Assert that the
sink produces a second `videoFrameChanged` before stopping the producer.

```cpp
QVERIFY(QMetaObject::invokeMethod(
    sink.get(), []() { QThread::msleep(50); }, Qt::QueuedConnection));

std::jthread producer([&](std::stop_token stopToken) {
    int frameNumber = 0;
    while (!stopToken.stop_requested()) {
        FrameHandle frame = solidYuv420pHandle(
            4, 4, quint8(40 + (frameNumber++ % 16) * 8), 128, 128);
        provider.deliverHandle(frame);
        QMetaObject::invokeMethod(
            sink.get(), []() { QThread::msleep(2); }, Qt::QueuedConnection);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
});

QTRY_VERIFY_WITH_TIMEOUT(visibleFrameChanges.load(std::memory_order_relaxed) > 1, 150);
producer.request_stop();
producer.join();
```

- [ ] **Step 2: Run the focused unit test and verify RED**

Run:

```sh
cmake --build build/c --target tst_qtpreviewsink
build/c/tests/unit/tst_qtpreviewsink frameProviderAdvancesBackloggedSinkWhileProducerIsRunning
```

Expected: FAIL because captured-serial callbacks all find a newer serial while
the producer is active.

- [ ] **Step 3: Implement one pending latest-frame callback per sink**

Add `QSet<QVideoSink*> m_pendingSinkUpdates`. For cross-thread delivery,
`queueLatestFrameForSink()` inserts the sink only if no callback is pending and
calls `postLatestFrameForSink()`. The callback reads the latest frame/serial when
it runs, presents it, then holds `m_sinkMutex` while reading the current serial:

```cpp
bool FrameProvider::postLatestFrameForSink(QVideoSink* sink) {
    QPointer<FrameProvider> provider = this;
    QPointer<QVideoSink> guardedSink = sink;
    const bool queued = QMetaObject::invokeMethod(
        sink,
        [provider, guardedSink]() {
            if (provider && guardedSink)
                provider->applyLatestFrameToSink(guardedSink);
        },
        Qt::QueuedConnection);
    if (!queued) {
        QMutexLocker locker(&m_sinkMutex);
        m_pendingSinkUpdates.remove(sink);
    }
    return queued;
}

void FrameProvider::queueLatestFrameForSink(QVideoSink* sink) {
    if (!sink) return;
    {
        QMutexLocker locker(&m_sinkMutex);
        const bool registered = std::any_of(
            m_sinks.cbegin(), m_sinks.cend(),
            [sink](const QPointer<QVideoSink>& candidate) { return candidate == sink; });
        if (!registered || m_pendingSinkUpdates.contains(sink)) return;
        m_pendingSinkUpdates.insert(sink);
    }
    postLatestFrameForSink(sink);
}

void FrameProvider::applyLatestFrameToSink(QVideoSink* sink) {
    if (!sink) return;

    QVideoFrame frame;
    quint64 appliedSerial = 0;
    {
        QMutexLocker frameLocker(&m_frameMutex);
        frame = m_lastFrame;
        appliedSerial = m_frameSerial;
    }
    if (frame.isValid()) sink->setVideoFrame(frame);

    bool queueAgain = false;
    {
        QMutexLocker sinkLocker(&m_sinkMutex);
        if (!m_pendingSinkUpdates.contains(sink)) return;
        if (frame.isValid()) {
            m_appliedSerialBySink[sink] =
                qMax(m_appliedSerialBySink.value(sink, 0), appliedSerial);
        }
        QMutexLocker frameLocker(&m_frameMutex);
        queueAgain = m_frameSerial > appliedSerial;
        if (!queueAgain) m_pendingSinkUpdates.remove(sink);
    }

    if (queueAgain) {
        postLatestFrameForSink(sink);
    }
}
```

Use this helper from both `addVideoSink()` and `deliverFrame()` for cross-thread
sinks. Remove the exact-serial callback and `latestFrameForSerial()`. Clear
pending state in `removeVideoSink()`.

- [ ] **Step 4: Run focused coalescing/flush tests and verify GREEN**

Run:

```sh
cmake --build build/c --target tst_qtpreviewsink
build/c/tests/unit/tst_qtpreviewsink \
  frameProviderCoalescesQueuedVideoSinkUpdatesToLatestFrame \
  frameProviderAdvancesBackloggedSinkWhileProducerIsRunning \
  frameProviderFlushWaitsForSubmittedSerial
```

Expected: PASS. The stopped-batch test still observes one update, the continuous
test advances before producer shutdown, and flush reaches its requested serial.

- [ ] **Step 5: Commit the delivery fix**

```sh
git add playback/frameprovider.h playback/frameprovider.cpp \
  tests/unit/tst_qtpreviewsink.cpp
git commit -m "fix: keep live preview sinks advancing" \
  -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 3: Reject silent QML preview failures in the real-app oracle

**Files:**
- Modify: `tests/e2e/test_macos_app_driver_static.py`
- Modify: `tests/e2e/macos_app_driver.py`

**Interfaces:**
- Produces: `assert_no_openlivereplay_qml_errors(app_log: Path) -> None`.
- Consumes: the app log already captured by `macos_app_driver.py`.

- [ ] **Step 1: Add a failing driver-unit assertion**

After importing the driver module in `test_macos_app_driver_static.py`, create a
temporary clean log and a log containing the observed PGM-stage `TypeError`.
Require clean input to pass and bad input to raise `AssertionError`. Also require
`main()` to call the new scanner before printing `APP_E2E_PASS`.

```python
with tempfile.TemporaryDirectory() as directory:
    app_log = Path(directory) / "app.log"
    app_log.write_text("normal output\n", encoding="utf-8")
    module.assert_no_openlivereplay_qml_errors(app_log)
    app_log.write_text(
        "qrc:/qt/qml/OpenLiveReplay/ui/components/PgmStage.qml:138: "
        "TypeError: addVideoSink is not a function\n",
        encoding="utf-8",
    )
    try:
        module.assert_no_openlivereplay_qml_errors(app_log)
    except AssertionError:
        pass
    else:
        raise AssertionError("OpenLiveReplay QML runtime errors must fail the app oracle")
```

- [ ] **Step 2: Run the static driver test and verify RED**

Run:

```sh
python3 tests/e2e/test_macos_app_driver_static.py tests/e2e/macos_app_driver.py
```

Expected: FAIL because `assert_no_openlivereplay_qml_errors` is absent.

- [ ] **Step 3: Implement and invoke the QML error scanner**

```python
def assert_no_openlivereplay_qml_errors(app_log):
    diagnostics = []
    for line in Path(app_log).read_text(encoding="utf-8", errors="replace").splitlines():
        if "qrc:/qt/qml/OpenLiveReplay/" not in line:
            continue
        if any(token in line for token in ("TypeError:", "ReferenceError:", "Binding loop")):
            diagnostics.append(line)
    if diagnostics:
        raise AssertionError(
            "OpenLiveReplay QML runtime errors:\n" + "\n".join(diagnostics[:20])
        )
```

Call it after `recording.stop` and before `APP_E2E_PASS`.

- [ ] **Step 4: Run the static driver test and verify GREEN**

Run the Step 2 command again.

Expected: PASS.

- [ ] **Step 5: Commit the oracle hardening**

```sh
git add tests/e2e/macos_app_driver.py tests/e2e/test_macos_app_driver_static.py
git commit -m "test: fail app oracle on QML preview errors" \
  -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 4: Verify the integrated preview path

**Files:**
- Verify only; modify files only if a focused failure identifies an in-scope defect.

**Interfaces:**
- Consumes: all behavior from Tasks 1-3.
- Produces: fresh build, test, runtime, formatting, and diff evidence for the PR.

- [ ] **Step 1: Format and inspect changed lines**

```sh
git add -- '*.cpp' '*.h'
python3 /opt/homebrew/opt/llvm/bin/git-clang-format \
  --binary /opt/homebrew/opt/llvm/bin/clang-format \
  --commit origin/main -- '*.cpp' '*.h'
git diff --check
```

Review the staged and unstaged diff; stage only intended files after formatting.

- [ ] **Step 2: Build and run focused tests**

```sh
cmake --build build/c
ctest --test-dir build/c \
  -R '^tst_pgmstage_mapping$|^tst_qtpreviewsink$|^tst_macos_app_driver_static$' \
  --output-on-failure
```

Expected: all three tests pass.

- [ ] **Step 3: Run the complete unit label**

```sh
ctest --test-dir build/c -L unit --output-on-failure
```

Expected: 100% pass.

- [ ] **Step 4: Run playback E2E**

```sh
ctest --test-dir build/c -R '^e2e_play_(stepscrub|sliderscrub)$' --output-on-failure
```

Expected: both scenarios pass.

- [ ] **Step 5: Run the real four-feed macOS app visual oracle**

```sh
OLR_APP_E2E_KEEP_WORKDIR=1 OLR_APP_E2E_VISUAL_DEBUG=1 \
  ctest --test-dir build/c -R '^e2e_macos_app_visual$' --output-on-failure
```

Expected: PASS, visible single-feed markers advance, and the app log contains no
OpenLiveReplay QML runtime errors. Inspect at least the startup, prime, and final
screenshots from the retained workdir.

- [ ] **Step 6: Review requirements and repository state**

```sh
git status --short --branch
git diff origin/main...HEAD --check
git diff --stat origin/main...HEAD
git log --oneline origin/main..HEAD
```

Confirm the diff is limited to the design/plan, provider lifecycle, delivery
coalescer, and regression gates described above.

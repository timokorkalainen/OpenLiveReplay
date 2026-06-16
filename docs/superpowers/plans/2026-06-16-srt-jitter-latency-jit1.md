# JIT-1: per-transport jitter window + inert-SRT-options fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** (1) Make SRT's `latency`/`rcvlatency`/`peerlatency`/`transtype`/`connect_timeout` actually apply on the ffmpeg ingest path (they're set on the avformat opts dict, where they're silently ignored — and the latency value is wrong-unit too). (2) Right-size the engine jitter window per transport: a small floor for SRT (TSBPD already smoothed it), the existing 200ms for raw UDP/RTMP.

**Architecture:** Two pure, unit-tested free functions in `recorder_engine/ingest/ingestsession.{h,cpp}` — `augmentSrtUrl(QUrl)` (adds the SRT options to the URL query) and `jitterWindowMs(scheme, srtFloorMs, defaultMs)` (transport→window). The ffmpeg session calls `augmentSrtUrl`; the `StreamWorker` sets a per-source `m_activeJitterWindowMs` from the URL scheme and reads it at the three tick sites. Shared `kSrtLatencyMs`/`kSrtConnectTimeoutMs` constants replace the native path's private duplicates.

**Tech Stack:** C++17, Qt6 Core (QUrl/QUrlQuery), FFmpeg libsrt (URL-query options), libsrt (native srt_setsockopt), Qt Test, bash e2e, CMake/Ninja/CTest.

**Spec:** `docs/superpowers/specs/2026-06-16-srt-jitter-latency-jit1-design.md`

**Base branch:** `feat/srt-jit1-jitter` (off `origin/main` = #40+#39). Build dir: `build/srt` (configured with `-DOLR_FFMPEG_SRT_PREFIX`). **Local-only; do not push** unless told.

**ffmpeg libsrt units (verified against `macos_build/src/ffmpeg-8.0/libavformat/libsrt.c`):**
- `latency`/`rcvlatency`/`peerlatency` = **microseconds** (libsrt.c:125-128); ffmpeg divides by 1000 → `SRTO_LATENCY` ms (libsrt.c:322-324). So 500 ms ⇒ query value **`500000`**. (The current `latency=500` would be 500µs/1000 = **0 ms**.)
- `connect_timeout` = **milliseconds** (libsrt.c:131, passed directly). So 5 s ⇒ **`5000`**. (The current `5000000` would be 5000 s.)
- `transtype=live`, `linger=0` (seconds) — correct as-is.

---

### Task 1: Shared constants + pure helpers (`augmentSrtUrl`, `jitterWindowMs`) + unit test

**Files:**
- Modify: `recorder_engine/ingest/ingestsession.h`
- Modify: `recorder_engine/ingest/ingestsession.cpp`
- Create: `tests/unit/tst_srt_options.cpp`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/unit/tst_srt_options.cpp`:

```cpp
#include <QtTest>
#include <QUrl>
#include <QUrlQuery>

#include "recorder_engine/ingest/ingestsession.h"

class TestSrtOptions : public QObject {
    Q_OBJECT
private slots:
    void srtUrlGetsAllOptions();
    void latencyIsMicrosecondsForFfmpeg();
    void connectTimeoutIsMilliseconds();
    void nonSrtUrlUntouched();
    void existingOptionPreserved();
    void jitterWindowSrtUsesFloor();
    void jitterWindowNonSrtUsesDefault();
};

void TestSrtOptions::srtUrlGetsAllOptions() {
    const QUrlQuery q(augmentSrtUrl(QUrl(QStringLiteral("srt://127.0.0.1:9000"))));
    QVERIFY(q.hasQueryItem(QStringLiteral("latency")));
    QVERIFY(q.hasQueryItem(QStringLiteral("rcvlatency")));
    QVERIFY(q.hasQueryItem(QStringLiteral("peerlatency")));
    QCOMPARE(q.queryItemValue(QStringLiteral("transtype")), QStringLiteral("live"));
    QVERIFY(q.hasQueryItem(QStringLiteral("connect_timeout")));
    QCOMPARE(q.queryItemValue(QStringLiteral("linger")), QStringLiteral("0"));
}

void TestSrtOptions::latencyIsMicrosecondsForFfmpeg() {
    // kSrtLatencyMs is milliseconds; ffmpeg's libsrt latency option is microseconds
    // (divided by 1000 -> SRTO_LATENCY ms), so the query must carry ms*1000.
    const QUrlQuery q(augmentSrtUrl(QUrl(QStringLiteral("srt://127.0.0.1:9000"))));
    const QString expected = QString::number(qint64(kSrtLatencyMs) * 1000);
    QCOMPARE(q.queryItemValue(QStringLiteral("latency")), expected);
    QCOMPARE(q.queryItemValue(QStringLiteral("rcvlatency")), expected);
    QCOMPARE(q.queryItemValue(QStringLiteral("peerlatency")), expected);
}

void TestSrtOptions::connectTimeoutIsMilliseconds() {
    // ffmpeg's connect_timeout is milliseconds (passed straight to SRTO_CONNTIMEO).
    const QUrlQuery q(augmentSrtUrl(QUrl(QStringLiteral("srt://127.0.0.1:9000"))));
    QCOMPARE(q.queryItemValue(QStringLiteral("connect_timeout")),
             QString::number(kSrtConnectTimeoutMs));
}

void TestSrtOptions::nonSrtUrlUntouched() {
    const QUrl in(QStringLiteral("udp://127.0.0.1:1234"));
    QCOMPARE(augmentSrtUrl(in), in);
    const QUrl rtmp(QStringLiteral("rtmp://h/live/a"));
    QCOMPARE(augmentSrtUrl(rtmp), rtmp);
}

void TestSrtOptions::existingOptionPreserved() {
    // A user-supplied option wins (same rule as the existing linger guard).
    const QUrlQuery q(augmentSrtUrl(QUrl(QStringLiteral("srt://127.0.0.1:9000?latency=200000"))));
    QCOMPARE(q.queryItemValue(QStringLiteral("latency")), QStringLiteral("200000"));
    QCOMPARE(q.allQueryItemValues(QStringLiteral("latency")).size(), 1); // no duplicate key
}

void TestSrtOptions::jitterWindowSrtUsesFloor() {
    QCOMPARE(jitterWindowMs(QStringLiteral("srt"), 80, 200), 80);
}

void TestSrtOptions::jitterWindowNonSrtUsesDefault() {
    QCOMPARE(jitterWindowMs(QStringLiteral("udp"), 80, 200), 200);
    QCOMPARE(jitterWindowMs(QStringLiteral("rtmp"), 80, 200), 200);
    QCOMPARE(jitterWindowMs(QString(), 80, 200), 200);
}

QTEST_GUILESS_MAIN(TestSrtOptions)
#include "tst_srt_options.moc"
```

- [ ] **Step 2: Register the test** — in `tests/unit/CMakeLists.txt`, after the line `olr_add_unit_test(tst_ingestbackendselector olr_test_core)`:

```cmake
olr_add_unit_test(tst_srt_options olr_test_core)
```

- [ ] **Step 3: Run it to verify it FAILS to build** (helpers/constants undeclared)

Run: `cmake -S . -B build/srt >/dev/null && cmake --build build/srt --target tst_srt_options`
Expected: compile error — `augmentSrtUrl` / `jitterWindowMs` / `kSrtLatencyMs` not declared.

- [ ] **Step 4: Declare the constants + helpers** in `recorder_engine/ingest/ingestsession.h`.

(`<QUrl>` and `<QString>` are already included.) After `constexpr int kDecodedAudioBytesPerSample = ...;` (the existing global constant), add:

```cpp
// Shared SRT receive latency / connect timeout (milliseconds), used by BOTH ingest
// paths: the native path passes them straight to srt_setsockopt (SRTO_LATENCY /
// SRTO_CONNTIMEO, which are milliseconds); the ffmpeg path puts them in the URL query
// via augmentSrtUrl() (note: ffmpeg's latency options are MICROSECONDS — see there).
constexpr int kSrtLatencyMs = 500;
constexpr int kSrtConnectTimeoutMs = 5000;
```

After the `selectIngestBackend(...)` declaration, add:

```cpp
// Append SRT-private options to an srt:// URL's query so they actually apply.
// FFmpeg's libsrt reads these via the URL query (av_find_info_tag); set on the
// avformat_open_input() opts dict they are silently ignored. Non-srt URLs are
// returned unchanged; an option already present in the query is left as-is (a
// user override wins). UNITS: ffmpeg's latency/rcvlatency/peerlatency are
// MICROSECONDS (it divides by 1000 -> SRTO_LATENCY ms), so they carry
// kSrtLatencyMs*1000; connect_timeout is milliseconds; linger is seconds.
QUrl augmentSrtUrl(const QUrl& url);

// Per-transport engine jitter window: SRT sources lean on SRT's TSBPD reorder
// buffer, so they need only a small residual floor; other transports get the
// default. Returns srtFloorMs for scheme "srt" (case-insensitive), else defaultMs.
int jitterWindowMs(const QString& scheme, int srtFloorMs, int defaultMs);
```

(`QUrlQuery` is used only in the .cpp.)

- [ ] **Step 5: Implement both helpers** — append to `recorder_engine/ingest/ingestsession.cpp` (add `#include <QUrlQuery>` near the top includes):

```cpp
QUrl augmentSrtUrl(const QUrl& url) {
    if (url.scheme().toLower() != QStringLiteral("srt")) {
        return url;
    }
    QUrl out = url;
    QUrlQuery query(out);
    const auto addIfAbsent = [&query](const QString& key, const QString& value) {
        if (!query.hasQueryItem(key)) query.addQueryItem(key, value);
    };
    // ffmpeg latency options are microseconds (-> /1000 -> SRTO_LATENCY ms).
    const QString latencyUs = QString::number(qint64(kSrtLatencyMs) * 1000);
    addIfAbsent(QStringLiteral("latency"), latencyUs);
    addIfAbsent(QStringLiteral("rcvlatency"), latencyUs);
    addIfAbsent(QStringLiteral("peerlatency"), latencyUs);
    addIfAbsent(QStringLiteral("transtype"), QStringLiteral("live"));
    addIfAbsent(QStringLiteral("connect_timeout"), QString::number(kSrtConnectTimeoutMs));
    addIfAbsent(QStringLiteral("linger"), QStringLiteral("0"));
    out.setQuery(query);
    return out;
}

int jitterWindowMs(const QString& scheme, int srtFloorMs, int defaultMs) {
    return scheme.toLower() == QStringLiteral("srt") ? srtFloorMs : defaultMs;
}
```

- [ ] **Step 6: Build + run the test, expect PASS**

Run: `cmake --build build/srt --target tst_srt_options && ( cd build/srt && ctest -R tst_srt_options --output-on-failure )`
Expected: PASS, 7 test functions.

- [ ] **Step 7: Format the changed lines only** (the ingest files have pre-existing clang-format drift — do NOT run `clang-format -i` on the whole file; verify only your added lines conform: `/opt/homebrew/opt/llvm/bin/clang-format <file>` diffed against the file should not list your lines). Then commit:

```bash
git add recorder_engine/ingest/ingestsession.h recorder_engine/ingest/ingestsession.cpp \
        tests/unit/tst_srt_options.cpp tests/unit/CMakeLists.txt
git commit -m "feat(srt): augmentSrtUrl() + jitterWindowMs() helpers + shared SRT constants (unit-tested)"
```

---

### Task 2: Component 1 — apply the SRT options (ffmpeg path) + DRY the native constants

**Files:**
- Modify: `recorder_engine/ingest/ffmpegingestsession.cpp:381-394`
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp:22-23`

- [ ] **Step 1: ffmpeg path — call `augmentSrtUrl`, drop the inert dict options.** In `ffmpegingestsession.cpp`, replace the entire `if (scheme == "srt") { ... }` block (lines 381-394) with:

```cpp
    if (scheme == "srt") {
        // SRT-private options (latency/rcvlatency/peerlatency, transtype,
        // connect_timeout, linger) only take effect via the URL query. Setting them
        // on the avformat_open_input() opts dict silently drops them, so SRT ran at
        // its defaults (and the old latency=500 would have meant 500us -> 0ms).
        currentUrl = augmentSrtUrl(currentUrl);
    }
```

This removes the five inert `av_dict_set(&opts, "connect_timeout"/"latency"/"rcvlatency"/"peerlatency"/"transtype", ...)` calls (now carried in the query) and folds in the existing `linger=0` (augmentSrtUrl adds it). Leave the generic dict options above (`rw_timeout`, `timeout`, `recv_buffer_size`, `fflags`, `probesize`, `analyzeduration`) untouched — those are not SRT-private and do propagate.

Ensure `augmentSrtUrl` is visible: `ffmpegingestsession.cpp` includes `ffmpegingestsession.h` which includes `ingest/ingestsession.h`. If the build can't find `augmentSrtUrl`, add `#include "ingestsession.h"` to `ffmpegingestsession.cpp`.

- [ ] **Step 2: native path — use the shared constants.** In `nativesrtingestsession.cpp`, delete the two private duplicates from the anonymous namespace (lines 22-23):

```cpp
constexpr int kSrtLatencyMs = 500;
constexpr int kSrtConnectTimeoutMs = 5000;
```

They are now provided by `ingestsession.h` (included via `nativesrtingestsession.h`). The existing uses (`const int latency = kSrtLatencyMs;`, `const int connectTimeout = kSrtConnectTimeoutMs;`, and `connectTimer.elapsed() > kSrtConnectTimeoutMs`) resolve to the shared constants unchanged — the native path still sets `SRTO_LATENCY=500` (ms) directly, which is correct.

- [ ] **Step 3: Build**

Run: `cmake --build build/srt --target sync_harness record_harness`
Expected: clean. If `kSrtLatencyMs` is now ambiguous/undeclared in the native TU, confirm `ingestsession.h` is included via `nativesrtingestsession.h` and the anon-namespace duplicates were removed.

- [ ] **Step 4: Verify the ffmpeg SRT path still ingests (options now in effect)** — run the ffmpeg-path SRT gates:

Run: `( cd build/srt && ctest -L srt --output-on-failure )`
Expected: 5/5 pass (smoke/4cam/sync/trim/connect). `transtype=live` and `latency=500000` now actually apply; ingestion must not regress. If a gate fails, capture the error and report (do NOT loosen) — a real connect/timeout unit error would surface here.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/ingest/ffmpegingestsession.cpp recorder_engine/ingest/nativesrtingestsession.cpp
git commit -m "fix(srt): apply SRT latency/transtype/connect_timeout via URL query (ffmpeg path)"
```

---

### Task 3: Component 2 — per-transport engine jitter window

**Files:**
- Modify: `recorder_engine/streamworker.h`
- Modify: `recorder_engine/streamworker.cpp`

- [ ] **Step 1: streamworker.h — constant, member, signatures.**

After `static constexpr int kJitterBufferMs = 200;` (line 37) add:

```cpp
    // SRT sources lean on SRT's TSBPD reorder buffer, so the engine only needs a
    // small residual window instead of the full kJitterBufferMs. Env-overridable
    // (OLR_SRT_JITTER_MS) for tuning/validation. Non-SRT transports keep 200.
    static constexpr int kSrtJitterFloorMs = 80;
```

Near `std::atomic<int> m_trimOffsetMs{0};` (line 132) add:

```cpp
    // Per-source jitter window (ms), chosen by transport in captureLoop and read by
    // the tick thread. Defaults to kJitterBufferMs until the URL is resolved.
    std::atomic<int> m_activeJitterWindowMs{kJitterBufferMs};
```

Update the two method declarations (add `int64_t jitterMs`):

```cpp
    void writeAudioForTick(int64_t recordingTimeMs, int track, int64_t trimMs, int64_t jitterMs);
```
```cpp
    void processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs, int64_t trimMs,
                            int64_t jitterMs);
```

- [ ] **Step 2: streamworker.cpp — set the window per source in `captureLoop`.** Right after the URL is resolved (line 213, `{ QMutexLocker locker(&m_urlMutex); currentUrl = m_url; }`), add:

```cpp
        {
            // Right-size the jitter window for this source's transport. SRT (native
            // or ffmpeg) pre-buffers via TSBPD, so it needs only a small floor.
            int srtFloor = kSrtJitterFloorMs;
            const int envFloor = qEnvironmentVariableIntValue("OLR_SRT_JITTER_MS");
            if (envFloor > 0) srtFloor = envFloor;
            m_activeJitterWindowMs.store(
                jitterWindowMs(QUrl(currentUrl).scheme().toLower(), srtFloor, kJitterBufferMs),
                std::memory_order_relaxed);
        }
```

(`jitterWindowMs` comes from `ingest/ingestsession.h`, already included by `streamworker.cpp`. `QUrl` is already included.)

- [ ] **Step 3: streamworker.cpp — snapshot the window in `onMasterPulse` and use/pass it.**

After `const int64_t trimMs = m_trimOffsetMs.load(std::memory_order_relaxed);` (line 81) add:

```cpp
    // Snapshot the jitter window once per pulse too, so video + audio of this tick
    // share one value even if a URL change flips it mid-pulse (keeps A/V locked).
    const int64_t jitterMs = m_activeJitterWindowMs.load(std::memory_order_relaxed);
```

Change the gate publish (line 86) from `- kJitterBufferMs - trimMs` to:

```cpp
        qMax<int64_t>(0, (frameIndex * 1000) / m_targetFps - jitterMs - trimMs),
```

Change the `processEncoderTick` call (line 112) to pass `jitterMs`:

```cpp
    processEncoderTick(m_persistentEncCtx, streamTimeMs, trimMs, jitterMs);
```

- [ ] **Step 4: streamworker.cpp — use `jitterMs` in `processEncoderTick`.** Update the definition signature (lines 115-116):

```cpp
void StreamWorker::processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs,
                                      int64_t trimMs, int64_t jitterMs) {
```

Change the video dequeue target (line 141) from `- kJitterBufferMs - trimMs` to:

```cpp
        int64_t targetTimeMs = currentRecordingTimeMs - jitterMs - trimMs;
```

Change the `writeAudioForTick` call (line 200) to pass `jitterMs`:

```cpp
    writeAudioForTick(currentRecordingTimeMs, track, trimMs, jitterMs);
```

- [ ] **Step 5: streamworker.cpp — use `jitterMs` in `writeAudioForTick`.** Update the definition signature (line 496):

```cpp
void StreamWorker::writeAudioForTick(int64_t recordingTimeMs, int track, int64_t trimMs,
                                     int64_t jitterMs) {
```

Change the `jitterSamples` computation (line ~502) from `int64_t(kJitterBufferMs)` to:

```cpp
    const int64_t jitterSamples = jitterMs * kAudioSampleRate / 1000;
```

- [ ] **Step 6: Build**

Run: `cmake --build build/srt --target sync_harness record_harness`
Expected: clean compile + link. (`kJitterBufferMs` is still referenced by the new member initializer and the env fallback, so it stays defined.)

- [ ] **Step 7: Verify no regression on non-SRT + that the SRT floor is safe.**

a) UDP/non-SRT path unchanged (still 200ms): run the standard record e2e:
Run: `( cd build/srt && ctest -L e2e -R "e2e_record_stereo|e2e_record_mono" --output-on-failure )`
Expected: pass.

b) SRT continuity at the 80ms floor — run the native continuity gate **scripts directly** (their CTest registration on Apple is restored only by PR #41, but the scripts run standalone). Each asserts full flash counts + no gap > 1.5s, so a too-small window stutters and fails:
Run:
```bash
H="build/srt/tests/e2e/sync_harness"
R="build/srt/tests/e2e/record_harness"
OLR_NATIVE_SRT=1 bash tests/e2e/run_srt_4cam.sh "$H" 24010
OLR_NATIVE_SRT=1 bash tests/e2e/run_srt_soak.sh "$H" 24020
OLR_NATIVE_SRT=1 bash tests/e2e/run_srt_loss.sh "$H" 24030
OLR_NATIVE_SRT=1 bash tests/e2e/run_srt_jitter.sh "$H" 24040
```
Expected: each prints `PASS`. If any shows a content gap (stutter from too-small a window), re-run with `OLR_SRT_JITTER_MS=120` to confirm headroom and report — the floor constant may need raising from 80 to a measured-safe value (change `kSrtJitterFloorMs` and note it). Do NOT weaken the gate assertions.

- [ ] **Step 8: Format changed lines only + commit**

```bash
git add recorder_engine/streamworker.h recorder_engine/streamworker.cpp
git commit -m "feat(srt): per-transport engine jitter window (small floor for SRT)"
```

---

### Task 4: Document the change

**Files:**
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Append a section** to `tests/e2e/SRT_README.md`:

````markdown
## JIT-1: per-transport jitter window + effective SRT options

**Effective SRT options (ffmpeg path).** SRT-private options must ride the URL query;
on the `avformat_open_input` opts dict they are silently dropped. `augmentSrtUrl()`
(`recorder_engine/ingest/ingestsession.cpp`) now adds them: `latency`/`rcvlatency`/
`peerlatency` (ffmpeg units are **microseconds** -> `kSrtLatencyMs*1000`), `transtype=live`,
`connect_timeout` (**ms**), `linger=0`. The native path sets the same `kSrtLatencyMs` /
`kSrtConnectTimeoutMs` via `srt_setsockopt` directly (those APIs are milliseconds).

**Per-transport jitter window.** The engine holds frames a jitter window in the past before
encoding. SRT sources lean on SRT's TSBPD reorder buffer, so they use a small floor
(`kSrtJitterFloorMs`, default 80 ms, env-overridable via `OLR_SRT_JITTER_MS`); raw UDP/RTMP
keep `kJitterBufferMs` (200 ms). The `StreamWorker` picks the window from the URL scheme.

**Tests:** `tst_srt_options` (unit — `augmentSrtUrl` option/unit set, `jitterWindowMs` mapping);
`ctest -L srt` proves the ffmpeg options don't break ingest; the native continuity gate scripts
(`run_srt_soak.sh`/`run_srt_loss.sh`/`run_srt_jitter.sh`/`run_srt_4cam.sh`, `OLR_NATIVE_SRT=1`)
prove the reduced floor keeps content continuous (no gaps).
````

- [ ] **Step 2: Commit**

```bash
git add tests/e2e/SRT_README.md
git commit -m "docs(srt): document JIT-1 per-transport jitter window + effective SRT options"
```

---

## After all tasks

- `( cd build/srt && ctest -L unit --output-on-failure )` — all unit tests incl. `tst_srt_options`.
- `( cd build/srt && ctest -L srt --output-on-failure )` — ffmpeg SRT path 5/5.
- The native continuity scripts from Task 3 Step 7b — SRT floor safe.
- Dispatch a final code review over the whole branch.
- Use superpowers:finishing-a-development-branch. **Do NOT push** unless explicitly told.

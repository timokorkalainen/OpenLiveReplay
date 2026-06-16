# Native RTMP Broadcast Ingest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring native RTMP/RTMPS ingest to a dependable live-broadcast profile before merge, including legacy AVC/AAC RTMP and E-RTMP HEVC/AAC with robust parsing, fallback, interop, and soak gates.

**Architecture:** Keep `StreamWorker` as the timeline/queue/mux owner and move RTMP protocol/container/codec work into focused native ingest helpers. Harden `RtmpChunkParser` and AMF0 first, then add FLV/E-RTMP video parsing, AVC/HEVC codec config conversion, reconnect/fallback policy, hostile fixtures, real-server interop, and soak gates. Native RTMP remains opt-in until every readiness gate passes.

**Tech Stack:** C++17, Qt 6.10, QtTest, CMake/CTest, Python 3 fixtures, FFmpeg/ffprobe for fixture generation and output inspection, Apple VideoToolbox and AudioToolbox for native decode, RTMPS through `QSslSocket`.

**Spec:** `docs/superpowers/specs/2026-06-16-native-rtmp-broadcast-ingest-design.md`

---

## Before You Start

- Work only in the local worktree:
  `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.worktrees/native-rtmp-ingest`
- Do not push. The user explicitly requested local-only work.
- Treat the current native RTMP code as an in-progress draft. Keep useful pieces, but do not preserve behavior that conflicts with this plan.
- Use test-first development for every production change.
- Commit locally after each completed task or tightly related group of steps.
- Keep native RTMP opt-in until Task 11.
- Use this build directory unless there is a good reason to reconfigure:
  `build/native-rtmp`

Common commands:

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol tst_ingestbackendselector record_harness sync_harness OpenLiveReplay -j2
ctest --test-dir build/native-rtmp -R 'tst_rtmpprotocol|tst_ingestbackendselector' --output-on-failure
ctest --test-dir build/native-rtmp -L native-rtmp --output-on-failure
git diff --check
```

---

## File Map

- `recorder_engine/ingest/rtmpprotocol.{h,cpp}`: AMF0 helpers, RTMP chunk parser/writer, FLV/E-RTMP video parsing, AVC/HEVC config conversion.
- `recorder_engine/ingest/nativertmpingestsession.{h,cpp}`: socket lifecycle, RTMP command flow, capability signaling, stream selection, decode orchestration, reconnect/fallback reasons, and callbacks.
- `recorder_engine/ingest/ingestsession.{h,cpp}`: backend kind, environment/default selection, native failure policy helpers.
- `recorder_engine/streamworker.cpp`: source retry loop, native-vs-FFmpeg backend selection, fallback after unsupported native profile.
- `recorder_engine/ingest/h26xaccessunit.{h,cpp}`: existing Annex B H.264/H.265 structures used by VideoToolbox.
- `recorder_engine/ingest/videotoolboxdecoder.mm`: existing Apple H.264/HEVC decoder boundary.
- `recorder_engine/ingest/audiotoolboxaacdecoder.mm`: existing AAC-LC-to-48k stereo decoder boundary.
- `tests/unit/tst_rtmpprotocol.cpp`: adversarial parser, AMF0, FLV/E-RTMP, AVC/HEVC config tests.
- `tests/unit/tst_ingestbackendselector.cpp`: opt-in/default/fallback policy tests.
- `tests/e2e/rtmp_fixture_server.py`: deterministic RTMP/RTMPS fixture server, hostile fragmentation/reconnect/auth/E-RTMP modes.
- `tests/e2e/rtmp_lib.sh`: shared fixture generation/server helpers.
- `tests/e2e/run_rtmp_*.sh`: RTMP/RTMPS E2E gates.
- `tests/e2e/CMakeLists.txt`: local-only native RTMP CTest registration.
- `tests/README.md`: local gate documentation and real-server/soak instructions.
- `.github/workflows/ci.yml`: keep local native RTMP gates excluded from CI until explicitly promoted.

---

### Task 1: Freeze Rollout Policy Before Hardening

Native RTMP must remain opt-in while broadcast-readiness gaps are being closed. This prevents current RTMP users from being moved onto the draft native path.

**Files:**
- Modify: `recorder_engine/ingest/ingestsession.cpp`
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `tests/unit/tst_ingestbackendselector.cpp`
- Modify: `tests/README.md`

- [ ] **Step 1: Write failing policy tests**

In `tests/unit/tst_ingestbackendselector.cpp`, add:

```cpp
void environmentKeepsRtmpOnFfmpegByDefaultUntilReady();
void environmentOptInRoutesRtmpAndRtmpsToNative();
```

Implement:

```cpp
void TestIngestBackendSelector::environmentKeepsRtmpOnFfmpegByDefaultUntilReady() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qunsetenv("OLR_NATIVE_RTMP");
    qunsetenv("OLR_FFMPEG_RTMP");

    IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")),
                                            false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::Ffmpeg);

    opts = ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmps://example.test/live/a")),
                                               false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::Ffmpeg);
}

void TestIngestBackendSelector::environmentOptInRoutesRtmpAndRtmpsToNative() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qputenv("OLR_NATIVE_RTMP", "1");
    qunsetenv("OLR_FFMPEG_RTMP");

    IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")),
                                            false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);

    opts = ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmps://example.test/live/a")),
                                               false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::NativeRtmp);
}
```

- [ ] **Step 2: Run red**

Run:

```bash
cmake --build build/native-rtmp --target tst_ingestbackendselector -j2
ctest --test-dir build/native-rtmp -R tst_ingestbackendselector --output-on-failure
```

Expected: test fails because current branch routes RTMP/RTMPS to native by default.

- [ ] **Step 3: Implement opt-in policy**

In `recorder_engine/ingest/ingestsession.cpp`, make `preferNativeRtmp` require explicit opt-in:

```cpp
bool nativeRtmpEnabledByEnvironment() {
    if (qEnvironmentVariableIsSet("OLR_FFMPEG_RTMP")) {
        return false;
    }
    const QString value = qEnvironmentVariable("OLR_NATIVE_RTMP").trimmed().toLower();
    return value == QStringLiteral("1") || value == QStringLiteral("true") ||
           value == QStringLiteral("on") || value == QStringLiteral("yes");
}
```

Use:

```cpp
options.preferNativeRtmp = nativeRtmpAvailable &&
                           (scheme == QStringLiteral("rtmp") ||
                            scheme == QStringLiteral("rtmps")) &&
                           nativeRtmpEnabledByEnvironment();
```

- [ ] **Step 4: Update docs**

In `tests/README.md`, replace the native-default wording with:

```markdown
Native RTMP/RTMPS is opt-in while broadcast-readiness gates are being hardened.
Set `OLR_NATIVE_RTMP=1` to use it locally. `OLR_FFMPEG_RTMP=1` forces the
legacy FFmpeg RTMP path when comparing behavior.
```

- [ ] **Step 5: Verify green**

Run:

```bash
cmake --build build/native-rtmp --target tst_ingestbackendselector OpenLiveReplay -j2
ctest --test-dir build/native-rtmp -R tst_ingestbackendselector --output-on-failure
```

Expected: selector tests pass.

- [ ] **Step 6: Commit**

```bash
git add recorder_engine/ingest/ingestsession.cpp tests/unit/tst_ingestbackendselector.cpp tests/README.md recorder_engine/streamworker.cpp
git commit -m "fix: keep native rtmp opt-in during hardening"
```

---

### Task 2: Make RTMP Chunk Parsing Transactional

Fix the parser bug where incomplete payload bytes can mutate previous header state and double-apply timestamp deltas.

**Files:**
- Modify: `recorder_engine/ingest/rtmpprotocol.h`
- Modify: `recorder_engine/ingest/rtmpprotocol.cpp`
- Modify: `tests/unit/tst_rtmpprotocol.cpp`

- [ ] **Step 1: Write failing fragmentation test**

Add test slot:

```cpp
void chunkParserDoesNotDoubleApplyTimestampDeltaWhenPayloadArrivesLater();
```

Test body:

```cpp
void TestRtmpProtocol::chunkParserDoesNotDoubleApplyTimestampDeltaWhenPayloadArrivesLater() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray first =
        RtmpChunkWriter::message(6, 9, 1, 1000, QByteArray("abcd", 4), 128);
    QVERIFY(parser.push(first, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, 1000);

    QByteArray second;
    second.append(char((1 << 6) | 6)); // fmt=1, csid=6
    second.append(QByteArray::fromHex("000028")); // timestamp delta 40
    second.append(QByteArray::fromHex("000004")); // payload length 4
    second.append(char(9));                       // message type video
    second.append("xy", 2);                       // incomplete payload

    QVERIFY(parser.push(second, &messages, &error));
    QVERIFY(messages.isEmpty());

    QVERIFY(parser.push(QByteArray("zz", 2), &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, 1040);
    QCOMPARE(messages.first().payload, QByteArray("xyzz", 4));
}
```

- [ ] **Step 2: Write failing fmt=3 extended timestamp test**

Add test slot:

```cpp
void chunkParserConsumesExtendedTimestampOnContinuationChunks();
```

Test body:

```cpp
void TestRtmpProtocol::chunkParserConsumesExtendedTimestampOnContinuationChunks() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray payload("abcdef", 6);
    const QByteArray chunks = RtmpChunkWriter::message(6, 9, 1, 0x1000000, payload, 3);

    QVERIFY(parser.push(chunks, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, qint64(0x1000000));
    QCOMPARE(messages.first().payload, payload);
}
```

- [ ] **Step 3: Run red**

Run:

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

Expected: at least one of the new parser tests fails.

- [ ] **Step 4: Implement transactional parse**

In `RtmpChunkParser::push`, parse from `m_buffer` into local variables first. Only commit `m_previousHeaders`, `m_assemblies`, `m_inputChunkSize`, and `m_buffer.remove(0, consumed)` after the full chunk fragment is available.

Required structure:

```cpp
struct ParsedChunkFragment {
    int consumed = 0;
    int csid = 0;
    ChunkHeader header;
    QByteArray fragment;
    bool startsMessage = false;
};
```

Add private helper in `rtmpprotocol.cpp`:

```cpp
bool RtmpChunkParser::tryParseFragment(ParsedChunkFragment* fragment, QString* error) const;
```

If adding private helpers to the header is too noisy, keep a lambda inside `push`, but it must use local copies and return “need more” without mutating parser members.

Commit rules:

```cpp
ChunkAssembly assembly = m_assemblies.value(fragment.csid);
if (fragment.startsMessage || assembly.payload.isEmpty()) {
    assembly.header = fragment.header;
}
assembly.payload.append(fragment.fragment);
m_previousHeaders.insert(fragment.csid, fragment.header);
m_buffer.remove(0, fragment.consumed);
```

For extended timestamps:

- fmt 0/1/2 set `header.usesExtendedTimestamp = true` when the timestamp field is `0xffffff`.
- fmt 3 inherits whether the previous header used extended timestamps.
- If `usesExtendedTimestamp` is true, consume four extended timestamp bytes for every chunk header including fmt 3.

Add `bool usesExtendedTimestamp = false;` to `ChunkHeader`.

- [ ] **Step 5: Verify green**

Run:

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

Expected: all RTMP protocol tests pass.

- [ ] **Step 6: Commit**

```bash
git add recorder_engine/ingest/rtmpprotocol.h recorder_engine/ingest/rtmpprotocol.cpp tests/unit/tst_rtmpprotocol.cpp
git commit -m "fix: make rtmp chunk parsing transactional"
```

---

### Task 3: Add Parser Limits, Abort Handling, And Control Message Coverage

Prevent malformed or hostile RTMP streams from growing memory indefinitely.

**Files:**
- Modify: `recorder_engine/ingest/rtmpprotocol.h`
- Modify: `recorder_engine/ingest/rtmpprotocol.cpp`
- Modify: `tests/unit/tst_rtmpprotocol.cpp`
- Modify: `recorder_engine/ingest/nativertmpingestsession.cpp`

- [ ] **Step 1: Write failing max-message-size test**

Add test slot:

```cpp
void chunkParserRejectsMessagesOverConfiguredLimit();
```

Test:

```cpp
void TestRtmpProtocol::chunkParserRejectsMessagesOverConfiguredLimit() {
    RtmpChunkParser parser;
    parser.setMaxMessageSize(4);
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray bytes = RtmpChunkWriter::message(6, 9, 1, 0, QByteArray("abcde", 5), 128);
    QVERIFY(!parser.push(bytes, &messages, &error));
    QVERIFY(error.contains(QStringLiteral("exceeds")));
}
```

- [ ] **Step 2: Write failing abort-clears-assembly test**

Add test slot:

```cpp
void chunkParserAbortClearsInFlightAssembly();
```

Test:

```cpp
void TestRtmpProtocol::chunkParserAbortClearsInFlightAssembly() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    parser.setInputChunkSizeForTest(2);
    const QByteArray video = RtmpChunkWriter::message(6, 9, 1, 0, QByteArray("abcdef", 6), 2);
    QVERIFY(parser.push(video.left(14), &messages, &error));
    QVERIFY(messages.isEmpty());

    QByteArray abortPayload;
    abortPayload.append(char(0));
    abortPayload.append(char(0));
    abortPayload.append(char(0));
    abortPayload.append(char(6));
    const QByteArray abort = RtmpChunkWriter::message(2, 2, 0, 0, abortPayload, 128);
    QVERIFY(parser.push(abort, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().type, 2);

    QVERIFY(parser.push(video.mid(14), &messages, &error));
    QVERIFY(messages.isEmpty());
}
```

- [ ] **Step 3: Run red**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

Expected: tests fail because parser limits/abort handling are missing.

- [ ] **Step 4: Add parser limit API**

In `rtmpprotocol.h`, add:

```cpp
void setMaxMessageSize(int bytes) { m_maxMessageSize = qMax(1, bytes); }
void setMaxBufferedBytes(int bytes) { m_maxBufferedBytes = qMax(1, bytes); }
void setInputChunkSizeForTest(int bytes) { m_inputChunkSize = qMax(1, bytes); }
```

Private members:

```cpp
int m_maxMessageSize = 16 * 1024 * 1024;
int m_maxBufferedBytes = 4 * 1024 * 1024;
```

- [ ] **Step 5: Enforce limits and abort**

In `push`:

```cpp
if (m_buffer.size() + bytes.size() > m_maxBufferedBytes) {
    if (error) *error = QStringLiteral("RTMP buffered bytes exceed limit.");
    return false;
}
```

After parsing a header:

```cpp
if (header.messageLength > m_maxMessageSize) {
    if (error) {
        *error = QStringLiteral("RTMP message length %1 exceeds limit %2.")
                     .arg(header.messageLength)
                     .arg(m_maxMessageSize);
    }
    return false;
}
```

When a complete Abort message is emitted:

```cpp
if (message.type == 2 && message.payload.size() >= 4) {
    const int abortCsid = int(readU32Be(message.payload.constData()));
    m_assemblies.remove(abortCsid);
}
```

- [ ] **Step 6: Verify**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/ingest/rtmpprotocol.h recorder_engine/ingest/rtmpprotocol.cpp tests/unit/tst_rtmpprotocol.cpp
git commit -m "feat: bound rtmp parser state"
```

---

### Task 4: Expand AMF0 For E-RTMP Connect And Metadata

Add enough AMF0 support for E-RTMP capability signaling and metadata skipping.

**Files:**
- Modify: `recorder_engine/ingest/rtmpprotocol.h`
- Modify: `recorder_engine/ingest/rtmpprotocol.cpp`
- Modify: `tests/unit/tst_rtmpprotocol.cpp`

- [ ] **Step 1: Write failing strict-array writer test**

Add test slot:

```cpp
void amf0WritesStrictArrayForFourCcList();
```

Test:

```cpp
void TestRtmpProtocol::amf0WritesStrictArrayForFourCcList() {
    const QByteArray array = RtmpAmf0::strictArray({
        RtmpAmf0::string(QStringLiteral("avc1")),
        RtmpAmf0::string(QStringLiteral("hvc1")),
        RtmpAmf0::string(QStringLiteral("mp4a")),
    });

    QCOMPARE(uchar(array[0]), 0x0a);
    QCOMPARE(array.mid(1, 4).toHex(), QByteArray("00000003"));
    int offset = 5;
    QString value;
    QVERIFY(RtmpAmf0::readString(array, &offset, &value));
    QCOMPARE(value, QStringLiteral("avc1"));
    QVERIFY(RtmpAmf0::readString(array, &offset, &value));
    QCOMPARE(value, QStringLiteral("hvc1"));
    QVERIFY(RtmpAmf0::readString(array, &offset, &value));
    QCOMPARE(value, QStringLiteral("mp4a"));
    QCOMPARE(offset, array.size());
}
```

- [ ] **Step 2: Write failing ECMA-array skip test**

Add:

```cpp
void amf0SkipsEcmaArrayMetadata();
```

Test:

```cpp
void TestRtmpProtocol::amf0SkipsEcmaArrayMetadata() {
    QByteArray metadata;
    metadata.append(char(0x08));
    metadata.append(QByteArray::fromHex("00000002"));
    metadata.append(QByteArray::fromHex("0005"));
    metadata.append("width", 5);
    metadata.append(RtmpAmf0::number(1920));
    metadata.append(QByteArray::fromHex("000c"));
    metadata.append("videocodecid", 12);
    metadata.append(RtmpAmf0::string(QStringLiteral("hvc1")));
    metadata.append("\0\0\x09", 3);

    int offset = 0;
    QVERIFY(RtmpAmf0::skipValue(metadata, &offset));
    QCOMPARE(offset, metadata.size());
}
```

- [ ] **Step 3: Run red**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

- [ ] **Step 4: Implement AMF0 strict arrays and ECMA-array skip**

In `rtmpprotocol.h`:

```cpp
QByteArray strictArray(const QList<QByteArray>& values);
```

In `rtmpprotocol.cpp`:

```cpp
QByteArray RtmpAmf0::strictArray(const QList<QByteArray>& values) {
    QByteArray out;
    out.append(char(0x0a));
    const quint32 count = quint32(values.size());
    out.append(char((count >> 24) & 0xff));
    out.append(char((count >> 16) & 0xff));
    out.append(char((count >> 8) & 0xff));
    out.append(char(count & 0xff));
    for (const QByteArray& value : values) {
        out.append(value);
    }
    return out;
}
```

Extend `skipValue`:

```cpp
if (type == 0x08) {
    if (needMore(data, *offset, 4)) return false;
    *offset += 4;
    while (*offset + 3 <= data.size()) {
        if (uchar(data[*offset]) == 0 && uchar(data[*offset + 1]) == 0 &&
            uchar(data[*offset + 2]) == 0x09) {
            *offset += 3;
            return true;
        }
        const int keySize = (int(uchar(data[*offset])) << 8) | int(uchar(data[*offset + 1]));
        *offset += 2 + keySize;
        if (!skipValue(data, offset)) return false;
    }
    return false;
}
if (type == 0x0a) {
    if (needMore(data, *offset, 4)) return false;
    const quint32 count = readU32Be(data.constData() + *offset);
    *offset += 4;
    for (quint32 i = 0; i < count; ++i) {
        if (!skipValue(data, offset)) return false;
    }
    return true;
}
```

- [ ] **Step 5: Verify**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add recorder_engine/ingest/rtmpprotocol.h recorder_engine/ingest/rtmpprotocol.cpp tests/unit/tst_rtmpprotocol.cpp
git commit -m "feat: support e-rtmp amf0 capability values"
```

---

### Task 5: Preserve Signed RTMP URLs And Add Capability Signaling

Fix query-token loss and advertise AVC/HEVC/AAC capability in `connect`.

**Files:**
- Modify: `recorder_engine/ingest/rtmpprotocol.h`
- Modify: `recorder_engine/ingest/rtmpprotocol.cpp`
- Modify: `recorder_engine/ingest/nativertmpingestsession.cpp`
- Modify: `tests/unit/tst_rtmpprotocol.cpp`
- Modify: `tests/e2e/rtmp_fixture_server.py`
- Modify: `tests/e2e/run_rtmp_smoke.sh`

- [ ] **Step 1: Add failing URL derivation tests**

Add helper declaration:

```cpp
struct RtmpUrlParts {
    QString app;
    QString playPath;
    QString tcUrl;

    static RtmpUrlParts fromUrl(const QUrl& url);
};
```

Add test:

```cpp
void TestRtmpProtocol::rtmpUrlPartsPreserveSignedQueryInPlayPath() {
    const RtmpUrlParts parts =
        RtmpUrlParts::fromUrl(QUrl(QStringLiteral("rtmps://host.example:443/live/cam1?token=abc&expires=123")));
    QCOMPARE(parts.app, QStringLiteral("live"));
    QCOMPARE(parts.playPath, QStringLiteral("cam1?token=abc&expires=123"));
    QCOMPARE(parts.tcUrl, QStringLiteral("rtmps://host.example:443/live"));
}
```

- [ ] **Step 2: Add failing connect capability test**

Create a test that builds the connect command payload via a new helper:

```cpp
QByteArray RtmpAmf0::connectCommandPayload(const QUrl& url);
```

Test:

```cpp
void TestRtmpProtocol::connectPayloadAdvertisesEnhancedCodecCapabilities() {
    const QByteArray payload =
        RtmpAmf0::connectCommandPayload(QUrl(QStringLiteral("rtmp://127.0.0.1/live/stream")));
    QVERIFY(payload.contains("fourCcList"));
    QVERIFY(payload.contains("avc1"));
    QVERIFY(payload.contains("hvc1"));
    QVERIFY(payload.contains("mp4a"));
    QVERIFY(payload.contains("videoFourCcInfoMap"));
    QVERIFY(payload.contains("audioFourCcInfoMap"));
}
```

- [ ] **Step 3: Run red**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

- [ ] **Step 4: Implement URL helper**

In `rtmpprotocol.h`, add `RtmpUrlParts`.

In `rtmpprotocol.cpp`, implement:

```cpp
RtmpUrlParts RtmpUrlParts::fromUrl(const QUrl& url) {
    const QString path = url.path().startsWith('/') ? url.path().mid(1) : url.path();
    RtmpUrlParts parts;
    parts.app = path.section('/', 0, 0);
    const QString rest = path.section('/', 1);
    parts.playPath = rest.isEmpty() ? parts.app : rest;
    if (!url.query().isEmpty()) {
        parts.playPath += QStringLiteral("?") + url.query(QUrl::FullyEncoded);
    }
    QUrl tc = url;
    tc.setPath(QStringLiteral("/") + parts.app);
    tc.setQuery(QString());
    parts.tcUrl = tc.toString(QUrl::FullyEncoded);
    return parts;
}
```

- [ ] **Step 5: Implement connect payload helper**

Use AMF0 object properties:

```cpp
{QStringLiteral("fourCcList"), RtmpAmf0::strictArray({
    RtmpAmf0::string(QStringLiteral("avc1")),
    RtmpAmf0::string(QStringLiteral("hvc1")),
    RtmpAmf0::string(QStringLiteral("mp4a")),
})},
{QStringLiteral("videoFourCcInfoMap"), RtmpAmf0::object({
    {QStringLiteral("avc1"), RtmpAmf0::number(1)},
    {QStringLiteral("hvc1"), RtmpAmf0::number(1)},
})},
{QStringLiteral("audioFourCcInfoMap"), RtmpAmf0::object({
    {QStringLiteral("mp4a"), RtmpAmf0::number(1)},
})},
```

Use `RtmpUrlParts::fromUrl(url)` for `app` and `tcUrl`.

- [ ] **Step 6: Use helpers in NativeRtmpIngestSession**

Replace duplicated URL/path logic in `sendConnectCommand` and `sendPlayCommand`:

```cpp
const RtmpUrlParts parts = RtmpUrlParts::fromUrl(m_url);
const QByteArray payload = RtmpAmf0::connectCommandPayload(m_url);
```

In `sendPlayCommand`, send `parts.playPath`.

- [ ] **Step 7: Add fixture auth assertion option**

In `tests/e2e/rtmp_fixture_server.py`, add:

```python
parser.add_argument("--require-play-query")
```

When `play` is parsed:

```python
if args.require_play_query and "?" not in playpath:
    raise ValueError("play path omitted required query string")
```

In `run_rtmp_smoke.sh`, support:

```bash
RTMP_REQUIRE_PLAY_QUERY=1
RTMP_URL_QUERY="token=abc"
```

Append query to URL and pass `--require-play-query` through `rtmp_server`.

- [ ] **Step 8: Verify**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol record_harness -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
RTMP_REQUIRE_PLAY_QUERY=1 RTMP_URL_QUERY=token=abc ctest --test-dir build/native-rtmp -R e2e_native_rtmp_smoke --output-on-failure
```

- [ ] **Step 9: Commit**

```bash
git add recorder_engine/ingest/rtmpprotocol.* recorder_engine/ingest/nativertmpingestsession.cpp tests/unit/tst_rtmpprotocol.cpp tests/e2e/rtmp_fixture_server.py tests/e2e/run_rtmp_smoke.sh tests/e2e/rtmp_lib.sh
git commit -m "fix: preserve signed rtmp play paths"
```

---

### Task 6: Add Legacy AVC/AAC Robustness And Connected-Black Prevention

Native RTMP must not report success indefinitely when no supported media arrives.

**Files:**
- Modify: `recorder_engine/ingest/nativertmpingestsession.h`
- Modify: `recorder_engine/ingest/nativertmpingestsession.cpp`
- Modify: `tests/e2e/rtmp_fixture_server.py`
- Create: `tests/e2e/run_rtmp_unsupported.sh`
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Write negative E2E script**

Create `tests/e2e/run_rtmp_unsupported.sh`:

```bash
#!/usr/bin/env bash
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23760}"

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

FLV="$WORKDIR/unsupported.flv"
SERVER_LOG="$WORKDIR/server.log"
HARNESS_OUT="$WORKDIR/harness.out"
HARNESS_ERR="$WORKDIR/harness.err"

ffmpeg -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -t 5 -c:v flv -an -f flv "$FLV"

rtmp_server "$PORT" "$FLV" "$SERVER_LOG" || exit 1

OLR_NATIVE_RTMP=1 "$HARNESS" --url "$(rtmp_url "$PORT")" --name rtmp_unsupported \
    --outdir "$WORKDIR" --seconds 6 --width 640 --height 480 --fps 30 \
    >"$HARNESS_OUT" 2>"$HARNESS_ERR"

if grep -q "Native RTMP connected" "$HARNESS_ERR" &&
   ! grep -q "unsupported" "$HARNESS_ERR"; then
    echo "FAIL: unsupported stream connected without explicit unsupported reason"
    cat "$HARNESS_ERR"
    exit 1
fi

echo "PASS: unsupported native RTMP profile fails visibly"
```

- [ ] **Step 2: Register and run red**

In `tests/e2e/CMakeLists.txt`, add:

```cmake
add_test(NAME e2e_native_rtmp_unsupported
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_unsupported.sh" "$<TARGET_FILE:record_harness>" 23760)
```

Add it to `native-rtmp` label.

Run:

```bash
cmake --build build/native-rtmp --target record_harness -j2
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_unsupported --output-on-failure
```

Expected: fail until unsupported media produces explicit failure.

- [ ] **Step 3: Track first supported media deadline**

In `nativertmpingestsession.h`, add:

```cpp
bool m_seenSupportedVideo = false;
bool m_seenSupportedAudio = false;
int64_t m_openedAtMs = -1;
QString m_unsupportedReason;
```

In `open`, reset and set `m_openedAtMs`.

In `processVideoMessage`, when codec id is unsupported:

```cpp
if (m_unsupportedReason.isEmpty()) {
    m_unsupportedReason = QStringLiteral("unsupported RTMP video codec id %1").arg(codecId);
    log(QStringLiteral("Native RTMP unsupported profile: %1").arg(m_unsupportedReason));
}
```

Set `m_seenSupportedVideo = true` after a valid AVC or HEVC sequence header.

In `run`, after processing messages:

```cpp
if (!m_seenSupportedVideo && m_openedAtMs >= 0 &&
    m_monotonic.elapsed() - m_openedAtMs > 5000) {
    log(QStringLiteral("Native RTMP unsupported profile: no supported video within probe window."));
    break;
}
```

- [ ] **Step 4: Verify**

```bash
cmake --build build/native-rtmp --target record_harness -j2
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_unsupported --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/ingest/nativertmpingestsession.* tests/e2e/run_rtmp_unsupported.sh tests/e2e/CMakeLists.txt
git commit -m "fix: fail visibly on unsupported native rtmp profiles"
```

---

### Task 7: Add E-RTMP Video Header Parsing

Parse E-RTMP video packets independently before wiring HEVC decode.

**Files:**
- Modify: `recorder_engine/ingest/rtmpprotocol.h`
- Modify: `recorder_engine/ingest/rtmpprotocol.cpp`
- Modify: `tests/unit/tst_rtmpprotocol.cpp`

- [ ] **Step 1: Add failing E-RTMP header tests**

Add data structures:

```cpp
enum class RtmpVideoPacketFlavor { LegacyAvc, Enhanced };
enum class RtmpEnhancedVideoPacketType { SequenceStart = 0, CodedFrames = 1, SequenceEnd = 2, CodedFramesX = 3, Metadata = 4, Multitrack = 6, Unknown = 255 };

struct RtmpVideoPacket {
    RtmpVideoPacketFlavor flavor = RtmpVideoPacketFlavor::LegacyAvc;
    NativeVideoCodec codec = NativeVideoCodec::Unknown;
    RtmpEnhancedVideoPacketType enhancedType = RtmpEnhancedVideoPacketType::Unknown;
    QString fourCc;
    qint32 compositionTimeMs = 0;
    int trackId = 0;
    QByteArray codecPayload;
};
```

Add test:

```cpp
void TestRtmpProtocol::parsesEnhancedHevcSequenceStartHeader() {
    QByteArray payload;
    payload.append(char(0x80 | 0)); // enhanced + SequenceStart
    payload.append("hvc1", 4);
    payload.append("CONFIG", 6);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::Enhanced);
    QCOMPARE(packet.codec, NativeVideoCodec::Hevc);
    QCOMPARE(packet.enhancedType, RtmpEnhancedVideoPacketType::SequenceStart);
    QCOMPARE(packet.fourCc, QStringLiteral("hvc1"));
    QCOMPARE(packet.codecPayload, QByteArray("CONFIG", 6));
}
```

Add test:

```cpp
void TestRtmpProtocol::parsesEnhancedCodedFramesCompositionTime() {
    QByteArray payload;
    payload.append(char(0x80 | 1)); // enhanced + CodedFrames
    payload.append("avc1", 4);
    payload.append(QByteArray::fromHex("00002a")); // comp time 42
    payload.append("FRAME", 5);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.codec, NativeVideoCodec::H264);
    QCOMPARE(packet.compositionTimeMs, 42);
    QCOMPARE(packet.codecPayload, QByteArray("FRAME", 5));
}
```

- [ ] **Step 2: Run red**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

- [ ] **Step 3: Implement parser**

In `RtmpFlv::parseVideoPacket`:

```cpp
const bool enhanced = (uchar(payload[0]) & 0x80) != 0;
if (!enhanced) {
    // Preserve existing legacy AVC behavior.
}
const int packetType = uchar(payload[0]) & 0x0f;
const QByteArray fourCcBytes = payload.mid(1, 4);
packet->fourCc = QString::fromLatin1(fourCcBytes);
packet->codec = packet->fourCc == QStringLiteral("hvc1") ? NativeVideoCodec::Hevc :
                packet->fourCc == QStringLiteral("avc1") ? NativeVideoCodec::H264 :
                NativeVideoCodec::Unknown;
```

For `CodedFrames`, read SI24 composition time after FourCC. For `CodedFramesX`, composition time is zero and payload begins immediately after FourCC. For `SequenceStart` and `SequenceEnd`, no composition time is present.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
git add recorder_engine/ingest/rtmpprotocol.* tests/unit/tst_rtmpprotocol.cpp
git commit -m "feat: parse e-rtmp video packet headers"
```

---

### Task 8: Parse HEVCDecoderConfigurationRecord

Convert E-RTMP `hvc1` sequence starts into VideoToolbox-ready parameter sets.

**Files:**
- Modify: `recorder_engine/ingest/rtmpprotocol.h`
- Modify: `recorder_engine/ingest/rtmpprotocol.cpp`
- Modify: `tests/unit/tst_rtmpprotocol.cpp`

- [ ] **Step 1: Add failing HEVC config test**

Add:

```cpp
void parsesHevcSequenceHeaderAndConvertsNalusToAnnexB();
```

Test:

```cpp
void TestRtmpProtocol::parsesHevcSequenceHeaderAndConvertsNalusToAnnexB() {
    const QByteArray vps = QByteArray::fromHex("40010c01ffff01600000030090000003000003005d959809");
    const QByteArray sps = QByteArray::fromHex("42010101600000030090000003000003005da00280802d1f");
    const QByteArray pps = QByteArray::fromHex("4401c172b46240");

    QByteArray config(23, char(0));
    config[0] = char(1);
    config[21] = char(0xfc | 3); // 4-byte NAL length
    config[22] = char(3);        // arrays

    auto appendArray = [&](int nalType, const QByteArray& nal) {
        config.append(char(0x80 | nalType));
        config.append(char(0));
        config.append(char(1));
        config.append(char((nal.size() >> 8) & 0xff));
        config.append(char(nal.size() & 0xff));
        config.append(nal);
    };
    appendArray(32, vps);
    appendArray(33, sps);
    appendArray(34, pps);

    RtmpHevcConfig parsed;
    QString error;
    QVERIFY(RtmpFlv::parseHevcSequenceHeader(config, &parsed, &error));
    QCOMPARE(parsed.nalLengthSize, 4);
    QCOMPARE(parsed.parameterSets.hevcVps, QList<QByteArray>{vps});
    QCOMPARE(parsed.parameterSets.hevcSps, QList<QByteArray>{sps});
    QCOMPARE(parsed.parameterSets.hevcPps, QList<QByteArray>{pps});

    QByteArray frame;
    frame.append(QByteArray::fromHex("00000002"));
    frame.append(QByteArray::fromHex("2601"));
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(frame, 4),
             QByteArray::fromHex("000000012601"));
}
```

- [ ] **Step 2: Run red**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
```

- [ ] **Step 3: Implement shared length-prefixed conversion**

Replace `avcPayloadToAnnexB` internals with:

```cpp
QByteArray RtmpFlv::lengthPrefixedPayloadToAnnexB(const QByteArray& payload, int nalLengthSize);
```

Keep `avcPayloadToAnnexB` as a wrapper for compatibility during migration.

- [ ] **Step 4: Implement HEVC config parser**

In `rtmpprotocol.h`:

```cpp
struct RtmpHevcConfig {
    int nalLengthSize = 4;
    H26xParameterSets parameterSets;
};
bool parseHevcSequenceHeader(const QByteArray& payload, RtmpHevcConfig* config, QString* error);
```

Parsing rules:

- require at least 23 bytes;
- `nalLengthSize = (payload[21] & 0x03) + 1`;
- `numOfArrays = payload[22]`;
- each array has one byte completeness/type, two-byte NAL count, then repeated two-byte NAL length + NAL bytes;
- collect types 32 VPS, 33 SPS, 34 PPS;
- require at least one VPS, SPS, and PPS.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R tst_rtmpprotocol --output-on-failure
git add recorder_engine/ingest/rtmpprotocol.* tests/unit/tst_rtmpprotocol.cpp
git commit -m "feat: parse hevc configuration for e-rtmp"
```

---

### Task 9: Wire E-RTMP HEVC Into NativeRtmpIngestSession

Decode E-RTMP `hvc1` streams through existing VideoToolbox HEVC support.

**Files:**
- Modify: `recorder_engine/ingest/nativertmpingestsession.h`
- Modify: `recorder_engine/ingest/nativertmpingestsession.cpp`
- Modify: `tests/e2e/rtmp_fixture_server.py`
- Create: `tests/e2e/run_rtmp_hevc_smoke.sh`
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Add HEVC fixture mode**

In `rtmp_fixture_server.py`, add deterministic E-RTMP HEVC input options:

```python
parser.add_argument("--enhanced-hevc", action="store_true")
parser.add_argument("--hevc-annexb-source")
```

Add helpers that read raw Annex B HEVC and emit length-prefixed E-RTMP video payloads:

```python
def split_annexb(data: bytes) -> list[bytes]:
    starts = []
    i = 0
    while i + 3 < len(data):
        if data[i:i + 3] == b"\x00\x00\x01":
            starts.append((i, 3))
            i += 3
        elif i + 4 < len(data) and data[i:i + 4] == b"\x00\x00\x00\x01":
            starts.append((i, 4))
            i += 4
        else:
            i += 1
    out = []
    for index, (start, prefix_len) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(data)
        nal = data[start + prefix_len:end].strip(b"\x00")
        if nal:
            out.append(nal)
    return out

def hevc_nal_type(nal: bytes) -> int:
    return (nal[0] >> 1) & 0x3F

def build_hvcc(vps: bytes, sps: bytes, pps: bytes) -> bytes:
    header = bytearray(23)
    header[0] = 1
    header[21] = 0xFC | 3  # lengthSizeMinusOne = 3, four-byte NAL lengths
    header[22] = 3
    arrays = bytearray()
    for nal_type, nal in ((32, vps), (33, sps), (34, pps)):
        arrays.append(0x80 | nal_type)
        arrays.extend((1).to_bytes(2, "big"))
        arrays.extend(len(nal).to_bytes(2, "big"))
        arrays.extend(nal)
    return bytes(header + arrays)

def length_prefixed(nals: list[bytes]) -> bytes:
    out = bytearray()
    for nal in nals:
        out.extend(len(nal).to_bytes(4, "big"))
        out.extend(nal)
    return bytes(out)
```

When `args.enhanced_hevc` is true, require `args.hevc_annexb_source`, parse the NALs, require one VPS/SPS/PPS, send a `SequenceStart` packet with FourCC `hvc1` and the `build_hvcc(...)` payload, then send `CodedFrames` packets every 33 ms. Group each access unit by starting a new frame at NAL types 19, 20, or 1, and include non-parameter-set NALs in that frame. Do not send VPS/SPS/PPS inside coded frames after the sequence header.

- [ ] **Step 2: Add HEVC smoke script**

Create `tests/e2e/run_rtmp_hevc_smoke.sh`:

```bash
#!/usr/bin/env bash
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23770}"
SECS=8

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

SOURCE="$WORKDIR/hevc_source.hevc"
ffmpeg -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -t "$SECS" -c:v hevc_videotoolbox -an -f hevc "$SOURCE" || \
ffmpeg -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -t "$SECS" -c:v libx265 -x265-params log-level=error -an -f hevc "$SOURCE" || {
        echo "SKIP: local ffmpeg cannot generate HEVC fixture"
        exit 77
    }

python3 "$HERE/rtmp_fixture_server.py" --port "$PORT" --hevc-annexb-source "$SOURCE" --enhanced-hevc \
    >"$WORKDIR/server.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 1 50); do
    grep -q '^READY ' "$WORKDIR/server.log" && break
    sleep 0.1
done

OLR_NATIVE_RTMP=1 "$HARNESS" --url "$(rtmp_url "$PORT")" --name rtmp_hevc \
    --outdir "$WORKDIR" --seconds "$SECS" --width 640 --height 480 --fps 30 \
    >"$WORKDIR/harness.out" 2>"$WORKDIR/harness.err"

OUT_MKV="$(tail -n 1 "$WORKDIR/harness.out")"
if [ -z "$OUT_MKV" ] || [ ! -s "$OUT_MKV" ]; then
    echo "FAIL: HEVC E-RTMP produced no output"
    cat "$WORKDIR/harness.err"
    cat "$WORKDIR/server.log"
    exit 1
fi
grep -q "hvc1" "$WORKDIR/harness.err" || {
    echo "FAIL: native RTMP log did not identify hvc1"
    cat "$WORKDIR/harness.err"
    exit 1
}
echo "PASS: native HEVC E-RTMP recorded output"
```

- [ ] **Step 3: Register and run red**

Add CTest:

```cmake
add_test(NAME e2e_native_rtmp_hevc_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23770)
set_tests_properties(e2e_native_rtmp_hevc_smoke PROPERTIES
    LABELS "native-rtmp"
    SKIP_RETURN_CODE 77)
```

Run:

```bash
cmake --build build/native-rtmp --target record_harness -j2
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_hevc_smoke --output-on-failure
```

Expected: fail because session does not decode E-RTMP HEVC yet. If local FFmpeg supports neither `hevc_videotoolbox` nor `libx265`, CTest marks the test skipped with return code 77.

- [ ] **Step 4: Add session HEVC state**

In `nativertmpingestsession.h`:

```cpp
RtmpHevcConfig m_hevcConfig;
NativeVideoCodec m_videoCodec = NativeVideoCodec::Unknown;
```

Reset in `open`.

- [ ] **Step 5: Replace processVideoMessage dispatch**

Use `RtmpFlv::parseVideoPacket`:

```cpp
RtmpVideoPacket packet;
QString error;
if (!RtmpFlv::parseVideoPacket(payload, &packet, &error)) {
    log(QStringLiteral("Native RTMP video parse failed: %1").arg(error));
    return;
}
```

For `SequenceStart`:

```cpp
if (packet.codec == NativeVideoCodec::Hevc) {
    if (!RtmpFlv::parseHevcSequenceHeader(packet.codecPayload, &m_hevcConfig, &error)) {
        log(error);
        return;
    }
    m_videoCodec = NativeVideoCodec::Hevc;
    m_seenSupportedVideo = true;
    if (m_videoDecoder) m_videoDecoder->reset();
    log(QStringLiteral("Native RTMP video codec hvc1."));
    return;
}
```

For `CodedFrames` / `CodedFramesX`:

```cpp
const QByteArray annexB =
    RtmpFlv::lengthPrefixedPayloadToAnnexB(packet.codecPayload,
                                           packet.codec == NativeVideoCodec::Hevc
                                               ? m_hevcConfig.nalLengthSize
                                               : m_avcConfig.nalLengthSize);
CompressedAccessUnit unit;
unit.codec = packet.codec;
unit.parameterSets = packet.codec == NativeVideoCodec::Hevc
    ? m_hevcConfig.parameterSets
    : m_avcConfig.parameterSets;
```

- [ ] **Step 6: Verify**

```bash
cmake --build build/native-rtmp --target record_harness tst_rtmpprotocol -j2
ctest --test-dir build/native-rtmp -R 'tst_rtmpprotocol|e2e_native_rtmp_hevc_smoke' --output-on-failure
```

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/ingest/nativertmpingestsession.* tests/e2e/rtmp_fixture_server.py tests/e2e/run_rtmp_hevc_smoke.sh tests/e2e/CMakeLists.txt
git commit -m "feat: decode hevc e-rtmp natively"
```

---

### Task 10: Add RTMPS HEVC Smoke

Run the same HEVC/E-RTMP path over TLS.

**Files:**
- Create: `tests/e2e/run_rtmps_hevc_smoke.sh`
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Add RTMPS HEVC wrapper**

Create:

```bash
#!/usr/bin/env bash
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23771}"
HERE="$(cd "$(dirname "$0")" && pwd)"

command -v openssl >/dev/null || { echo "SKIP: openssl not found"; exit 0; }
WORKDIR="$(mktemp -d)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

CERT="$WORKDIR/rtmps-cert.pem"
KEY="$WORKDIR/rtmps-key.pem"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj "/CN=127.0.0.1" \
    -addext "subjectAltName=IP:127.0.0.1" \
    -keyout "$KEY" -out "$CERT" >/dev/null 2>&1 || {
        echo "SKIP: could not generate RTMPS certificate"
        exit 0
    }

RTMP_SCHEME=rtmps \
RTMP_SERVER_TLS_CERT="$CERT" \
RTMP_SERVER_TLS_KEY="$KEY" \
OLR_NATIVE_RTMP_ALLOW_INSECURE_TLS=1 \
    bash "$HERE/run_rtmp_hevc_smoke.sh" "$HARNESS" "$PORT"
```

- [ ] **Step 2: Register and run**

Add:

```cmake
add_test(NAME e2e_native_rtmps_hevc_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmps_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23771)
set_tests_properties(e2e_native_rtmps_hevc_smoke PROPERTIES
    LABELS "native-rtmp"
    SKIP_RETURN_CODE 77)
```

Run:

```bash
cmake --build build/native-rtmp --target record_harness -j2
ctest --test-dir build/native-rtmp -R 'e2e_native_rtmp_hevc_smoke|e2e_native_rtmps_hevc_smoke' --output-on-failure
```

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/run_rtmps_hevc_smoke.sh tests/e2e/CMakeLists.txt
git commit -m "test: cover hevc e-rtmp over rtmps"
```

---

### Task 11: Add Native Fallback Reason And FFmpeg Retry Policy

If native detects unsupported profile or decode capability failure, `StreamWorker` should retry with FFmpeg for that URL until source changes.

**Files:**
- Modify: `recorder_engine/ingest/ingestsession.h`
- Modify: `recorder_engine/ingest/nativertmpingestsession.h`
- Modify: `recorder_engine/ingest/nativertmpingestsession.cpp`
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `tests/unit/tst_ingestbackendselector.cpp`

- [ ] **Step 1: Add failure category API**

In `ingestsession.h`:

```cpp
enum class IngestFailureKind {
    None,
    TransientNetwork,
    UnsupportedProfile,
    DecodeCapability,
    MalformedStream
};

struct IngestOpenResult {
    bool ok = false;
    IngestFailureKind failure = IngestFailureKind::None;
    QString message;
};
```

Do not replace `open` yet. Add optional virtual:

```cpp
virtual IngestFailureKind lastFailureKind() const { return IngestFailureKind::None; }
```

- [ ] **Step 2: Add failing selector/fallback unit test**

Add pure helper:

```cpp
bool shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind failure);
```

Test:

```cpp
void TestIngestBackendSelector::nativeUnsupportedProfileAllowsFfmpegFallback() {
    QVERIFY(shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind::UnsupportedProfile));
    QVERIFY(shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind::DecodeCapability));
    QVERIFY(!shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind::TransientNetwork));
}
```

- [ ] **Step 3: Implement fallback helper**

```cpp
bool shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind failure) {
    return failure == IngestFailureKind::UnsupportedProfile ||
           failure == IngestFailureKind::DecodeCapability ||
           failure == IngestFailureKind::MalformedStream;
}
```

- [ ] **Step 4: Set native RTMP failure kind**

In `NativeRtmpIngestSession`, add:

```cpp
IngestFailureKind m_lastFailureKind = IngestFailureKind::None;
IngestFailureKind lastFailureKind() const override { return m_lastFailureKind; }
```

Set:

- network connect/read timeout: `TransientNetwork`
- unsupported codec/FourCC: `UnsupportedProfile`
- VideoToolbox session creation failure: `DecodeCapability`
- parser errors: `MalformedStream`

- [ ] **Step 5: Use fallback in StreamWorker**

In `StreamWorker::captureLoop`, keep:

```cpp
bool forceFfmpegForCurrentUrl = false;
```

After native `open` fails:

```cpp
if (backendKind == IngestBackendKind::NativeRtmp &&
    shouldFallbackToFfmpegAfterNativeFailure(session->lastFailureKind()) &&
    !qEnvironmentVariableIsSet("OLR_NATIVE_RTMP_DISABLE_FALLBACK")) {
    qDebug() << "Source" << m_sourceIndex << "Native RTMP failed with unsupported profile; retrying FFmpeg for this URL.";
    forceFfmpegForCurrentUrl = true;
    continue;
}
```

When source URL changes, reset `forceFfmpegForCurrentUrl`.

- [ ] **Step 6: Verify**

```bash
cmake --build build/native-rtmp --target tst_ingestbackendselector record_harness -j2
ctest --test-dir build/native-rtmp -R 'tst_ingestbackendselector|e2e_native_rtmp_unsupported' --output-on-failure
```

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/ingest/ingestsession.* recorder_engine/ingest/nativertmpingestsession.* recorder_engine/streamworker.cpp tests/unit/tst_ingestbackendselector.cpp
git commit -m "feat: fall back after unsupported native rtmp profiles"
```

---

### Task 12: Add Stall And Reconnect Request Handling

Native RTMP should restart cleanly after stalls and recognize E-RTMP reconnect requests.

**Files:**
- Modify: `recorder_engine/ingest/nativertmpingestsession.h`
- Modify: `recorder_engine/ingest/nativertmpingestsession.cpp`
- Modify: `tests/e2e/rtmp_fixture_server.py`
- Create: `tests/e2e/run_rtmp_reconnect.sh`
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Add reconnect fixture mode**

In `rtmp_fixture_server.py`, add:

```python
parser.add_argument("--disconnect-after-tags", type=int, default=0)
parser.add_argument("--send-reconnect-request", action="store_true")
```

When `--send-reconnect-request` is set, send an `onStatus` command:

```python
writer.command(
    0,
    "onStatus",
    0,
    None,
    {
        "level": "status",
        "code": "NetConnection.Connect.ReconnectRequest",
        "description": "Reconnect requested.",
    },
)
```

- [ ] **Step 2: Add reconnect E2E**

Create `tests/e2e/run_rtmp_reconnect.sh`:

```bash
#!/usr/bin/env bash
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23780}"
SECS=12

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

SOURCE="$WORKDIR/source.flv"
rtmp_generate_tone_flv "$SOURCE" 1000 "$SECS"

python3 "$HERE/rtmp_fixture_server.py" \
    --port "$PORT" \
    --source "$SOURCE" \
    --disconnect-after-tags 12 \
    --send-reconnect-request \
    >"$WORKDIR/server.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 1 50); do
    grep -q '^READY ' "$WORKDIR/server.log" && break
    sleep 0.1
done

OUT="$(OLR_NATIVE_RTMP=1 "$HARNESS" --url "$(rtmp_url "$PORT")" \
      --name rtmp_reconnect --outdir "$WORKDIR" --seconds "$SECS" \
      --width 640 --height 480 --fps 30 2>"$WORKDIR/harness.err")"
MKV="$(printf '%s\n' "$OUT" | tail -n1)"
if [ -z "$MKV" ] || [ ! -s "$MKV" ]; then
    echo "FAIL: no reconnect output"
    cat "$WORKDIR/harness.err"
    cat "$WORKDIR/server.log"
    exit 1
fi

PACKETS="$(ffprobe -v error -select_streams v:0 -count_packets \
    -show_entries stream=nb_read_packets -of default=noprint_wrappers=1:nokey=1 "$MKV" | head -n1)"
MIN=$((30 * SECS / 2))
if [ "${PACKETS:-0}" -lt "$MIN" ]; then
    echo "FAIL: too few packets after reconnect $PACKETS < $MIN"
    cat "$WORKDIR/harness.err"
    cat "$WORKDIR/server.log"
    exit 1
fi

grep -Eiq 'reconnect|stalled|retry' "$WORKDIR/harness.err" || {
    echo "FAIL: reconnect/stall marker missing"
    cat "$WORKDIR/harness.err"
    cat "$WORKDIR/server.log"
    exit 1
}

echo "PASS: native RTMP reconnect packets=$PACKETS"
```

Register it:

```cmake
add_test(NAME e2e_native_rtmp_reconnect
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_reconnect.sh" "$<TARGET_FILE:record_harness>" 23780)
set_tests_properties(e2e_native_rtmp_reconnect PROPERTIES
    LABELS "native-rtmp"
    TIMEOUT 60)
```

- [ ] **Step 3: Run red**

```bash
cmake --build build/native-rtmp --target record_harness -j2
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_reconnect --output-on-failure
```

- [ ] **Step 4: Add stall timeout**

In native RTMP run loop:

```cpp
constexpr int kStallTimeoutMs = 8000;
```

When `waitForReadyRead` times out and last packet is older than timeout:

```cpp
m_lastFailureKind = IngestFailureKind::TransientNetwork;
if (error) *error = QStringLiteral("Native RTMP stalled.");
return false;
```

- [ ] **Step 5: Parse reconnect request**

Handle command messages in `processMessage`:

```cpp
if (message.type == kMessageCommandAmf0) {
    processCommandMessage(message);
    return;
}
```

`processCommandMessage` reads command name and info object enough to detect `NetConnection.Connect.ReconnectRequest`; log and set transient failure / stop session.

- [ ] **Step 6: Verify and commit**

```bash
cmake --build build/native-rtmp --target record_harness -j2
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_reconnect --output-on-failure
git add recorder_engine/ingest/nativertmpingestsession.* tests/e2e/rtmp_fixture_server.py tests/e2e/run_rtmp_reconnect.sh tests/e2e/CMakeLists.txt
git commit -m "feat: restart native rtmp after stalls"
```

---

### Task 13: Make The Fixture Hostile

Ensure the E2E fixture exercises arbitrary TCP fragmentation and chunking.

**Files:**
- Modify: `tests/e2e/rtmp_fixture_server.py`
- Modify: `tests/e2e/rtmp_lib.sh`
- Modify: `tests/e2e/run_rtmp_smoke.sh`
- Modify: `tests/e2e/run_rtmp_4cam.sh`
- Modify: `tests/e2e/run_rtmp_sync.sh`
- Modify: `tests/e2e/run_rtmp_trim.sh`
- Modify: `tests/e2e/run_rtmp_connect.sh`

- [ ] **Step 1: Add fixture send fragmentation**

In `RtmpWriter`, add constructor args:

```python
def __init__(self, conn: socket.socket, chunk_size: int = OUT_CHUNK_SIZE, write_fragment: int = 0) -> None:
    self.conn = conn
    self.chunk_size = chunk_size
    self.write_fragment = write_fragment
```

Add:

```python
def send_bytes(self, data: bytes) -> None:
    if self.write_fragment <= 0:
        self.conn.sendall(data)
        return
    for offset in range(0, len(data), self.write_fragment):
        self.conn.sendall(data[offset : offset + self.write_fragment])
        time.sleep(0.001)
```

Use `send_bytes` instead of `conn.sendall`.

- [ ] **Step 2: Add command-line options**

```python
parser.add_argument("--out-chunk-size", type=int, default=OUT_CHUNK_SIZE)
parser.add_argument("--write-fragment", type=int, default=0)
```

Construct:

```python
writer = RtmpWriter(conn, args.out_chunk_size, args.write_fragment)
```

- [ ] **Step 3: Wire shell env**

In `rtmp_lib.sh`:

```bash
local extra_args=()
[ -n "${RTMP_FIXTURE_OUT_CHUNK_SIZE:-}" ] && extra_args+=(--out-chunk-size "$RTMP_FIXTURE_OUT_CHUNK_SIZE")
[ -n "${RTMP_FIXTURE_WRITE_FRAGMENT:-}" ] && extra_args+=(--write-fragment "$RTMP_FIXTURE_WRITE_FRAGMENT")
```

Pass `"${extra_args[@]}"` in both TLS and non-TLS server invocations. Branch explicitly for empty arrays for macOS Bash 3 compatibility.

- [ ] **Step 4: Run hostile matrix**

```bash
RTMP_FIXTURE_OUT_CHUNK_SIZE=7 RTMP_FIXTURE_WRITE_FRAGMENT=1 \
ctest --test-dir build/native-rtmp -L native-rtmp --output-on-failure
```

Expected: all native RTMP tests pass. HEVC fixture tests may skip only with return code 77 and message `SKIP: local ffmpeg cannot generate HEVC fixture`.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/rtmp_fixture_server.py tests/e2e/rtmp_lib.sh tests/e2e/run_rtmp_*.sh
git commit -m "test: make native rtmp fixture fragmentation hostile"
```

---

### Task 14: Add Real-Server Interop Gates

Make the opt-in interop script report explicit skip semantics and document server recipes.

**Files:**
- Modify: `tests/e2e/run_rtmp_interop.sh`
- Modify: `tests/README.md`
- Modify: `tests/e2e/CMakeLists.txt`

- [ ] **Step 1: Use CTest skip marker**

Change missing URL case in `run_rtmp_interop.sh`:

```bash
if [ -z "$PLAY_URL" ]; then
    echo "SKIP: set OLR_RTMP_INTEROP_PLAY_URL to run real-server RTMP interop"
    exit 77
fi
```

In CMake:

```cmake
set_tests_properties(e2e_native_rtmp_interop PROPERTIES
    SKIP_RETURN_CODE 77)
```

- [ ] **Step 2: Add HEVC interop mode**

Support:

```bash
OLR_RTMP_INTEROP_CODEC=hevc
```

When codec is `hevc`, generate `hevc_source.hevc` with the same two-command fallback used by `run_rtmp_hevc_smoke.sh`, publish it through the configured `OLR_RTMP_INTEROP_PUBLISH_URL`, and record from `OLR_RTMP_INTEROP_PLAY_URL`. If neither `hevc_videotoolbox` nor `libx265` can create the Annex B source, print `SKIP: local ffmpeg cannot generate HEVC fixture` and exit 77.

- [ ] **Step 3: Document recipes**

In `tests/README.md`, add:

````markdown
Real-server native RTMP interop:

```bash
OLR_RTMP_INTEROP_PLAY_URL=rtmp://127.0.0.1/live/stream \
OLR_RTMP_INTEROP_PUBLISH_URL=rtmp://127.0.0.1/live/stream \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_interop --output-on-failure
```

For HEVC/E-RTMP:

```bash
OLR_RTMP_INTEROP_CODEC=hevc \
OLR_RTMP_INTEROP_PLAY_URL=rtmp://127.0.0.1/live/hevc \
OLR_RTMP_INTEROP_PUBLISH_URL=rtmp://127.0.0.1/live/hevc \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_interop --output-on-failure
```
````

- [ ] **Step 4: Verify skip and configured local server**

Run:

```bash
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_interop --output-on-failure
```

Expected: skipped if URLs unset.

If no real server is configured, this gate remains unfulfilled and Task 16 must not flip native RTMP on by default. When a real server is configured, record the exact command and pass/fail output in the final local handoff notes.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/run_rtmp_interop.sh tests/e2e/CMakeLists.txt tests/README.md
git commit -m "test: clarify real-server rtmp interop gate"
```

---

### Task 15: Add Soak Gates

Add opt-in long-run AVC and HEVC soak tests.

**Files:**
- Create: `tests/e2e/run_rtmp_soak.sh`
- Modify: `tests/e2e/CMakeLists.txt`
- Modify: `tests/README.md`

- [ ] **Step 1: Create soak script**

Create `tests/e2e/run_rtmp_soak.sh`:

```bash
#!/usr/bin/env bash
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/rtmp_lib.sh"

HARNESS="${1:?record_harness executable path required}"
PORT="${2:-23790}"
CODEC="${OLR_RTMP_SOAK_CODEC:-avc}"
SECS="${OLR_RTMP_SOAK_SECONDS:-1800}"

if [ "${OLR_RTMP_RUN_SOAK:-0}" != "1" ]; then
    echo "SKIP: set OLR_RTMP_RUN_SOAK=1 to run native RTMP soak"
    exit 77
fi

rtmp_require_tools
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() { ((${#PIDS[@]})) && kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

SOURCE="$WORKDIR/source.flv"
if [ "$CODEC" = "avc" ]; then
    rtmp_generate_tone_flv "$SOURCE" 1000 "$SECS"
    rtmp_server "$PORT" "$SOURCE" "$WORKDIR/server.log" || exit 1
elif [ "$CODEC" = "hevc" ]; then
    SOURCE="$WORKDIR/hevc_source.hevc"
    ffmpeg -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -t "$SECS" -c:v hevc_videotoolbox -an -f hevc "$SOURCE" || \
    ffmpeg -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -t "$SECS" -c:v libx265 -x265-params log-level=error -an -f hevc "$SOURCE" || {
            echo "SKIP: local ffmpeg cannot generate HEVC fixture"
            exit 77
        }
    python3 "$HERE/rtmp_fixture_server.py" --port "$PORT" --hevc-annexb-source "$SOURCE" --enhanced-hevc \
        >"$WORKDIR/server.log" 2>&1 &
    PIDS+=("$!")
    for _ in $(seq 1 50); do
        grep -q '^READY ' "$WORKDIR/server.log" && break
        sleep 0.1
    done
else
    echo "FAIL: unsupported OLR_RTMP_SOAK_CODEC=$CODEC"
    exit 1
fi

OUT="$(OLR_NATIVE_RTMP=1 "$HARNESS" --url "$(rtmp_url "$PORT")" \
      --name "rtmp_soak_${CODEC}" --outdir "$WORKDIR" --seconds "$SECS" \
      --width 640 --height 480 --fps 30 2>"$WORKDIR/harness.err")"
MKV="$(printf '%s\n' "$OUT" | tail -n1)"
[ -s "$MKV" ] || { echo "FAIL: no soak output"; cat "$WORKDIR/harness.err"; exit 1; }

PACKETS="$(ffprobe -v error -select_streams v:0 -count_packets \
    -show_entries stream=nb_read_packets -of default=noprint_wrappers=1:nokey=1 "$MKV" | head -n1)"
MIN=$((30 * SECS * 8 / 10))
[ "${PACKETS:-0}" -ge "$MIN" ] || { echo "FAIL: too few soak packets $PACKETS < $MIN"; exit 1; }

echo "PASS: native RTMP ${CODEC} soak packets=$PACKETS seconds=$SECS"
```

- [ ] **Step 2: Register with skip code**

```cmake
add_test(NAME e2e_native_rtmp_soak
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_rtmp_soak.sh" "$<TARGET_FILE:record_harness>" 23790)
set_tests_properties(e2e_native_rtmp_soak PROPERTIES
    LABELS "native-rtmp-soak"
    TIMEOUT 3900
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)
```

- [ ] **Step 3: Verify default skip**

```bash
cmake --build build/native-rtmp --target record_harness -j2
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_soak --output-on-failure
```

Expected: skipped.

- [ ] **Step 4: Run short soak locally**

```bash
OLR_RTMP_RUN_SOAK=1 OLR_RTMP_SOAK_SECONDS=60 \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_soak --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/run_rtmp_soak.sh tests/e2e/CMakeLists.txt tests/README.md
git commit -m "test: add opt-in native rtmp soak gate"
```

---

### Task 16: Full Verification And Default Flip Decision

Only flip native RTMP default after all readiness gates pass.

**Files:**
- Modify only if gates pass: `recorder_engine/ingest/ingestsession.cpp`
- Modify only if gates pass: `tests/unit/tst_ingestbackendselector.cpp`
- Modify only if gates pass: `tests/README.md`

- [ ] **Step 1: Run mandatory local verification**

```bash
cmake --build build/native-rtmp --target tst_rtmpprotocol tst_ingestbackendselector record_harness sync_harness OpenLiveReplay -j2
ctest --test-dir build/native-rtmp -R 'tst_rtmpprotocol|tst_ingestbackendselector' --output-on-failure
ctest --test-dir build/native-rtmp -L native-rtmp --output-on-failure
RTMP_FIXTURE_OUT_CHUNK_SIZE=7 RTMP_FIXTURE_WRITE_FRAGMENT=1 ctest --test-dir build/native-rtmp -L native-rtmp --output-on-failure
git diff --check
```

- [ ] **Step 2: Run repeat check**

```bash
ctest --test-dir build/native-rtmp -L native-rtmp --repeat until-fail:3 --output-on-failure
```

- [ ] **Step 3: Run real-server interop**

Run AVC real-server interop and record exact command/output in local notes:

```bash
OLR_RTMP_INTEROP_PLAY_URL=rtmp://127.0.0.1/live/stream \
OLR_RTMP_INTEROP_PUBLISH_URL=rtmp://127.0.0.1/live/stream \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_interop --output-on-failure
```

Run HEVC/E-RTMP real-server interop. If there is no configured server that supports E-RTMP `hvc1`, keep native RTMP opt-in and record that blocker in the final local handoff:

```bash
OLR_RTMP_INTEROP_CODEC=hevc \
OLR_RTMP_INTEROP_PLAY_URL=rtmp://127.0.0.1/live/hevc \
OLR_RTMP_INTEROP_PUBLISH_URL=rtmp://127.0.0.1/live/hevc \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_interop --output-on-failure
```

- [ ] **Step 4: Run soak gates**

Run at least:

```bash
OLR_RTMP_RUN_SOAK=1 OLR_RTMP_SOAK_SECONDS=1800 OLR_RTMP_SOAK_CODEC=avc \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_soak --output-on-failure
```

Run HEVC soak:

```bash
OLR_RTMP_RUN_SOAK=1 OLR_RTMP_SOAK_SECONDS=1800 OLR_RTMP_SOAK_CODEC=hevc \
ctest --test-dir build/native-rtmp -R e2e_native_rtmp_soak --output-on-failure
```

If this exits 77 because local FFmpeg cannot generate HEVC with `hevc_videotoolbox` or `libx265`, keep native RTMP opt-in and record that blocker in the final local handoff.

- [ ] **Step 5: If any readiness gate fails, do not flip default**

Leave `OLR_NATIVE_RTMP=1` opt-in. Write a short local note in the final response listing which gate is missing.

- [ ] **Step 6: If all readiness gates pass, write failing default-flip test**

In `tst_ingestbackendselector.cpp`, change default expectation:

```cpp
void TestIngestBackendSelector::environmentDefaultsRtmpAndRtmpsToNativeWhenAvailable() {
    ScopedEnv nativeRtmp("OLR_NATIVE_RTMP");
    ScopedEnv forceFfmpeg("OLR_FFMPEG_RTMP");
    qunsetenv("OLR_NATIVE_RTMP");
    qunsetenv("OLR_FFMPEG_RTMP");

    IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")),
                                            false, true);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);
}
```

- [ ] **Step 7: Implement default flip**

Change `nativeRtmpEnabledByEnvironment` to default true unless explicitly disabled:

```cpp
bool nativeRtmpEnabledByEnvironment() {
    if (qEnvironmentVariableIsSet("OLR_FFMPEG_RTMP")) {
        return false;
    }
    const QString value = qEnvironmentVariable("OLR_NATIVE_RTMP").trimmed().toLower();
    return !(value == QStringLiteral("0") || value == QStringLiteral("false") ||
             value == QStringLiteral("off") || value == QStringLiteral("no"));
}
```

- [ ] **Step 8: Verify after default flip**

```bash
cmake --build build/native-rtmp --target tst_ingestbackendselector record_harness sync_harness OpenLiveReplay -j2
ctest --test-dir build/native-rtmp -R tst_ingestbackendselector --output-on-failure
ctest --test-dir build/native-rtmp -L native-rtmp --output-on-failure
git diff --check
```

- [ ] **Step 9: Commit**

If default was not flipped:

```bash
git status --short
```

Do not commit a default flip.

If default was flipped:

```bash
git add recorder_engine/ingest/ingestsession.cpp tests/unit/tst_ingestbackendselector.cpp tests/README.md
git commit -m "feat: default rtmp to native after readiness gates"
```

---

## Final Branch Acceptance Checklist

- [ ] Native RTMP remains opt-in until all readiness gates pass.
- [ ] Parser unit tests cover arbitrary fragmentation and extended timestamps.
- [ ] URL query auth is preserved and tested.
- [ ] Legacy AVC/AAC RTMP and RTMPS pass.
- [ ] E-RTMP HEVC RTMP and RTMPS pass or are explicitly blocked by local HEVC fixture availability with a documented skip.
- [ ] Unsupported profiles do not produce connected-black output.
- [ ] Native profile failures fall back to FFmpeg unless fallback is disabled.
- [ ] Hostile fixture fragmentation matrix passes.
- [ ] Real-server AVC interop has been run and result recorded.
- [ ] Real-server HEVC/E-RTMP interop has been run or a concrete server/tooling blocker is documented.
- [ ] 30-minute AVC soak has passed.
- [ ] 30-minute HEVC soak has passed or a concrete fixture/tooling blocker is documented.
- [ ] `ctest --test-dir build/native-rtmp -L native-rtmp --repeat until-fail:3 --output-on-failure` passes.
- [ ] `git diff --check` passes.

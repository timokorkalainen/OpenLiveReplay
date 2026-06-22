#include <QtTest>

#include "recorder_engine/ingest/h26xaccessunit.h"
#include "recorder_engine/ingest/ingestsession.h"
#include "recorder_engine/ingest/nativesrtingestsession.h"
#include "recorder_engine/timing/smpte12m.h"
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
#include "recorder_engine/ingest/nativertmpingestsession.h"
#endif

#include <algorithm>

class TestIngestBackendSelector : public QObject {
    Q_OBJECT
private slots:
    void srtRoutesToNativeSrt();
    void srtHostnameWithStreamIdRoutesToNativeSrt();
    void srtStreamIdIsDecodedForSocketOption();
    void srtEncryptedUrlIsSupported();
    void srtBadEncryptionIsRejected();
    void srtListenerAndRendezvousAreSupported();
    void srtUnknownModeIsRejected();
    void rtmpRoutesToNativeRtmp();
    void ndiRoutesToNativeNdi();
    void unsupportedSchemesAreRejected();
    void srtIsNativeByDefaultWithoutEnv();
    void rtmpIsNativeByDefault();
    void ndiIsNativeByDefaultWhenAvailable();
    void nativeFailureStopsNativeRetryWithoutFfmpegFallback();
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
    void malformedLegacyVideoPacketStaysMalformed();
    void nativeRtmpConnectAdvertisesEnhancedCodecCapabilities();
    void nativeRtmpAcknowledgesServerReceiveWindows();
    void nativeRtmpRejectsUnrepresentableOutgoingMessage();
    void nativeRtmpAudioFollowsSharedVideoAnchor();
    void nativeRtmpVideoBindsToSharedAnchorWhenAudioArrivesFirst();
    void nativeRtmpAudioFollowsVideoReanchorWithoutDrift();
    void nativeRtmpAudioDiscontinuityKeepsSharedAnchor();
    void nativeRtmpAllowsNegativeCompositionTimeAfterAnchor();
    void nativeRtmpCanUseExternallyOwnedClockAcrossSessions();
    void nativeRtmpExtractsSeiTimecodeOntoPendingVideoTimecode();
    void nativeRtmpNoSeiTimecodeLeavesNoStaleTimecodeBleed();
    void nativeRtmpSeiTimecodeOverridesAmfFallback();
    void nativeRtmpAmfFallbackTimecodePersistsUntilNextSei();
    void nativeRtmpOnMetaDataDataMessageSetsAmfTimecode();
#endif
    void sharedAnchorMapsStreamTime();
    void srtAnchorMapPureCases();
    void nativeSrtUnwrapsThirtyThreeBitTimestampWrap();
    void nativeSrtExtractsSeiTimecodeOntoPendingVideoTimecode();
    void nativeSrtNoSeiTimecodeLeavesNoStaleTimecodeBleed();
};

void TestIngestBackendSelector::srtRoutesToNativeSrt() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::NativeSrt);
}
void TestIngestBackendSelector::srtHostnameWithStreamIdRoutesToNativeSrt() {
    const QUrl url(QStringLiteral("srt://maps.rally.promo:8890?streamid=read:stream4"));
    QVERIFY(NativeSrtIngestSession::supportsUrl(url));

    const IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(url, NativeSrtIngestSession::supportsUrl(url), false);
    QVERIFY(opts.preferNativeSrt);
    QCOMPARE(selectIngestBackend(url, opts), IngestBackendKind::NativeSrt);
}
void TestIngestBackendSelector::srtStreamIdIsDecodedForSocketOption() {
    QCOMPARE(NativeSrtIngestSession::streamIdForSocketOption(
                 QUrl(QStringLiteral("srt://maps.rally.promo:8890?streamid=read%3Astream4"))),
             QByteArray("read:stream4"));
    QVERIFY(NativeSrtIngestSession::streamIdForSocketOption(
                QUrl(QStringLiteral("srt://maps.rally.promo:8890")))
                .isEmpty());
}
void TestIngestBackendSelector::srtEncryptedUrlIsSupported() {
    // Encrypted SRT (passphrase + optional pbkeylen) is restored natively: the URL
    // is accepted and routes to NativeSrt, and the parsed options carry the
    // passphrase + key length so the socket setup can apply SRTO_PASSPHRASE.
    const QUrl url(QStringLiteral("srt://127.0.0.1:9000?passphrase=secretpass123&pbkeylen=16"));
    QVERIFY(NativeSrtIngestSession::supportsUrl(url));

    const IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(url, NativeSrtIngestSession::supportsUrl(url), false);
    QVERIFY(opts.preferNativeSrt);
    QCOMPARE(selectIngestBackend(url, opts), IngestBackendKind::NativeSrt);

    // A passphrase without an explicit pbkeylen is also valid (defaults to AES-128).
    QVERIFY(NativeSrtIngestSession::supportsUrl(
        QUrl(QStringLiteral("srt://127.0.0.1:9000?passphrase=secretpass123"))));
}

void TestIngestBackendSelector::srtBadEncryptionIsRejected() {
    // pbkeylen must be 16/24/32; anything else is genuinely-bad input and must be
    // rejected so the scheme-aware unsupported diagnostic stays accurate.
    QVERIFY(!NativeSrtIngestSession::supportsUrl(
        QUrl(QStringLiteral("srt://127.0.0.1:9000?passphrase=secretpass123&pbkeylen=20"))));
    // SRT passphrases must be 10..79 characters; a too-short one is invalid.
    QVERIFY(!NativeSrtIngestSession::supportsUrl(
        QUrl(QStringLiteral("srt://127.0.0.1:9000?passphrase=short"))));
    // pbkeylen without any passphrase is meaningless (encryption needs a key).
    QVERIFY(!NativeSrtIngestSession::supportsUrl(
        QUrl(QStringLiteral("srt://127.0.0.1:9000?pbkeylen=16"))));
}

void TestIngestBackendSelector::srtListenerAndRendezvousAreSupported() {
    // Native SRT was caller-only; listener + rendezvous are restored. Both URLs are
    // accepted, route to NativeSrt, and the session reports the parsed mode so the
    // socket setup can bind+listen (listener) or set SRTO_RENDEZVOUS (rendezvous).
    const QUrl listener(QStringLiteral("srt://127.0.0.1:9000?mode=listener"));
    QVERIFY(NativeSrtIngestSession::supportsUrl(listener));
    QCOMPARE(NativeSrtIngestSession::connectionModeForUrl(listener), NativeSrtMode::Listener);
    const IngestBackendOptions listenerOpts = ingestBackendOptionsFromEnvironment(
        listener, NativeSrtIngestSession::supportsUrl(listener), false);
    QCOMPARE(selectIngestBackend(listener, listenerOpts), IngestBackendKind::NativeSrt);

    const QUrl rendezvous(QStringLiteral("srt://127.0.0.1:9000?mode=rendezvous"));
    QVERIFY(NativeSrtIngestSession::supportsUrl(rendezvous));
    QCOMPARE(NativeSrtIngestSession::connectionModeForUrl(rendezvous), NativeSrtMode::Rendezvous);

    // Default + explicit caller are unchanged (no behavior change for existing URLs).
    QCOMPARE(
        NativeSrtIngestSession::connectionModeForUrl(QUrl(QStringLiteral("srt://127.0.0.1:9000"))),
        NativeSrtMode::Caller);
    QCOMPARE(NativeSrtIngestSession::connectionModeForUrl(
                 QUrl(QStringLiteral("srt://127.0.0.1:9000?mode=caller"))),
             NativeSrtMode::Caller);
}

void TestIngestBackendSelector::srtUnknownModeIsRejected() {
    // An unrecognised mode is genuinely-bad input: rejected so the scheme-aware
    // unsupported diagnostic stays accurate (it is not silently treated as caller).
    QVERIFY(!NativeSrtIngestSession::supportsUrl(
        QUrl(QStringLiteral("srt://127.0.0.1:9000?mode=bogus"))));
}

void TestIngestBackendSelector::rtmpRoutesToNativeRtmp() {
    IngestBackendOptions opts;
    opts.preferNativeRtmp = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::NativeRtmp);
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
    const QUrl signedUrl(QStringLiteral("rtmp://maps.rally.promo/live/stream4?token=abc"));
    QVERIFY(NativeRtmpIngestSession::supportsUrl(signedUrl));
    const IngestBackendOptions signedOpts = ingestBackendOptionsFromEnvironment(
        signedUrl, false, NativeRtmpIngestSession::supportsUrl(signedUrl));
    QVERIFY(signedOpts.preferNativeRtmp);
    QCOMPARE(selectIngestBackend(signedUrl, signedOpts), IngestBackendKind::NativeRtmp);

    const QUrl streamIdUrl(
        QStringLiteral("rtmp://maps.rally.promo:8890/live/stream4?streamid=read:stream4"));
    QVERIFY(NativeRtmpIngestSession::supportsUrl(streamIdUrl));
    const IngestBackendOptions streamIdOpts = ingestBackendOptionsFromEnvironment(
        streamIdUrl, false, NativeRtmpIngestSession::supportsUrl(streamIdUrl));
    QVERIFY(streamIdOpts.preferNativeRtmp);
    QCOMPARE(selectIngestBackend(streamIdUrl, streamIdOpts), IngestBackendKind::NativeRtmp);
#endif
}
void TestIngestBackendSelector::ndiRoutesToNativeNdi() {
    IngestBackendOptions opts;
    opts.preferNativeNdi = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("ndi://STUDIO%20(CAM1)")), opts),
             IngestBackendKind::NativeNdi);
}
void TestIngestBackendSelector::unsupportedSchemesAreRejected() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;
    opts.preferNativeRtmp = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("udp://127.0.0.1:1234")), opts),
             IngestBackendKind::Unsupported);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("file:///tmp/x.ts")), opts),
             IngestBackendKind::Unsupported);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("http://example.test/x")), opts),
             IngestBackendKind::Unsupported);
}
void TestIngestBackendSelector::srtIsNativeByDefaultWithoutEnv() {
    qunsetenv("OLR_NATIVE_SRT");
    const IngestBackendOptions opts = ingestBackendOptionsFromEnvironment(
        QUrl(QStringLiteral("srt://127.0.0.1:9000")), true, false);
    QVERIFY(opts.preferNativeSrt);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::NativeSrt);
}
void TestIngestBackendSelector::rtmpIsNativeByDefault() {
    const IngestBackendOptions opts = ingestBackendOptionsFromEnvironment(
        QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), false, true);
    QVERIFY(opts.preferNativeRtmp);
}

void TestIngestBackendSelector::ndiIsNativeByDefaultWhenAvailable() {
    const IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("ndi://CAM1")), false, false, true);
    QVERIFY(opts.preferNativeNdi);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("ndi://CAM1")), opts),
             IngestBackendKind::NativeNdi);
}

void TestIngestBackendSelector::nativeFailureStopsNativeRetryWithoutFfmpegFallback() {
    QVERIFY(shouldStopNativeRtmpAfterFailure(IngestFailureKind::UnsupportedProfile));
    QVERIFY(shouldStopNativeRtmpAfterFailure(IngestFailureKind::DecodeCapability));
    QVERIFY(shouldStopNativeRtmpAfterFailure(IngestFailureKind::MalformedStream));
    QVERIFY(!shouldStopNativeRtmpAfterFailure(IngestFailureKind::TransientNetwork));
    QVERIFY(!shouldStopNativeRtmpAfterFailure(IngestFailureKind::None));
}

#if defined(OLR_NATIVE_RTMP_AVAILABLE)
void TestIngestBackendSelector::malformedLegacyVideoPacketStaysMalformed() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    QStringList logLines;
    IngestCallbacks callbacks;
    callbacks.logInfo = [&logLines](const QString& message) { logLines.append(message); };
    session.m_callbacks = callbacks;

    session.processVideoMessage(0, QByteArray::fromHex("02"));

    QCOMPARE(session.lastFailureKind(), IngestFailureKind::MalformedStream);
    QVERIFY(session.m_unsupportedReason.isEmpty());
    QVERIFY(std::any_of(logLines.cbegin(), logLines.cend(), [](const QString& message) {
        return message.contains(QStringLiteral("Native RTMP video parse failed"));
    }));
}

void TestIngestBackendSelector::nativeRtmpConnectAdvertisesEnhancedCodecCapabilities() {
    QCOMPARE(NativeRtmpIngestSession::connectCodecProfile(),
             RtmpConnectCodecProfile::EnhancedAvcHevcAac);
}

void TestIngestBackendSelector::nativeRtmpAcknowledgesServerReceiveWindows() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    session.configureAcknowledgementWindow(10);
    quint32 sequence = 0;
    QVERIFY(!session.noteIncomingChunkBytes(9, &sequence));
    QCOMPARE(sequence, quint32(0));
    QVERIFY(session.noteIncomingChunkBytes(1, &sequence));
    QCOMPARE(sequence, quint32(10));
    QVERIFY(!session.noteIncomingChunkBytes(9, &sequence));
    QCOMPARE(sequence, quint32(0));
    QVERIFY(session.noteIncomingChunkBytes(1, &sequence));
    QCOMPARE(sequence, quint32(20));

    NativeRtmpIngestSession alreadyReadSession(0, 640, 480, nullptr);
    QVERIFY(!alreadyReadSession.noteIncomingChunkBytes(12, &sequence));
    alreadyReadSession.configureAcknowledgementWindow(10);
    QVERIFY(alreadyReadSession.noteIncomingChunkBytes(0, &sequence));
    QCOMPARE(sequence, quint32(12));
}

void TestIngestBackendSelector::nativeRtmpRejectsUnrepresentableOutgoingMessage() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);
    QString error;

    QVERIFY(!session.sendMessage(3, 20, 1, 0, QByteArray(0x1000000, char('x')), &error));
    QVERIFY(error.contains(QStringLiteral("too large"), Qt::CaseInsensitive));
}

void TestIngestBackendSelector::nativeRtmpAudioFollowsSharedVideoAnchor() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 10000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    // First video establishes the shared anchor: FLV ms 1000 -> recording clock 10000.
    QCOMPARE(session.sourcePtsMsForVideo(1000, 1000), int64_t(10000));

    // Co-timed audio binds to the SAME shared anchor; it must not read a fresh clock.
    clockMs = 99999;
    QCOMPARE(session.sourcePtsMsForAudio(1000), int64_t(10000));

    // Later audio advances against the unchanged anchor.
    QCOMPARE(session.sourcePtsMsForAudio(1100), int64_t(10100));
}

void TestIngestBackendSelector::nativeRtmpVideoBindsToSharedAnchorWhenAudioArrivesFirst() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 100;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    // Audio with no prior video establishes the shared anchor against the clock.
    QCOMPARE(session.sourcePtsMsForAudio(0), int64_t(100));

    // A later co-timed video maps against THAT shared anchor, not a fresh clock read.
    clockMs = 5000;
    QCOMPARE(session.sourcePtsMsForVideo(0, 0), int64_t(100));
    QCOMPARE(session.sourcePtsMsForAudio(40), int64_t(140));
}

void TestIngestBackendSelector::nativeRtmpAudioFollowsVideoReanchorWithoutDrift() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 10000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    // Shared anchor established by first video at FLV ms 1000 -> clock 10000.
    QCOMPARE(session.sourcePtsMsForVideo(1000, 1000), int64_t(10000));
    QCOMPARE(session.sourcePtsMsForAudio(1000), int64_t(10000));

    // Video forward jump (delta 8000 > kForwardJumpMs=3000) re-anchors to clock 20000.
    clockMs = 20000;
    QCOMPARE(session.sourcePtsMsForVideo(9000, 9000), int64_t(20000));

    // Audio at the same FLV clock FOLLOWS the new shared anchor: no A/V drift.
    clockMs = 99999;
    QCOMPARE(session.sourcePtsMsForAudio(9000), int64_t(20000));
}

void TestIngestBackendSelector::nativeRtmpAudioDiscontinuityKeepsSharedAnchor() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 10000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    // Shared anchor established by first video at FLV ms 1000 -> clock 10000.
    QCOMPARE(session.sourcePtsMsForVideo(1000, 1000), int64_t(10000));

    // Audio advances normally; m_prevAudioPtsMs becomes 1000.
    QCOMPARE(session.sourcePtsMsForAudio(1000), int64_t(10000));

    // An AUDIO discontinuity NOT co-timed with any video re-anchor (delta
    // 5000 - 1000 = 4000 > kForwardJumpMs=3000) flushes the audio decoder but
    // must NOT move the shared anchor — video owns re-anchoring. The clock is
    // moved to a sentinel the audio path must ignore: if it wrongly re-anchored
    // to the fresh clock the result would be 99999, not 14000.
    clockMs = 99999;
    QCOMPARE(session.sourcePtsMsForAudio(5000), int64_t(14000));

    // The shared anchor is unchanged (media 1000 / stream 10000), so the next
    // audio packet still maps against it.
    QCOMPARE(session.sourcePtsMsForAudio(5040), int64_t(14040));
}

void TestIngestBackendSelector::nativeRtmpAllowsNegativeCompositionTimeAfterAnchor() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    int64_t clockMs = 1000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    QCOMPARE(session.sourcePtsMsForVideo(0, -20), int64_t(980));
    QCOMPARE(session.sourcePtsMsForVideo(40, 20), int64_t(1020));
}

void TestIngestBackendSelector::nativeRtmpCanUseExternallyOwnedClockAcrossSessions() {
    AnchoredSourceClock sharedClock(ClockQuality::FlvPll);

    int64_t clockMs = 5000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };

    {
        NativeRtmpIngestSession first(0, 640, 480, nullptr, &sharedClock);
        first.m_callbacks = callbacks;
        QCOMPARE(first.sourcePtsMsForVideo(1000, 1000), int64_t(5000));
    }

    clockMs = 99999;
    NativeRtmpIngestSession second(0, 640, 480, nullptr, &sharedClock);
    second.m_callbacks = callbacks;
    QCOMPARE(second.sourcePtsMsForVideo(1100, 1100), int64_t(5100));
}
#endif

void TestIngestBackendSelector::sharedAnchorMapsStreamTime() {
    // The single shared A/V anchor maps a 90kHz stream timestamp onto the recording
    // timeline: sourceMs = anchorStreamMs + (pts90k - anchorTs90k)/90; -1 on any
    // negative input. (1000 + (108000-90000)/90 = 1200.)
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(108000, 90000, 1000), 1200);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(-1, 90000, 1000), -1);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(108000, -1, 1000), -1);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(108000, 90000, -1), -1);
}

void TestIngestBackendSelector::srtAnchorMapPureCases() {
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(90000, 90000, 5000), 5000);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(180000, 90000, 5000), 6000);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(-1, 90000, 5000), -1);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(180000, -1, 5000), -1);
    QCOMPARE(NativeSrtIngestSession::sourcePtsMsFromAnchor(180000, 90000, -1), -1);
}

void TestIngestBackendSelector::nativeSrtUnwrapsThirtyThreeBitTimestampWrap() {
    NativeSrtIngestSession session(0, 640, 480, nullptr);
    constexpr int64_t wrap90k = 1LL << 33;

    int64_t clockMs = 1000;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = [&clockMs]() { return clockMs; };
    session.m_callbacks = callbacks;

    QCOMPARE(session.sourcePtsMsForAudio(wrap90k - 90), int64_t(1000));
    clockMs = 99999;
    QCOMPARE(session.sourcePtsMsForAudio(90), int64_t(1002));
}

namespace {

// Assemble a minimal H.264 Annex-B access unit carrying a pic_timing SEI
// (payloadType 1) whose payload is the big-endian SMPTE 12M word, followed by a
// dummy VCL NAL — the same on-wire shape tst_h26xseitimecode builds.
QByteArray h264AccessUnitWithSeiTimecode(const Smpte12mTimecode& tc) {
    const char startCode[4] = {0x00, 0x00, 0x00, 0x01};
    const uint32_t word = Smpte12m::toPackedWord(tc);
    QByteArray payload(4, char(0));
    payload[0] = char((word >> 24) & 0xFF);
    payload[1] = char((word >> 16) & 0xFF);
    payload[2] = char((word >> 8) & 0xFF);
    payload[3] = char(word & 0xFF);

    QByteArray rbsp;
    rbsp.append(char(1));              // payloadType = 1 (pic_timing)
    rbsp.append(char(payload.size())); // payloadSize = 4
    rbsp.append(payload);

    QByteArray annexB;
    annexB.append(startCode, 4);
    annexB.append(char(0x06)); // SEI NAL header (nal_type 6)
    annexB.append(rbsp);
    annexB.append(char(0x80)); // RBSP trailing-bits stop byte
    annexB.append(startCode, 4);
    annexB.append(QByteArray::fromHex("658884")); // dummy IDR VCL NAL
    return annexB;
}

QByteArray h264AccessUnitWithoutSei() {
    const char startCode[4] = {0x00, 0x00, 0x00, 0x01};
    QByteArray annexB;
    annexB.append(startCode, 4);
    annexB.append(QByteArray::fromHex("658884")); // VCL NAL only, no SEI
    return annexB;
}

} // namespace

void TestIngestBackendSelector::nativeSrtExtractsSeiTimecodeOntoPendingVideoTimecode() {
    NativeSrtIngestSession session(0, 640, 480, nullptr);

    CompressedAccessUnit unit;
    unit.codec = NativeVideoCodec::H264;
    const Smpte12mTimecode tc{10, 11, 12, 13, /*drop*/ false, /*valid*/ true};
    unit.annexB = h264AccessUnitWithSeiTimecode(tc);

    session.updatePendingVideoTimecode(unit);

    // The session stamps DecodedVideoFrame::sourceTimecode100ns from this member;
    // it must equal the encoded TC mapped to 100 ns at the nominal fps.
    QCOMPARE(session.m_pendingVideoTimecode100ns,
             Smpte12m::to100ns(tc, NativeSrtIngestSession::kTimecodeNominalFps));
    QVERIFY(session.m_pendingVideoTimecode100ns >= 0);
}

void TestIngestBackendSelector::nativeSrtNoSeiTimecodeLeavesNoStaleTimecodeBleed() {
    NativeSrtIngestSession session(0, 640, 480, nullptr);

    // A frame WITH timecode seeds the pending member.
    CompressedAccessUnit withTc;
    withTc.codec = NativeVideoCodec::H264;
    withTc.annexB = h264AccessUnitWithSeiTimecode(Smpte12mTimecode{1, 0, 0, 0, false, true});
    session.updatePendingVideoTimecode(withTc);
    QVERIFY(session.m_pendingVideoTimecode100ns >= 0);

    // A subsequent frame with NO SEI must report none (-1), not the previous TC.
    CompressedAccessUnit noTc;
    noTc.codec = NativeVideoCodec::H264;
    noTc.annexB = h264AccessUnitWithoutSei();
    session.updatePendingVideoTimecode(noTc);
    QCOMPARE(session.m_pendingVideoTimecode100ns, int64_t(-1));
}

#if defined(OLR_NATIVE_RTMP_AVAILABLE)
void TestIngestBackendSelector::nativeRtmpExtractsSeiTimecodeOntoPendingVideoTimecode() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    const Smpte12mTimecode tc{10, 11, 12, 13, /*drop*/ false, /*valid*/ true};
    session.updatePendingVideoTimecode(h264AccessUnitWithSeiTimecode(tc), NativeVideoCodec::H264);

    // The session stamps DecodedVideoFrame::sourceTimecode100ns from this member;
    // it must equal the encoded SEI TC mapped to 100 ns at the nominal fps.
    QCOMPARE(session.m_pendingVideoTimecode100ns,
             Smpte12m::to100ns(tc, NativeRtmpIngestSession::kTimecodeNominalFps));
    QVERIFY(session.m_pendingVideoTimecode100ns >= 0);
}

void TestIngestBackendSelector::nativeRtmpNoSeiTimecodeLeavesNoStaleTimecodeBleed() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    // A frame WITH timecode seeds the pending member.
    session.updatePendingVideoTimecode(
        h264AccessUnitWithSeiTimecode(Smpte12mTimecode{1, 0, 0, 0, false, true}),
        NativeVideoCodec::H264);
    QVERIFY(session.m_pendingVideoTimecode100ns >= 0);

    // A subsequent frame with NO SEI (and no AMF fallback) must report none (-1),
    // never the previous frame's TC.
    session.updatePendingVideoTimecode(h264AccessUnitWithoutSei(), NativeVideoCodec::H264);
    QCOMPARE(session.m_pendingVideoTimecode100ns, int64_t(-1));
}

void TestIngestBackendSelector::nativeRtmpSeiTimecodeOverridesAmfFallback() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    // AMF onMetaData carried a timecode string (the fallback).
    session.applyAmfTimecodeString(QStringLiteral("01:00:00:00"));
    QVERIFY(session.m_amfTimecode100ns >= 0);

    // A frame that ALSO carries an SEI TC: the SEI wins over the AMF fallback.
    const Smpte12mTimecode sei{02, 03, 04, 05, false, true};
    session.updatePendingVideoTimecode(h264AccessUnitWithSeiTimecode(sei), NativeVideoCodec::H264);
    QCOMPARE(session.m_pendingVideoTimecode100ns,
             Smpte12m::to100ns(sei, NativeRtmpIngestSession::kTimecodeNominalFps));
}

void TestIngestBackendSelector::nativeRtmpAmfFallbackTimecodePersistsUntilNextSei() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    // No SEI yet: a frame with no SEI reports -1 (no AMF, no SEI).
    session.updatePendingVideoTimecode(h264AccessUnitWithoutSei(), NativeVideoCodec::H264);
    QCOMPARE(session.m_pendingVideoTimecode100ns, int64_t(-1));

    // onMetaData delivers an AMF timecode string; a malformed one is ignored.
    session.applyAmfTimecodeString(QStringLiteral("garbage"));
    QCOMPARE(session.m_amfTimecode100ns, int64_t(-1));
    session.applyAmfTimecodeString(QStringLiteral("01:00:00:00"));
    const Smpte12mTimecode amf{1, 0, 0, 0, false, true};
    const int64_t amf100ns = Smpte12m::to100ns(amf, NativeRtmpIngestSession::kTimecodeNominalFps);
    QCOMPARE(session.m_amfTimecode100ns, amf100ns);

    // Frames with NO SEI now carry the AMF fallback TC (it persists across frames).
    session.updatePendingVideoTimecode(h264AccessUnitWithoutSei(), NativeVideoCodec::H264);
    QCOMPARE(session.m_pendingVideoTimecode100ns, amf100ns);
    session.updatePendingVideoTimecode(h264AccessUnitWithoutSei(), NativeVideoCodec::H264);
    QCOMPARE(session.m_pendingVideoTimecode100ns, amf100ns);

    // A frame WITH an SEI TC overrides the fallback for that frame...
    const Smpte12mTimecode sei{02, 03, 04, 05, false, true};
    session.updatePendingVideoTimecode(h264AccessUnitWithSeiTimecode(sei), NativeVideoCodec::H264);
    QCOMPARE(session.m_pendingVideoTimecode100ns,
             Smpte12m::to100ns(sei, NativeRtmpIngestSession::kTimecodeNominalFps));

    // ...and the AMF fallback resumes on the next SEI-less frame.
    session.updatePendingVideoTimecode(h264AccessUnitWithoutSei(), NativeVideoCodec::H264);
    QCOMPARE(session.m_pendingVideoTimecode100ns, amf100ns);
}

void TestIngestBackendSelector::nativeRtmpOnMetaDataDataMessageSetsAmfTimecode() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);

    // Build a real AMF0 onMetaData data message: ["onMetaData", { ... timecode ... }]
    // and drive it through the production processMessage dispatch (type 18).
    RtmpMessage meta;
    meta.type = 18; // RTMP AMF0 data
    meta.payload = RtmpAmf0::string(QStringLiteral("onMetaData"));
    meta.payload.append(RtmpAmf0::object(
        {{QStringLiteral("width"), RtmpAmf0::number(1920)},
         {QStringLiteral("timecode"), RtmpAmf0::string(QStringLiteral("01:00:00:00"))},
         {QStringLiteral("framerate"), RtmpAmf0::number(30)}}));
    session.processMessage(meta);

    const Smpte12mTimecode amf{1, 0, 0, 0, false, true};
    QCOMPARE(session.m_amfTimecode100ns,
             Smpte12m::to100ns(amf, NativeRtmpIngestSession::kTimecodeNominalFps));

    // A frame with no SEI now carries that AMF timecode.
    session.updatePendingVideoTimecode(h264AccessUnitWithoutSei(), NativeVideoCodec::H264);
    QCOMPARE(session.m_pendingVideoTimecode100ns, session.m_amfTimecode100ns);
}
#endif

QTEST_GUILESS_MAIN(TestIngestBackendSelector)
#include "tst_ingestbackendselector.moc"

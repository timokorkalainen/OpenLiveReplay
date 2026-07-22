#include <QtTest>

#include "recorder_engine/ingest/nativertmpingestsession.h"
#include "recorder_engine/ingest/nativesrtingestsession.h"

#include <QFile>

class TestIngestTimecodeEvidence : public QObject {
    Q_OBJECT

private slots:
    void decodedFrameCarriesEvidenceInsteadOfPrimitiveAlignmentFields();
    void srtStandardAccessUnitProducesGenerationBoundEvidence();
    void srtUnchangedParameterSetsKeepTimingGeneration();
    void srtCodecReplacementBumpsSourceGenerationOnce();
    void srtDtsDiscontinuityBumpsSourceGenerationOnce();
    void srtHevcDiscontinuitySurvivesProductionAccessUnit();
    void rtmpSequenceAndVideoMessagesProduceStandardEvidence();
    void rtmpStructuredMetadataCanonicalizesAndAdvances();
    void rtmpRepeatedMetadataDoesNotReanchorOrRepeatLabel();
    void rtmpSemanticallyIdenticalMetadataDoesNotReapply();
    void rtmpCodecAndConfigurationReplacementBumpSourceGenerationOnce();
    void rtmpIgnoresFpsAliasAndNestedBytePattern();
    void rtmpCodecTimingPreventsMetadataFallback();
};

namespace {

QByteArray fixture(const char* name) {
    QFile file(QStringLiteral(OLR_SOURCE_DIR "/tests/fixtures/timecode/") +
               QString::fromLatin1(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QByteArray h264NoHrdSps() {
    return QByteArray::fromHex("6742001efb908000000300800000194a");
}

CompressedAccessUnit standardH264Unit() {
    CompressedAccessUnit unit;
    unit.codec = NativeVideoCodec::H264;
    unit.pts90k = 180000;
    unit.dts90k = 180000;
    unit.annexB = fixture("h264_pic_timing_no_hrd.264");
    unit.parameterSets.h264Sps.append(h264NoHrdSps());
    return unit;
}

QByteArray h264VclOnly() {
    return QByteArray::fromHex("00000001658884");
}

RtmpMessage metadataMessage(const QList<QPair<QString, QByteArray>>& values) {
    RtmpMessage message;
    message.type = 18;
    message.payload = RtmpAmf0::string(QStringLiteral("onMetaData"));
    message.payload.append(RtmpAmf0::object(values));
    return message;
}

QByteArray avcSequenceHeaderPayload(QByteArray pps = QByteArray::fromHex("68ce06e2")) {
    const QByteArray sps = h264NoHrdSps();
    QByteArray config;
    config.append(char(1));
    config.append(char(0x42));
    config.append(char(0));
    config.append(char(0x1e));
    config.append(char(0xff));
    config.append(char(0xe1));
    config.append(char((sps.size() >> 8) & 0xff));
    config.append(char(sps.size() & 0xff));
    config.append(sps);
    config.append(char(1));
    config.append(char((pps.size() >> 8) & 0xff));
    config.append(char(pps.size() & 0xff));
    config.append(pps);

    QByteArray payload = QByteArray::fromHex("1700000000");
    payload.append(config);
    return payload;
}

void appendHevcArray(QByteArray* config, int nalType, const QByteArray& nal) {
    config->append(char(0x80 | nalType));
    config->append(char(0));
    config->append(char(1));
    config->append(char((nal.size() >> 8) & 0xff));
    config->append(char(nal.size() & 0xff));
    config->append(nal);
}

QByteArray hevcSequenceHeaderPayload() {
    const QByteArray vps = QByteArray::fromHex("40010c01ffff01600000030090000003000003005d959809");
    const QByteArray sps = QByteArray::fromHex("42010101600000030090000003000003005da00280802d1f");
    const QByteArray pps = QByteArray::fromHex("4401c172b46240");
    QByteArray config(23, char(0));
    config[0] = char(1);
    config[21] = char(0xff);
    config[22] = char(3);
    appendHevcArray(&config, 32, vps);
    appendHevcArray(&config, 33, sps);
    appendHevcArray(&config, 34, pps);

    QByteArray payload;
    payload.append(char(0x80)); // enhanced SequenceStart
    payload.append("hvc1", 4);
    payload.append(config);
    return payload;
}

QByteArray avcCodedTimecodePayload() {
    const QByteArray sei = QByteArray::fromHex("0601060904040c20c080");
    const QByteArray vcl = QByteArray::fromHex("658884");
    QByteArray payload = QByteArray::fromHex("1701000000");
    for (const QByteArray& nal : {sei, vcl}) {
        const uint32_t size = uint32_t(nal.size());
        payload.append(char((size >> 24) & 0xff));
        payload.append(char((size >> 16) & 0xff));
        payload.append(char((size >> 8) & 0xff));
        payload.append(char(size & 0xff));
        payload.append(nal);
    }
    return payload;
}

} // namespace

void TestIngestTimecodeEvidence::decodedFrameCarriesEvidenceInsteadOfPrimitiveAlignmentFields() {
    DecodedVideoFrame frame;
    QVERIFY(!frame.timecodeEvidence.has_value());
    frame.timecodeEvidence = TimecodeEvidence{};
    QVERIFY(frame.timecodeEvidence.has_value());
}

void TestIngestTimecodeEvidence::srtStandardAccessUnitProducesGenerationBoundEvidence() {
    NativeSrtIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 7;

    const CompressedAccessUnit unit = standardH264Unit();
    QVERIFY(!unit.annexB.isEmpty());
    session.updatePendingVideoTimecode(unit, 2000);

    QVERIFY(session.m_pendingTimecodeEvidence.has_value());
    const TimecodeEvidence& evidence = *session.m_pendingTimecodeEvidence;
    QCOMPARE(evidence.frameOfDay, int64_t((1 * 60 * 60 + 2 * 60 + 3) * 25 + 4));
    QCOMPARE(evidence.labelRate, (FrameRateQ{25, 1}));
    QCOMPARE(evidence.sourceGeneration, uint64_t(7));
    QCOMPARE(evidence.timingGeneration, uint64_t(1));
    QCOMPARE(evidence.provenance, TimecodeProvenance::H264PicTiming);
    QVERIFY(!evidence.discontinuity);
    QVERIFY(evidence.valid());
}

void TestIngestTimecodeEvidence::srtUnchangedParameterSetsKeepTimingGeneration() {
    NativeSrtIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 1;
    const CompressedAccessUnit unit = standardH264Unit();

    session.updatePendingVideoTimecode(unit, 2000);
    const uint64_t firstGeneration = session.m_timingContext.generation();
    session.updatePendingVideoTimecode(unit, 2040);

    QCOMPARE(firstGeneration, uint64_t(1));
    QCOMPARE(session.m_timingContext.generation(), firstGeneration);
    QVERIFY(session.m_pendingTimecodeEvidence.has_value());
    QCOMPARE(session.m_pendingTimecodeEvidence->timingGeneration, firstGeneration);
}

void TestIngestTimecodeEvidence::srtCodecReplacementBumpsSourceGenerationOnce() {
    NativeSrtIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 10;

    PesPacket packet;
    packet.kind = NativeElementaryStreamKind::Video;
    packet.videoCodec = NativeVideoCodec::H264;
    session.processPesPacket(packet);
    QCOMPARE(session.m_sourceGeneration, uint64_t(10));
    session.processPesPacket(packet);
    QCOMPARE(session.m_sourceGeneration, uint64_t(10));
    session.m_decodedFrameEvidence.enqueue({90'000, 1000, 10, std::nullopt});

    packet.videoCodec = NativeVideoCodec::Hevc;
    session.processPesPacket(packet);
    QCOMPARE(session.m_sourceGeneration, uint64_t(11));
    QCOMPARE(session.m_decodedFrameEvidence.size(), qsizetype(0));
    session.processPesPacket(packet);
    QCOMPARE(session.m_sourceGeneration, uint64_t(11));
}

void TestIngestTimecodeEvidence::srtDtsDiscontinuityBumpsSourceGenerationOnce() {
    NativeSrtIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 20;
    int64_t recordingNowMs = 1000;
    session.m_callbacks.recordingClockMs = [&recordingNowMs]() { return recordingNowMs; };

    CompressedAccessUnit unit;
    unit.codec = NativeVideoCodec::H264;
    unit.pts90k = 90'000;
    unit.dts90k = 90'000;
    QVERIFY(session.sourcePtsMsForUnit(unit) >= 0);
    QCOMPARE(session.m_sourceGeneration, uint64_t(20));
    session.m_decodedFrameEvidence.enqueue({90'000, 1000, 10, std::nullopt});

    recordingNowMs += 3001;
    unit.pts90k += 3001 * 90;
    unit.dts90k += 3001 * 90;
    QVERIFY(session.sourcePtsMsForUnit(unit) >= 0);
    QCOMPARE(session.m_sourceGeneration, uint64_t(21));
    QCOMPARE(session.m_decodedFrameEvidence.size(), qsizetype(0));

    recordingNowMs += 40;
    unit.pts90k += 40 * 90;
    unit.dts90k += 40 * 90;
    QVERIFY(session.sourcePtsMsForUnit(unit) >= 0);
    QCOMPARE(session.m_sourceGeneration, uint64_t(21));
}

void TestIngestTimecodeEvidence::srtHevcDiscontinuitySurvivesProductionAccessUnit() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::Hevc);
    QList<CompressedAccessUnit> units =
        splitter.pushPesPayload(fixture("hevc_hm_time_code.265"), 270000, 270000);
    units.append(splitter.flush());
    QVERIFY(!units.isEmpty());

    NativeSrtIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 3;
    session.updatePendingVideoTimecode(units.constFirst(), 3000);

    QVERIFY(session.m_pendingTimecodeEvidence.has_value());
    QCOMPARE(session.m_pendingTimecodeEvidence->provenance, TimecodeProvenance::HevcTimeCode);
    QVERIFY(session.m_pendingTimecodeEvidence->discontinuity);
    QCOMPARE(session.m_pendingTimecodeEvidence->sourceGeneration, uint64_t(4));
}

void TestIngestTimecodeEvidence::rtmpSequenceAndVideoMessagesProduceStandardEvidence() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 5;
    session.m_callbacks.recordingClockMs = []() { return int64_t(5000); };

    RtmpMessage sequence;
    sequence.type = 9;
    sequence.payload = avcSequenceHeaderPayload();
    session.processMessage(sequence);
    QCOMPARE(session.m_timingContext.generation(), uint64_t(1));
    session.processMessage(sequence);
    QCOMPARE(session.m_timingContext.generation(), uint64_t(1));

    RtmpMessage video;
    video.type = 9;
    video.timestampMs = 1000;
    video.payload = avcCodedTimecodePayload();
    session.processMessage(video);

    QVERIFY(session.m_pendingTimecodeEvidence.has_value());
    QCOMPARE(session.m_pendingTimecodeEvidence->frameOfDay,
             int64_t((1 * 60 * 60 + 2 * 60 + 3) * 25 + 4));
    QCOMPARE(session.m_pendingTimecodeEvidence->labelRate, (FrameRateQ{25, 1}));
    QCOMPARE(session.m_pendingTimecodeEvidence->provenance, TimecodeProvenance::H264PicTiming);
    QCOMPARE(session.m_pendingTimecodeEvidence->sourceGeneration, uint64_t(5));
}

void TestIngestTimecodeEvidence::rtmpStructuredMetadataCanonicalizesAndAdvances() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 9;
    session.processMessage(metadataMessage({
        {QStringLiteral("timecode"), RtmpAmf0::string(QStringLiteral("01:00:00:00"))},
        {QStringLiteral("framerate"), RtmpAmf0::number(29.97)},
    }));

    session.updatePendingVideoTimecode(h264VclOnly(), NativeVideoCodec::H264, 1000);
    QVERIFY(session.m_pendingTimecodeEvidence.has_value());
    const TimecodeEvidence first = *session.m_pendingTimecodeEvidence;
    QCOMPARE(first.frameOfDay, int64_t(108000));
    QCOMPARE(first.labelRate, (FrameRateQ{30000, 1001}));
    QCOMPARE(first.sourceGeneration, uint64_t(9));
    QCOMPARE(first.provenance, TimecodeProvenance::RtmpMetadata);
    QCOMPARE(session.m_pendingVideoTimecode100ns, int64_t(36'000'000'000));

    session.updatePendingVideoTimecode(h264VclOnly(), NativeVideoCodec::H264, 1000);
    QVERIFY(!session.m_pendingTimecodeEvidence.has_value());

    session.updatePendingVideoTimecode(h264VclOnly(), NativeVideoCodec::H264, 1034);
    QVERIFY(session.m_pendingTimecodeEvidence.has_value());
    QCOMPARE(session.m_pendingTimecodeEvidence->frameOfDay, first.frameOfDay + 1);
    QVERIFY(session.m_pendingTimecodeEvidence->valid());
}

void TestIngestTimecodeEvidence::rtmpRepeatedMetadataDoesNotReanchorOrRepeatLabel() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 2;
    const RtmpMessage metadata = metadataMessage({
        {QStringLiteral("timecode"), RtmpAmf0::string(QStringLiteral("01:00:00:00"))},
        {QStringLiteral("framerate"), RtmpAmf0::number(29.97)},
    });
    session.processMessage(metadata);
    QCOMPARE(session.m_amfMetadataParseCount, uint64_t(1));
    QCOMPARE(session.m_amfMetadataApplyCount, uint64_t(1));
    session.updatePendingVideoTimecode(h264VclOnly(), NativeVideoCodec::H264, 1000, 1000);
    QVERIFY(session.m_pendingTimecodeEvidence.has_value());
    const uint64_t timingGeneration = session.m_amfTimingGeneration;
    const int64_t anchorPts = session.m_amfAnchorPtsMs;

    session.processMessage(metadata);
    QCOMPARE(session.m_amfMetadataParseCount, uint64_t(1));
    QCOMPARE(session.m_amfMetadataApplyCount, uint64_t(1));
    QCOMPARE(session.m_amfTimingGeneration, timingGeneration);
    QCOMPARE(session.m_amfAnchorPtsMs, anchorPts);
    session.updatePendingVideoTimecode(h264VclOnly(), NativeVideoCodec::H264, 1000, 1000);
    QVERIFY(!session.m_pendingTimecodeEvidence.has_value());
}

void TestIngestTimecodeEvidence::rtmpSemanticallyIdenticalMetadataDoesNotReapply() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);
    session.processMessage(metadataMessage({
        {QStringLiteral("timecode"), RtmpAmf0::string(QStringLiteral("01:00:00:00"))},
        {QStringLiteral("framerate"), RtmpAmf0::number(29.97)},
    }));
    QCOMPARE(session.m_amfMetadataParseCount, uint64_t(1));
    QCOMPARE(session.m_amfMetadataApplyCount, uint64_t(1));
    session.updatePendingVideoTimecode(h264VclOnly(), NativeVideoCodec::H264, 1000, 1000);
    const uint64_t timingGeneration = session.m_amfTimingGeneration;
    const int64_t anchorPts = session.m_amfAnchorPtsMs;

    session.processMessage(metadataMessage({
        {QStringLiteral("comment"), RtmpAmf0::string(QStringLiteral("payload changed"))},
        {QStringLiteral("framerate"), RtmpAmf0::number(30000.0 / 1001.0)},
        {QStringLiteral("timecode"), RtmpAmf0::string(QStringLiteral("01:00:00:00"))},
    }));
    QCOMPARE(session.m_amfMetadataParseCount, uint64_t(2));
    QCOMPARE(session.m_amfMetadataApplyCount, uint64_t(1));
    QCOMPARE(session.m_amfTimingGeneration, timingGeneration);
    QCOMPARE(session.m_amfAnchorPtsMs, anchorPts);
}

void TestIngestTimecodeEvidence::rtmpCodecAndConfigurationReplacementBumpSourceGenerationOnce() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 30;

    RtmpMessage sequence;
    sequence.type = 9;
    sequence.payload = avcSequenceHeaderPayload();
    session.processMessage(sequence);
    QCOMPARE(session.m_sourceGeneration, uint64_t(30));
    session.processMessage(sequence);
    QCOMPARE(session.m_sourceGeneration, uint64_t(30));
    session.m_decodedFrameEvidence.enqueue({90'000, 1000, 10, std::nullopt});

    sequence.payload = avcSequenceHeaderPayload(QByteArray::fromHex("68ce06e3"));
    session.processMessage(sequence);
    QCOMPARE(session.m_sourceGeneration, uint64_t(31));
    QCOMPARE(session.m_decodedFrameEvidence.size(), qsizetype(0));
    session.processMessage(sequence);
    QCOMPARE(session.m_sourceGeneration, uint64_t(31));

    sequence.payload = hevcSequenceHeaderPayload();
    session.processMessage(sequence);
    QCOMPARE(session.m_sourceGeneration, uint64_t(32));
    session.processMessage(sequence);
    QCOMPARE(session.m_sourceGeneration, uint64_t(32));
}

void TestIngestTimecodeEvidence::rtmpIgnoresFpsAliasAndNestedBytePattern() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 1;
    const QByteArray fakePattern =
        QByteArrayLiteral("xx\0\x09framerate\0") + QByteArray::fromHex("403df853e2556b28");
    session.processMessage(metadataMessage({
        {QStringLiteral("timecode"), RtmpAmf0::string(QStringLiteral("01:00:00:00"))},
        {QStringLiteral("fps"), RtmpAmf0::number(29.97)},
        {QStringLiteral("comment"), RtmpAmf0::string(QString::fromLatin1(fakePattern))},
    }));

    session.updatePendingVideoTimecode(h264VclOnly(), NativeVideoCodec::H264, 1000);
    QVERIFY(!session.m_pendingTimecodeEvidence.has_value());
}

void TestIngestTimecodeEvidence::rtmpCodecTimingPreventsMetadataFallback() {
    NativeRtmpIngestSession session(0, 640, 480, nullptr);
    session.m_sourceGeneration = 1;
    session.m_timingContext.updateParameterSets(NativeVideoCodec::H264, {}, {h264NoHrdSps()});
    session.processMessage(metadataMessage({
        {QStringLiteral("timecode"), RtmpAmf0::string(QStringLiteral("01:00:00:00"))},
        {QStringLiteral("framerate"), RtmpAmf0::number(30.0)},
    }));

    session.updatePendingVideoTimecode(h264VclOnly(), NativeVideoCodec::H264, 1000);
    QVERIFY(!session.m_pendingTimecodeEvidence.has_value());
}

QTEST_GUILESS_MAIN(TestIngestTimecodeEvidence)
#include "tst_ingesttimecodeevidence.moc"

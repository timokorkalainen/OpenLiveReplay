#include <QtTest>

#include "recorder_engine/ingest/mpegtsparser.h"

#include <initializer_list>
#include <utility>

static QByteArray tsPacket(quint16 pid, bool payloadStart, const QByteArray& payload,
                           quint8 continuityCounter = 0)
{
    Q_ASSERT(payload.size() <= 184);

    QByteArray pkt(188, char(0xff));
    pkt[0] = char(0x47);
    pkt[1] = char((payloadStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1f));
    pkt[2] = char(pid & 0xff);

    if (payload.size() == 184) {
        pkt[3] = char(0x10 | (continuityCounter & 0x0f));
        memcpy(pkt.data() + 4, payload.constData(), size_t(payload.size()));
    } else {
        const int adaptationLength = 183 - payload.size();
        pkt[3] = char(0x30 | (continuityCounter & 0x0f));
        pkt[4] = char(adaptationLength);
        if (adaptationLength > 0) {
            pkt[5] = char(0x00);
        }
        memcpy(pkt.data() + 5 + adaptationLength, payload.constData(), size_t(payload.size()));
    }

    return pkt;
}

static QByteArray adaptationOnlyPacket(quint16 pid, quint8 continuityCounter, quint8 flags,
                                       int adaptationLength = 183)
{
    QByteArray pkt(188, char(0xff));
    pkt[0] = char(0x47);
    pkt[1] = char((pid >> 8) & 0x1f);
    pkt[2] = char(pid & 0xff);
    pkt[3] = char(0x20 | (continuityCounter & 0x0f));
    pkt[4] = char(adaptationLength);
    if (adaptationLength > 0 && pkt.size() > 5) {
        pkt[5] = char(flags);
    }
    return pkt;
}

static QByteArray patSection(quint16 pmtPid)
{
    QByteArray pat;
    pat.append(char(0x00)); // pointer field
    pat.append(QByteArray::fromHex("00b00d0001c100000001"));
    pat.append(char(0xe0 | ((pmtPid >> 8) & 0x1f)));
    pat.append(char(pmtPid & 0xff));
    pat.append(QByteArray(4, char(0x00))); // ignored CRC32
    return pat;
}

static QByteArray pmtSection(std::initializer_list<std::pair<quint8, quint16>> streams,
                             quint16 pcrPid)
{
    QByteArray body;
    body.append(QByteArray::fromHex("0001c10000"));
    body.append(char(0xe0 | ((pcrPid >> 8) & 0x1f)));
    body.append(char(pcrPid & 0xff));
    body.append(QByteArray::fromHex("f000"));
    for (const auto& stream : streams) {
        body.append(char(stream.first));
        body.append(char(0xe0 | ((stream.second >> 8) & 0x1f)));
        body.append(char(stream.second & 0xff));
        body.append(QByteArray::fromHex("f000"));
    }
    body.append(QByteArray(4, char(0x00))); // ignored CRC32

    QByteArray pmt;
    pmt.append(char(0x00)); // pointer field
    pmt.append(char(0x02));
    pmt.append(char(0xb0 | ((body.size() >> 8) & 0x0f)));
    pmt.append(char(body.size() & 0xff));
    pmt.append(body);
    return pmt;
}

static QByteArray malformedPmtSectionWithPartialVideo(quint16 videoPid)
{
    QByteArray body;
    body.append(QByteArray::fromHex("0001c10000"));
    body.append(char(0xe0 | ((videoPid >> 8) & 0x1f)));
    body.append(char(videoPid & 0xff));
    body.append(QByteArray::fromHex("f000"));
    body.append(char(0x1b));
    body.append(char(0xe0 | ((videoPid >> 8) & 0x1f)));
    body.append(char(videoPid & 0xff));
    body.append(QByteArray::fromHex("f000"));
    body.append(char(0x0f));
    body.append(QByteArray::fromHex("e103f0ff"));
    body.append(QByteArray(4, char(0x00))); // ignored CRC32

    QByteArray pmt;
    pmt.append(char(0x00)); // pointer field
    pmt.append(char(0x02));
    pmt.append(char(0xb0 | ((body.size() >> 8) & 0x0f)));
    pmt.append(char(body.size() & 0xff));
    pmt.append(body);
    return pmt;
}

static QByteArray encodePts(qint64 pts90k, quint8 prefix)
{
    QByteArray out;
    out.append(char(prefix | (((pts90k >> 30) & 0x07) << 1) | 0x01));
    out.append(char((pts90k >> 22) & 0xff));
    out.append(char((((pts90k >> 15) & 0x7f) << 1) | 0x01));
    out.append(char((pts90k >> 7) & 0xff));
    out.append(char(((pts90k & 0x7f) << 1) | 0x01));
    return out;
}

static QByteArray pesPacket(quint8 streamId, const QByteArray& payload, qint64 pts90k)
{
    QByteArray optional;
    optional.append(char(0x80));
    optional.append(char(0x80));
    optional.append(char(0x05));
    optional.append(encodePts(pts90k, 0x20));

    const int pesLength = optional.size() + payload.size();
    QByteArray pes = QByteArray::fromHex("000001");
    pes.append(char(streamId));
    pes.append(char((pesLength >> 8) & 0xff));
    pes.append(char(pesLength & 0xff));
    pes.append(optional);
    pes.append(payload);
    return pes;
}

class TestMpegTsParser : public QObject {
    Q_OBJECT

private slots:
    void rejectsBadSyncByte();
    void parsesPatPmtPid();
    void parsesH264VideoPidAndCodec();
    void parsesHevcVideoPidAndCodec();
    void parsesAacPid();
    void parsesLatmAacPid();
    void malformedPmtDoesNotPartiallyApply();
    void validPmtResetsAbsentStreams();
    void reassemblesPesAcrossPacketsAndExtractsPts();
    void dropsPesAfterContinuityGapUntilNextPayloadStart();
    void rejectsMalformedAdaptationOnlyLength();
    void adaptationOnlyDiscontinuityDropsInProgressPes();
    void ignoresDuplicatePayloadStartPacket();
    void unboundedPesIsCappedAndResyncs();
};

void TestMpegTsParser::rejectsBadSyncByte()
{
    MpegTsParser parser;
    QByteArray pkt(188, char(0xff));
    QList<PesPacket> out;

    QVERIFY(!parser.pushTsPacket(pkt, &out));
}

void TestMpegTsParser::parsesPatPmtPid()
{
    MpegTsParser parser;
    QList<PesPacket> out;

    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000)), &out));
    QCOMPARE(parser.pmtPid(), quint16(0x1000));
}

void TestMpegTsParser::parsesH264VideoPidAndCodec()
{
    MpegTsParser parser;
    QList<PesPacket> out;

    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000)), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x1b, 0x0101}}, 0x0101)), &out));
    QCOMPARE(parser.videoPid(), quint16(0x0101));
    QCOMPARE(parser.videoCodec(), NativeVideoCodec::H264);
}

void TestMpegTsParser::parsesHevcVideoPidAndCodec()
{
    MpegTsParser parser;
    QList<PesPacket> out;

    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000)), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x24, 0x0102}}, 0x0102)), &out));
    QCOMPARE(parser.videoPid(), quint16(0x0102));
    QCOMPARE(parser.videoCodec(), NativeVideoCodec::Hevc);
}

void TestMpegTsParser::parsesAacPid()
{
    MpegTsParser parser;
    QList<PesPacket> out;

    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000)), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x1b, 0x0101}, {0x0f, 0x0103}}, 0x0101)),
                                &out));
    QCOMPARE(parser.audioPid(), quint16(0x0103));
}

void TestMpegTsParser::parsesLatmAacPid()
{
    MpegTsParser parser;
    QList<PesPacket> out;

    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000)), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x1b, 0x0101}, {0x11, 0x0104}}, 0x0101)),
                                &out));
    QCOMPARE(parser.audioPid(), quint16(0x0104));

    const QByteArray payload = QByteArray::fromHex("56e000");
    QVERIFY(parser.pushTsPacket(tsPacket(0x0104, true, pesPacket(0xc0, payload, 90000), 0),
                                &out));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out.first().kind, NativeElementaryStreamKind::AudioAacLatm);
    QCOMPARE(out.first().payload, payload);
}

void TestMpegTsParser::malformedPmtDoesNotPartiallyApply()
{
    MpegTsParser parser;
    QList<PesPacket> out;

    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000)), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         malformedPmtSectionWithPartialVideo(0x0101)), &out));
    QCOMPARE(parser.videoPid(), quint16(0xffff));
    QCOMPARE(parser.videoCodec(), NativeVideoCodec::Unknown);
    QCOMPARE(parser.audioPid(), quint16(0xffff));
}

void TestMpegTsParser::validPmtResetsAbsentStreams()
{
    MpegTsParser parser;
    QList<PesPacket> out;

    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000)), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x1b, 0x0101}, {0x0f, 0x0103}}, 0x0101),
                                         0), &out));
    QCOMPARE(parser.audioPid(), quint16(0x0103));

    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x24, 0x0102}}, 0x0102), 1), &out));
    QCOMPARE(parser.videoPid(), quint16(0x0102));
    QCOMPARE(parser.videoCodec(), NativeVideoCodec::Hevc);
    QCOMPARE(parser.audioPid(), quint16(0xffff));
}

void TestMpegTsParser::reassemblesPesAcrossPacketsAndExtractsPts()
{
    constexpr quint16 videoPid = 0x0101;
    constexpr qint64 pts90k = 123456789;

    MpegTsParser parser;
    QList<PesPacket> out;
    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000)), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x1b, videoPid}, {0x0f, 0x0103}}, videoPid)),
                                &out));

    QByteArray esPayload;
    for (int i = 0; i < 210; ++i) {
        esPayload.append(char(i & 0xff));
    }
    const QByteArray pes = pesPacket(0xe0, esPayload, pts90k);

    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, true, pes.left(184), 0), &out));
    QCOMPARE(out.size(), 0);
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, false, pes.mid(184), 1), &out));

    QCOMPARE(out.size(), 1);
    QCOMPARE(out.first().pid, videoPid);
    QCOMPARE(out.first().kind, NativeElementaryStreamKind::Video);
    QCOMPARE(out.first().videoCodec, NativeVideoCodec::H264);
    QCOMPARE(out.first().pts90k, pts90k);
    QCOMPARE(out.first().dts90k, qint64(-1));
    QCOMPARE(out.first().payload, esPayload);
}

void TestMpegTsParser::dropsPesAfterContinuityGapUntilNextPayloadStart()
{
    constexpr quint16 videoPid = 0x0101;

    MpegTsParser parser;
    QList<PesPacket> out;
    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000), 0), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x1b, videoPid}}, videoPid), 0), &out));

    QByteArray largePayload;
    for (int i = 0; i < 210; ++i) {
        largePayload.append(char(i & 0xff));
    }
    const QByteArray interruptedPes = pesPacket(0xe0, largePayload, 90000);
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, true, interruptedPes.left(184), 0), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, false, interruptedPes.mid(184), 2), &out));
    QCOMPARE(out.size(), 0);

    const QByteArray nextPayload = QByteArray::fromHex("00000001658884");
    const QByteArray nextPes = pesPacket(0xe0, nextPayload, 180000);
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, true, nextPes, 3), &out));

    QCOMPARE(out.size(), 1);
    QCOMPARE(out.first().pts90k, qint64(180000));
    QCOMPARE(out.first().payload, nextPayload);
}

void TestMpegTsParser::rejectsMalformedAdaptationOnlyLength()
{
    MpegTsParser parser;
    QList<PesPacket> out;

    QVERIFY(!parser.pushTsPacket(adaptationOnlyPacket(0x0101, 0, 0x00, 184), &out));
}

void TestMpegTsParser::adaptationOnlyDiscontinuityDropsInProgressPes()
{
    constexpr quint16 videoPid = 0x0101;

    MpegTsParser parser;
    QList<PesPacket> out;
    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000), 0), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x1b, videoPid}}, videoPid), 0), &out));

    QByteArray largePayload;
    for (int i = 0; i < 210; ++i) {
        largePayload.append(char(i & 0xff));
    }
    const QByteArray interruptedPes = pesPacket(0xe0, largePayload, 90000);
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, true, interruptedPes.left(184), 0), &out));
    QVERIFY(parser.pushTsPacket(adaptationOnlyPacket(videoPid, 0, 0x80), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, false, interruptedPes.mid(184), 1), &out));
    QCOMPARE(out.size(), 0);

    const QByteArray nextPayload = QByteArray::fromHex("00000001658884");
    const QByteArray nextPes = pesPacket(0xe0, nextPayload, 180000);
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, true, nextPes, 2), &out));

    QCOMPARE(out.size(), 1);
    QCOMPARE(out.first().pts90k, qint64(180000));
    QCOMPARE(out.first().payload, nextPayload);
}

void TestMpegTsParser::ignoresDuplicatePayloadStartPacket()
{
    constexpr quint16 videoPid = 0x0101;

    MpegTsParser parser;
    QList<PesPacket> out;
    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000), 0), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true,
                                         pmtSection({{0x1b, videoPid}}, videoPid), 0), &out));

    const QByteArray payload = QByteArray::fromHex("00000001658884");
    const QByteArray pes = pesPacket(0xe0, payload, 90000);
    const QByteArray packet = tsPacket(videoPid, true, pes, 0);

    QVERIFY(parser.pushTsPacket(packet, &out));
    QCOMPARE(out.size(), 1);
    QVERIFY(parser.pushTsPacket(packet, &out));
    QCOMPARE(out.size(), 1);
}

// Regression for the unbounded PES-reassembly DoS: a video PES that declares
// PES_packet_length=0 (legal "unbounded", terminated by the next payload-start)
// followed by a flood of continuation packets with no further payload-start must
// NOT grow the reassembly buffer without limit, and must NOT later emit a giant
// access unit built from the accumulated bytes — it caps, drops, and resyncs.
void TestMpegTsParser::unboundedPesIsCappedAndResyncs() {
    constexpr quint16 videoPid = 0x0101;

    MpegTsParser parser;
    parser.setMaxPesAssemblyBytesForTest(4096); // small cap -> overflow is cheap to hit
    QList<PesPacket> out;
    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, patSection(0x1000), 0), &out));
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true, pmtSection({{0x1b, videoPid}}, videoPid), 0),
                                &out));

    // PES start with PES_packet_length = 0 and a minimal header (no PTS).
    QByteArray unboundedStart = QByteArray::fromHex("000001"); // start code
    unboundedStart.append(char(0xe0));                         // video stream id
    unboundedStart.append(char(0x00)).append(char(0x00));      // PES_packet_length = 0
    unboundedStart.append(char(0x80)).append(char(0x00)).append(char(0x00)); // flags/flags2/hdrlen
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, true, unboundedStart, 0), &out));

    // Flood of continuation packets, no payload-start: ~11.7 KiB, well past the
    // 4 KiB cap. Without the cap this would all be retained in one assembly.
    quint8 cc = 1;
    const QByteArray filler(184, char(0xAA));
    for (int i = 0; i < 64; ++i) {
        QVERIFY(parser.pushTsPacket(tsPacket(videoPid, false, filler, cc), &out));
        cc = quint8((cc + 1) & 0x0f);
    }
    QCOMPARE(out.size(), 0); // length-0 PES isn't flushed until a payload-start

    // The next payload-start yields ONLY the new, small PES — never a giant one
    // assembled from the overflow (which is what an uncapped parser would emit).
    const QByteArray nextPayload = QByteArray::fromHex("00000001658884");
    const QByteArray nextPes = pesPacket(0xe0, nextPayload, 180000);
    QVERIFY(parser.pushTsPacket(tsPacket(videoPid, true, nextPes, cc), &out));

    QCOMPARE(out.size(), 1);
    QCOMPARE(out.first().payload, nextPayload);
}

QTEST_GUILESS_MAIN(TestMpegTsParser)
#include "tst_mpegtsparser.moc"

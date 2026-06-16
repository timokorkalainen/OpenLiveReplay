#include <QtTest>

#include "recorder_engine/ingest/mpegtsparser.h"

// Build an adaptation-only TS packet on `pid` carrying a 48-bit PCR whose 90 kHz
// base is `pcrBase90k` (extension 0). Layout: af_length, flags(0x10=PCR_flag),
// then 6 PCR bytes (33-bit base | 6 reserved | 9-bit ext).
static QByteArray pcrPacket(quint16 pid, quint8 cc, qint64 pcrBase90k)
{
    QByteArray pkt(188, char(0xff));
    pkt[0] = char(0x47);
    pkt[1] = char((pid >> 8) & 0x1f);
    pkt[2] = char(pid & 0xff);
    pkt[3] = char(0x20 | (cc & 0x0f)); // adaptation only
    pkt[4] = char(183);                // adaptation_field_length
    pkt[5] = char(0x10);               // PCR_flag
    const quint64 b = quint64(pcrBase90k) & 0x1ffffffffULL;
    pkt[6] = char((b >> 25) & 0xff);
    pkt[7] = char((b >> 17) & 0xff);
    pkt[8] = char((b >> 9) & 0xff);
    pkt[9] = char((b >> 1) & 0xff);
    pkt[10] = char(((b & 0x1) << 7) | 0x7e); // last base bit + 6 reserved (1s)
    pkt[11] = char(0x00);                    // extension low byte (=0)
    return pkt;
}

static QByteArray patSection(quint16 pmtPid)
{
    QByteArray pat;
    pat.append(char(0x00));
    pat.append(QByteArray::fromHex("00b00d0001c100000001"));
    pat.append(char(0xe0 | ((pmtPid >> 8) & 0x1f)));
    pat.append(char(pmtPid & 0xff));
    pat.append(QByteArray(4, char(0x00)));
    return pat;
}

static QByteArray pmtSection(quint16 pcrPid, quint16 videoPid)
{
    QByteArray body;
    body.append(QByteArray::fromHex("0001c10000"));
    body.append(char(0xe0 | ((pcrPid >> 8) & 0x1f)));
    body.append(char(pcrPid & 0xff));
    body.append(QByteArray::fromHex("f000"));
    body.append(char(0x1b)); // H.264
    body.append(char(0xe0 | ((videoPid >> 8) & 0x1f)));
    body.append(char(videoPid & 0xff));
    body.append(QByteArray::fromHex("f000"));
    body.append(QByteArray(4, char(0x00)));
    QByteArray pmt;
    pmt.append(char(0x00));
    pmt.append(char(0x02));
    pmt.append(char(0xb0 | ((body.size() >> 8) & 0x0f)));
    pmt.append(char(body.size() & 0xff));
    pmt.append(body);
    return pmt;
}

static QByteArray tsSection(quint16 pid, const QByteArray& section)
{
    QByteArray pkt(188, char(0xff));
    pkt[0] = char(0x47);
    pkt[1] = char(0x40 | ((pid >> 8) & 0x1f)); // payload_unit_start
    pkt[2] = char(pid & 0xff);
    pkt[3] = char(0x10);
    memcpy(pkt.data() + 4, section.constData(), size_t(qMin(184, section.size())));
    return pkt;
}

class TestMpegTsParserPcr : public QObject {
    Q_OBJECT
private slots:
    void extractsPcrFromPcrPid();
    void noPcrLeavesInfoUnset();
    void pcrFlagOnNonPcrPidIgnored();
    void adaptationWithoutPcrFlagLeavesUnset();
    void discontinuitySurfacesOnPcrPid();
};

// Adaptation-only TS packet on `pid` with an explicit adaptation-flags byte and no
// PCR payload (used to exercise the discontinuity_indicator and the PCR_flag guard).
static QByteArray afFlagsPacket(quint16 pid, quint8 cc, quint8 flags) {
    QByteArray pkt(188, char(0xff));
    pkt[0] = char(0x47);
    pkt[1] = char((pid >> 8) & 0x1f);
    pkt[2] = char(pid & 0xff);
    pkt[3] = char(0x20 | (cc & 0x0f));
    pkt[4] = char(183);
    pkt[5] = char(flags);
    return pkt;
}

void TestMpegTsParserPcr::extractsPcrFromPcrPid() {
    MpegTsParser parser;
    QList<PesPacket> pes;
    const quint16 pmtPid = 0x1000, pcrPid = 0x0100, videoPid = 0x0100;
    parser.pushTsPacket(tsSection(0x0000, patSection(pmtPid)), &pes);
    parser.pushTsPacket(tsSection(pmtPid, pmtSection(pcrPid, videoPid)), &pes);

    MpegTsParser::TsPacketInfo info;
    parser.pushTsPacket(pcrPacket(pcrPid, 0, 123456789), &pes, &info);
    QCOMPARE(info.pcr90k, qint64(123456789));
}

void TestMpegTsParserPcr::noPcrLeavesInfoUnset() {
    MpegTsParser parser;
    QList<PesPacket> pes;
    parser.pushTsPacket(tsSection(0x0000, patSection(0x1000)), &pes);
    MpegTsParser::TsPacketInfo info;
    // A PAT packet (no adaptation PCR) must not report a PCR.
    parser.pushTsPacket(tsSection(0x0000, patSection(0x1000)), &pes, &info);
    QCOMPARE(info.pcr90k, qint64(-1));
}

void TestMpegTsParserPcr::pcrFlagOnNonPcrPidIgnored() {
    MpegTsParser parser;
    QList<PesPacket> pes;
    const quint16 pmtPid = 0x1000, pcrPid = 0x0100, videoPid = 0x0100;
    parser.pushTsPacket(tsSection(0x0000, patSection(pmtPid)), &pes);
    parser.pushTsPacket(tsSection(pmtPid, pmtSection(pcrPid, videoPid)), &pes);

    // A PCR carried on some OTHER pid must not be reported as the program PCR.
    MpegTsParser::TsPacketInfo info;
    parser.pushTsPacket(pcrPacket(0x0200, 0, 555555), &pes, &info);
    QCOMPARE(info.pcr90k, qint64(-1));
}

void TestMpegTsParserPcr::adaptationWithoutPcrFlagLeavesUnset() {
    MpegTsParser parser;
    QList<PesPacket> pes;
    const quint16 pmtPid = 0x1000, pcrPid = 0x0100, videoPid = 0x0100;
    parser.pushTsPacket(tsSection(0x0000, patSection(pmtPid)), &pes);
    parser.pushTsPacket(tsSection(pmtPid, pmtSection(pcrPid, videoPid)), &pes);

    // Adaptation field on the PCR pid but PCR_flag (0x10) clear -> no PCR.
    MpegTsParser::TsPacketInfo info;
    parser.pushTsPacket(afFlagsPacket(pcrPid, 0, 0x00), &pes, &info);
    QCOMPARE(info.pcr90k, qint64(-1));
}

void TestMpegTsParserPcr::discontinuitySurfacesOnPcrPid() {
    MpegTsParser parser;
    QList<PesPacket> pes;
    const quint16 pmtPid = 0x1000, pcrPid = 0x0100, videoPid = 0x0100;
    parser.pushTsPacket(tsSection(0x0000, patSection(pmtPid)), &pes);
    parser.pushTsPacket(tsSection(pmtPid, pmtSection(pcrPid, videoPid)), &pes);

    // discontinuity_indicator (0x80) on the PCR pid surfaces as info.discontinuity.
    MpegTsParser::TsPacketInfo info;
    parser.pushTsPacket(afFlagsPacket(pcrPid, 0, 0x80), &pes, &info);
    QVERIFY(info.discontinuity);
}

QTEST_GUILESS_MAIN(TestMpegTsParserPcr)
#include "tst_mpegtsparser_pcr.moc"

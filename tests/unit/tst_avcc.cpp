// Unit tests for buildAvcCFromParameterSets — the AVCDecoderConfigurationRecord
// (avcC) byte layout used as Matroska CodecPrivate for H.264.
#include <QtTest>

#include "recorder_engine/codec/avcc.h"

class TestAvcc : public QObject {
    Q_OBJECT
private slots:
    void emptyInputsYieldEmpty();
    void buildsWellFormedRecord();
    void multipleParameterSets();
    void rejectsNalPayloadsTooLargeForAvccLengthField();
    void rejectsParameterSetCountsTooLargeForAvccFields();
    void parseAvccRoundTrips();
};

void TestAvcc::emptyInputsYieldEmpty() {
    QByteArray sps(QByteArrayLiteral("\x67\x42\x00\x1f"));
    QByteArray pps(QByteArrayLiteral("\x68\xce\x3c\x80"));
    QVERIFY(buildAvcCFromParameterSets({}, {pps}).isEmpty());
    QVERIFY(buildAvcCFromParameterSets({sps}, {}).isEmpty());
    QVERIFY(buildAvcCFromParameterSets({QByteArrayLiteral("\x67\x42")}, {pps}).isEmpty()); // SPS < 4 bytes
}

void TestAvcc::buildsWellFormedRecord() {
    // Minimal SPS/PPS NAL payloads (profile 0x42 baseline, compat 0x00, level 0x1f).
    const QByteArray sps = QByteArrayLiteral("\x67\x42\x00\x1f\x8c\x8d\x40");
    const QByteArray pps = QByteArrayLiteral("\x68\xce\x3c\x80");
    const QByteArray avcc = buildAvcCFromParameterSets({sps}, {pps});

    QVERIFY(!avcc.isEmpty());
    QCOMPARE(quint8(avcc[0]), quint8(0x01));         // configurationVersion
    QCOMPARE(quint8(avcc[1]), quint8(0x42));         // AVCProfileIndication = SPS[1]
    QCOMPARE(quint8(avcc[2]), quint8(0x00));         // profile_compatibility = SPS[2]
    QCOMPARE(quint8(avcc[3]), quint8(0x1f));         // AVCLevelIndication = SPS[3]
    QCOMPARE(quint8(avcc[4]), quint8(0xff));         // 6 reserved bits | lengthSizeMinusOne (3)
    QCOMPARE(quint8(avcc[5]), quint8(0xe1));         // 3 reserved bits | numOfSPS (1)
    const int spsLen = (quint8(avcc[6]) << 8) | quint8(avcc[7]);
    QCOMPARE(spsLen, sps.size());
    QCOMPARE(avcc.mid(8, sps.size()), sps);
    const int ppsCountIdx = 8 + sps.size();
    QCOMPARE(quint8(avcc[ppsCountIdx]), quint8(0x01)); // numOfPPS
    const int ppsLen = (quint8(avcc[ppsCountIdx + 1]) << 8) | quint8(avcc[ppsCountIdx + 2]);
    QCOMPARE(ppsLen, pps.size());
    QCOMPARE(avcc.mid(ppsCountIdx + 3, pps.size()), pps);
}

void TestAvcc::multipleParameterSets() {
    // Two distinct SPS NALs (profile 0x42 baseline, compat 0x00, level 0x1f).
    const QByteArray sps1 = QByteArrayLiteral("\x67\x42\x00\x1f\x8c\x8d\x40");
    const QByteArray sps2 = QByteArrayLiteral("\x67\x42\x00\x1f\x9a\x9b\x50");
    // Two distinct PPS NALs.
    const QByteArray pps1 = QByteArrayLiteral("\x68\xce\x3c\x80");
    const QByteArray pps2 = QByteArrayLiteral("\x68\xda\x3d\x90");

    const QByteArray avcc = buildAvcCFromParameterSets({sps1, sps2}, {pps1, pps2});

    QVERIFY(!avcc.isEmpty());
    QCOMPARE(quint8(avcc[0]), quint8(0x01));         // configurationVersion
    QCOMPARE(quint8(avcc[1]), quint8(0x42));         // AVCProfileIndication = SPS[1]
    QCOMPARE(quint8(avcc[2]), quint8(0x00));         // profile_compatibility = SPS[2]
    QCOMPARE(quint8(avcc[3]), quint8(0x1f));         // AVCLevelIndication = SPS[3]
    QCOMPARE(quint8(avcc[4]), quint8(0xff));         // 6 reserved bits | lengthSizeMinusOne (3)
    // Verify numSPS = 2 in low 5 bits of byte[5]
    QCOMPARE(quint8(avcc[5]) & 0x1f, quint8(2));

    // Walk the buffer to verify SPS entries.
    int offset = 6;
    // First SPS with 2-byte big-endian length prefix
    int sps1Len = (quint8(avcc[offset]) << 8) | quint8(avcc[offset + 1]);
    QCOMPARE(sps1Len, sps1.size());
    QCOMPARE(avcc.mid(offset + 2, sps1.size()), sps1);
    offset += 2 + sps1.size();

    // Second SPS with 2-byte big-endian length prefix
    int sps2Len = (quint8(avcc[offset]) << 8) | quint8(avcc[offset + 1]);
    QCOMPARE(sps2Len, sps2.size());
    QCOMPARE(avcc.mid(offset + 2, sps2.size()), sps2);
    offset += 2 + sps2.size();

    // Verify numPPS = 2
    QCOMPARE(quint8(avcc[offset]), quint8(2));
    offset += 1;

    // First PPS with 2-byte big-endian length prefix
    int pps1Len = (quint8(avcc[offset]) << 8) | quint8(avcc[offset + 1]);
    QCOMPARE(pps1Len, pps1.size());
    QCOMPARE(avcc.mid(offset + 2, pps1.size()), pps1);
    offset += 2 + pps1.size();

    // Second PPS with 2-byte big-endian length prefix
    int pps2Len = (quint8(avcc[offset]) << 8) | quint8(avcc[offset + 1]);
    QCOMPARE(pps2Len, pps2.size());
    QCOMPARE(avcc.mid(offset + 2, pps2.size()), pps2);
}

void TestAvcc::rejectsNalPayloadsTooLargeForAvccLengthField() {
    QByteArray oversizedSps(0x10000, char(0x67));
    oversizedSps[1] = char(0x42);
    oversizedSps[2] = char(0x00);
    oversizedSps[3] = char(0x1f);
    const QByteArray sps = QByteArrayLiteral("\x67\x42\x00\x1f\x8c\x8d\x40");
    const QByteArray pps = QByteArrayLiteral("\x68\xce\x3c\x80");
    const QByteArray oversizedPps(0x10000, char(0x68));

    QVERIFY(buildAvcCFromParameterSets({oversizedSps}, {pps}).isEmpty());
    QVERIFY(buildAvcCFromParameterSets({sps}, {oversizedPps}).isEmpty());
}

void TestAvcc::rejectsParameterSetCountsTooLargeForAvccFields() {
    const QByteArray sps = QByteArrayLiteral("\x67\x42\x00\x1f\x8c\x8d\x40");
    const QByteArray pps = QByteArrayLiteral("\x68\xce\x3c\x80");

    QVERIFY(buildAvcCFromParameterSets(QList<QByteArray>(32, sps), {pps}).isEmpty());
    QVERIFY(buildAvcCFromParameterSets({sps}, QList<QByteArray>(256, pps)).isEmpty());
}

void TestAvcc::parseAvccRoundTrips() {
    // Build a well-formed avcC and then parse it back — verify round-trip fidelity.
    const QByteArray sps = QByteArrayLiteral("\x67\x42\x00\x1f\x8c\x8d\x40");
    const QByteArray pps = QByteArrayLiteral("\x68\xce\x3c\x80");
    const QByteArray avcc = buildAvcCFromParameterSets({sps}, {pps});
    QVERIFY(!avcc.isEmpty());

    QList<QByteArray> parsedSps, parsedPps;
    QVERIFY(parseAvcc(avcc, &parsedSps, &parsedPps));
    QCOMPARE(parsedSps.size(), 1);
    QCOMPARE(parsedPps.size(), 1);
    QCOMPARE(parsedSps.at(0), sps);
    QCOMPARE(parsedPps.at(0), pps);

    // Truncated data must return false.
    QVERIFY(!parseAvcc(avcc.left(4), &parsedSps, &parsedPps));
    // Empty must return false.
    QVERIFY(!parseAvcc({}, &parsedSps, &parsedPps));
}

QTEST_GUILESS_MAIN(TestAvcc)
#include "tst_avcc.moc"

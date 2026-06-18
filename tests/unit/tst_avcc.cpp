// Unit tests for buildAvcCFromParameterSets — the AVCDecoderConfigurationRecord
// (avcC) byte layout used as Matroska CodecPrivate for H.264.
#include <QtTest>

#include "recorder_engine/codec/avcc.h"

class TestAvcc : public QObject {
    Q_OBJECT
private slots:
    void emptyInputsYieldEmpty();
    void buildsWellFormedRecord();
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

QTEST_GUILESS_MAIN(TestAvcc)
#include "tst_avcc.moc"

#include <QtTest>

#include "recorder_engine/ingest/h26xaccessunit.h"

class TestH26xAccessUnit : public QObject {
    Q_OBJECT

private slots:
    void collectsH264ParameterSets();
    void collectsHevcParameterSets();
    void supportsThreeByteStartCodes();
    void ignoresDuplicateParameterSets();
    void propagatesTimestamps();
    void splitsMultipleAccessUnits();
    void ignoresUnknownCodecAndNoStartCodePayloads();
    void keepsH264NonFirstSliceInSameAccessUnit();
    void appendsChangedParameterSetValues();
};

void TestH26xAccessUnit::collectsH264ParameterSets() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::H264);
    const QByteArray sps = QByteArray::fromHex("6764001facd940780227e5c05a808080a0");
    const QByteArray pps = QByteArray::fromHex("68ebe3cb22c0");
    const QByteArray idr = QByteArray::fromHex("658884");
    const QByteArray payload = QByteArray::fromHex("00000001") + sps
        + QByteArray::fromHex("00000001") + pps
        + QByteArray::fromHex("00000001") + idr;

    const QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 90000, 81000);

    QCOMPARE(units.size(), 1);
    QCOMPARE(units.first().codec, NativeVideoCodec::H264);
    QCOMPARE(units.first().parameterSets.h264Sps, QList<QByteArray>{sps});
    QCOMPARE(units.first().parameterSets.h264Pps, QList<QByteArray>{pps});
    QCOMPARE(units.first().annexB, payload);
}

void TestH26xAccessUnit::collectsHevcParameterSets() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::Hevc);
    const QByteArray vps = QByteArray::fromHex("40010c01");
    const QByteArray sps = QByteArray::fromHex("4201010160");
    const QByteArray pps = QByteArray::fromHex("4401c172b46240");
    const QByteArray trail = QByteArray::fromHex("2601af09");
    const QByteArray payload = QByteArray::fromHex("00000001") + vps
        + QByteArray::fromHex("00000001") + sps
        + QByteArray::fromHex("00000001") + pps
        + QByteArray::fromHex("00000001") + trail;

    const QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 90000, 81000);

    QCOMPARE(units.size(), 1);
    QCOMPARE(units.first().codec, NativeVideoCodec::Hevc);
    QCOMPARE(units.first().parameterSets.hevcVps, QList<QByteArray>{vps});
    QCOMPARE(units.first().parameterSets.hevcSps, QList<QByteArray>{sps});
    QCOMPARE(units.first().parameterSets.hevcPps, QList<QByteArray>{pps});
    QCOMPARE(units.first().annexB, payload);
}

void TestH26xAccessUnit::supportsThreeByteStartCodes() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::H264);
    const QByteArray sps = QByteArray::fromHex("6742c01e");
    const QByteArray pps = QByteArray::fromHex("68ce3c80");
    const QByteArray idr = QByteArray::fromHex("658899");
    const QByteArray payload = QByteArray::fromHex("000001") + sps
        + QByteArray::fromHex("000001") + pps
        + QByteArray::fromHex("000001") + idr;

    const QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 123, 100);

    QCOMPARE(units.size(), 1);
    QCOMPARE(units.first().parameterSets.h264Sps, QList<QByteArray>{sps});
    QCOMPARE(units.first().parameterSets.h264Pps, QList<QByteArray>{pps});
}

void TestH26xAccessUnit::ignoresDuplicateParameterSets() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::Hevc);
    const QByteArray vps = QByteArray::fromHex("40010c01");
    const QByteArray sps = QByteArray::fromHex("4201010160");
    const QByteArray pps = QByteArray::fromHex("4401c172b46240");
    const QByteArray payload = QByteArray::fromHex("00000001") + vps
        + QByteArray::fromHex("00000001") + sps
        + QByteArray::fromHex("00000001") + pps
        + QByteArray::fromHex("00000001") + vps
        + QByteArray::fromHex("00000001") + sps
        + QByteArray::fromHex("00000001") + pps
        + QByteArray::fromHex("00000001") + QByteArray::fromHex("2601af09");

    splitter.pushPesPayload(payload, 90, 80);
    splitter.pushPesPayload(payload, 180, 170);

    QCOMPARE(splitter.parameterSets().hevcVps, QList<QByteArray>{vps});
    QCOMPARE(splitter.parameterSets().hevcSps, QList<QByteArray>{sps});
    QCOMPARE(splitter.parameterSets().hevcPps, QList<QByteArray>{pps});
}

void TestH26xAccessUnit::propagatesTimestamps() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::H264);
    const QByteArray payload = QByteArray::fromHex("00000001678800000001650102");

    const QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 123456, 123000);

    QCOMPARE(units.size(), 1);
    QCOMPARE(units.first().pts90k, 123456);
    QCOMPARE(units.first().dts90k, 123000);
}

void TestH26xAccessUnit::splitsMultipleAccessUnits() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::H264);
    const QByteArray firstAccessUnit = QByteArray::fromHex("000000016742c01e0000000168ce3c8000000001658884");
    const QByteArray secondAccessUnit = QByteArray::fromHex("00000001091000000001418884");
    const QByteArray payload = firstAccessUnit + secondAccessUnit;

    const QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 45000, 45000);

    QCOMPARE(units.size(), 2);
    QCOMPARE(units.at(0).annexB, firstAccessUnit);
    QCOMPARE(units.at(1).annexB, secondAccessUnit);
    QCOMPARE(units.at(1).parameterSets.h264Sps, QList<QByteArray>{QByteArray::fromHex("6742c01e")});
    QCOMPARE(units.at(1).parameterSets.h264Pps, QList<QByteArray>{QByteArray::fromHex("68ce3c80")});
}

void TestH26xAccessUnit::ignoresUnknownCodecAndNoStartCodePayloads() {
    H26xAccessUnitSplitter unknownSplitter(NativeVideoCodec::Unknown);
    QVERIFY(unknownSplitter.pushPesPayload(QByteArray::fromHex("00000001658884"), 1, 1).isEmpty());

    H26xAccessUnitSplitter h264Splitter(NativeVideoCodec::H264);
    QVERIFY(h264Splitter.pushPesPayload(QByteArray::fromHex("658884"), 1, 1).isEmpty());
}

void TestH26xAccessUnit::keepsH264NonFirstSliceInSameAccessUnit() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::H264);
    const QByteArray firstSlice = QByteArray::fromHex("00000001658884");
    const QByteArray secondSlice = QByteArray::fromHex("00000001414084");
    const QByteArray payload = firstSlice + secondSlice;

    const QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 10, 9);

    QCOMPARE(units.size(), 1);
    QCOMPARE(units.first().annexB, payload);
}

void TestH26xAccessUnit::appendsChangedParameterSetValues() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::H264);
    const QByteArray sps1 = QByteArray::fromHex("6742c01e");
    const QByteArray sps2 = QByteArray::fromHex("6742c01f");
    const QByteArray pps = QByteArray::fromHex("68ce3c80");
    const QByteArray payload = QByteArray::fromHex("00000001") + sps1
        + QByteArray::fromHex("00000001") + pps
        + QByteArray::fromHex("00000001") + sps2
        + QByteArray::fromHex("00000001") + QByteArray::fromHex("658899");

    splitter.pushPesPayload(payload, 10, 9);

    QCOMPARE(splitter.parameterSets().h264Sps, (QList<QByteArray>{sps1, sps2}));
    QCOMPARE(splitter.parameterSets().h264Pps, QList<QByteArray>{pps});
}

QTEST_GUILESS_MAIN(TestH26xAccessUnit)
#include "tst_h26xaccessunit.moc"

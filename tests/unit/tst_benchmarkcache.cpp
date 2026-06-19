// Unit tests for benchmark result caching: round-trip + device/resolution invalidation.
#include <QtTest>
#include <QTemporaryDir>

#include "recorder_engine/benchmark/benchmarkcache.h"

class TestBenchmarkCache : public QObject {
    Q_OBJECT
private slots:
    void roundTrip();
    void invalidatesOnDeviceOrResolutionChange();
    void deviceLabelIsNonEmpty();
};

void TestBenchmarkCache::roundTrip() {
    QTemporaryDir dir; const QString path = dir.filePath("bench.json");
    CodecBenchmarkResult in;
    in.h264Available = true; in.h264SafeFeeds = 12; in.mpeg2SafeFeeds = 5;
    in.h264EncodeMs = 1.8; in.recommended = VideoCodecChoice::H264Hardware;
    in.deviceLabel = "TestChip arm64"; in.resolution = "1920x1080@30"; in.timestamp = "2026-06-19T00:00:00Z";
    QVERIFY(saveBenchmarkResult(path, in));
    CodecBenchmarkResult out;
    QVERIFY(loadBenchmarkResult(path, out));
    QCOMPARE(out.h264SafeFeeds, 12);
    QCOMPARE(out.mpeg2SafeFeeds, 5);
    QCOMPARE(out.recommended, VideoCodecChoice::H264Hardware);
    QCOMPARE(out.deviceLabel, in.deviceLabel);
    QCOMPARE(out.resolution, in.resolution);
}

void TestBenchmarkCache::invalidatesOnDeviceOrResolutionChange() {
    CodecBenchmarkResult c; c.deviceLabel = "ChipA arm64"; c.resolution = "1920x1080@30";
    QVERIFY(benchmarkResultMatches(c, "ChipA arm64", "1920x1080@30"));
    QVERIFY(!benchmarkResultMatches(c, "ChipB arm64", "1920x1080@30"));
    QVERIFY(!benchmarkResultMatches(c, "ChipA arm64", "1280x720@30"));
}

void TestBenchmarkCache::deviceLabelIsNonEmpty() {
    QVERIFY(!benchmarkDeviceLabel().isEmpty());
}

QTEST_GUILESS_MAIN(TestBenchmarkCache)
#include "tst_benchmarkcache.moc"

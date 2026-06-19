// Unit tests for benchmark result caching: round-trip + device/resolution invalidation
// + robustness against malformed input.
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>

#include "recorder_engine/benchmark/benchmarkcache.h"

class TestBenchmarkCache : public QObject {
    Q_OBJECT
private slots:
    void roundTrip();
    void invalidatesOnDeviceOrResolutionChange();
    void deviceLabelIsNonEmpty();
    void loadMalformedJsonReturnsFalse();        // T-cache-robustness
    void loadMissingKeysYieldsDocumentedDefaults(); // T-cache-robustness
};

void TestBenchmarkCache::roundTrip() {
    QTemporaryDir dir; const QString path = dir.filePath("bench.json");
    CodecBenchmarkResult in;
    in.h264Available = true; in.h264SafeFeeds = 12; in.mpeg2SafeFeeds = 5;
    in.h264EncodeMs = 1.8; in.h264DecodeMs = 2.3; in.mpeg2EncodeMs = 3.5; in.mpeg2DecodeMs = 4.1;
    in.recommended = VideoCodecChoice::H264Hardware;
    in.deviceLabel = "TestChip arm64"; in.resolution = "1920x1080@30"; in.timestamp = "2026-06-19T00:00:00Z";
    in.ceilingReached = true;
    QVERIFY(saveBenchmarkResult(path, in));
    CodecBenchmarkResult out;
    QVERIFY(loadBenchmarkResult(path, out));
    QCOMPARE(out.h264Available, true);
    QCOMPARE(out.h264SafeFeeds, 12);
    QCOMPARE(out.mpeg2SafeFeeds, 5);
    QCOMPARE(out.h264EncodeMs, 1.8);
    QCOMPARE(out.h264DecodeMs, 2.3);
    QCOMPARE(out.mpeg2EncodeMs, 3.5);
    QCOMPARE(out.mpeg2DecodeMs, 4.1);
    QCOMPARE(out.recommended, VideoCodecChoice::H264Hardware);
    QCOMPARE(out.deviceLabel, in.deviceLabel);
    QCOMPARE(out.resolution, in.resolution);
    QCOMPARE(out.timestamp, in.timestamp);
    QCOMPARE(out.ceilingReached, true);
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

// T-cache-robustness: loadBenchmarkResult on a non-JSON file must return false.
void TestBenchmarkCache::loadMalformedJsonReturnsFalse() {
    QTemporaryDir dir;
    const QString path = dir.filePath("bad.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("this is not json {{{");
    f.close();

    CodecBenchmarkResult out;
    // Must return false, not crash
    QVERIFY(!loadBenchmarkResult(path, out));
}

// T-cache-robustness: loadBenchmarkResult on a valid JSON object missing benchmark keys
// must not crash and must yield the documented defaults.
// Documented defaults: h264SafeFeeds == -1 (toInt(-1)), mpeg2SafeFeeds == -1,
// h264Available == false (toBool()), ceilingReached == false.
void TestBenchmarkCache::loadMissingKeysYieldsDocumentedDefaults() {
    QTemporaryDir dir;
    const QString path = dir.filePath("empty.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("{}"); // valid JSON object, but none of the benchmark keys present
    f.close();

    CodecBenchmarkResult out;
    // Must succeed (valid JSON) without crashing
    QVERIFY(loadBenchmarkResult(path, out));
    // h264SafeFeeds and mpeg2SafeFeeds default to -1 per toInt(-1) fallback
    QCOMPARE(out.h264SafeFeeds, -1);
    QCOMPARE(out.mpeg2SafeFeeds, -1);
    // h264Available and ceilingReached default to false per toBool() fallback
    QCOMPARE(out.h264Available, false);
    QCOMPARE(out.ceilingReached, false);
}

QTEST_GUILESS_MAIN(TestBenchmarkCache)
#include "tst_benchmarkcache.moc"

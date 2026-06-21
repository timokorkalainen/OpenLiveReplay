#include <QtTest>

#include "playback/output/gpureadbacktelemetry.h"

class TestGpuReadbackTelemetry : public QObject {
    Q_OBJECT
private slots:
    void init() { GpuReadbackTelemetry::instance().reset(); }

    void freshSnapshotIsZero();
    void oneReadbackPerSurfaceIsNotRedundant();
    void twoReadbacksForSameSurfaceAreRedundant();
    void distinctFormatsAreDistinctSurfaces();
};

void TestGpuReadbackTelemetry::freshSnapshotIsZero() {
    const auto snapshot = GpuReadbackTelemetry::instance().snapshot();
    QCOMPARE(snapshot.gpuReadbacks, qint64(0));
    QCOMPARE(snapshot.uniqueSurfaces, qint64(0));
    QCOMPARE(snapshot.redundantReadbacks, qint64(0));
}

void TestGpuReadbackTelemetry::oneReadbackPerSurfaceIsNotRedundant() {
    auto& telemetry = GpuReadbackTelemetry::instance();
    const GpuReadbackSurfaceKey key{0u, 7, FramePixelFormat::Yuv420p};
    telemetry.recordSurface(key);
    telemetry.recordGpuReadback(key);

    const auto snapshot = telemetry.snapshot();
    QCOMPARE(snapshot.gpuReadbacks, qint64(1));
    QCOMPARE(snapshot.uniqueSurfaces, qint64(1));
    QCOMPARE(snapshot.redundantReadbacks, qint64(0));
}

void TestGpuReadbackTelemetry::twoReadbacksForSameSurfaceAreRedundant() {
    auto& telemetry = GpuReadbackTelemetry::instance();
    const GpuReadbackSurfaceKey key{0u, 7, FramePixelFormat::Yuv420p};
    telemetry.recordSurface(key);
    telemetry.recordGpuReadback(key);
    telemetry.recordGpuReadback(key);

    const auto snapshot = telemetry.snapshot();
    QCOMPARE(snapshot.gpuReadbacks, qint64(2));
    QCOMPARE(snapshot.uniqueSurfaces, qint64(1));
    QCOMPARE(snapshot.redundantReadbacks, qint64(1));
}

void TestGpuReadbackTelemetry::distinctFormatsAreDistinctSurfaces() {
    auto& telemetry = GpuReadbackTelemetry::instance();
    const GpuReadbackSurfaceKey i420{0u, 7, FramePixelFormat::Yuv420p};
    const GpuReadbackSurfaceKey nv12{0u, 7, FramePixelFormat::Nv12};
    telemetry.recordSurface(i420);
    telemetry.recordSurface(nv12);
    telemetry.recordGpuReadback(i420);
    telemetry.recordGpuReadback(nv12);

    const auto snapshot = telemetry.snapshot();
    QCOMPARE(snapshot.gpuReadbacks, qint64(2));
    QCOMPARE(snapshot.uniqueSurfaces, qint64(2));
    QCOMPARE(snapshot.redundantReadbacks, qint64(0));
}

QTEST_GUILESS_MAIN(TestGpuReadbackTelemetry)
#include "tst_gpureadbacktelemetry.moc"

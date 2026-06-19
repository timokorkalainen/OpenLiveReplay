// Integration test: real MPEG-2 (FFmpeg) and H.264 (native) runners produce
// plausible measurements at small scale. H.264 skips where no hardware encoder.
#include <QtTest>
#include <atomic>

#include "recorder_engine/benchmark/realcodecrunners.h"
#include "recorder_engine/benchmark/codecbenchmark.h"

extern "C" {
#include <libavutil/frame.h>
}

class TestRealCodecBenchmark : public QObject {
    Q_OBJECT
private slots:
    void syntheticFrameIsDeterministic();
    void mpeg2RunnerMeasuresOneStep();
    void h264RunnerRampsWhenAvailable();
};

void TestRealCodecBenchmark::syntheticFrameIsDeterministic() {
    AVFrame* a = makeSyntheticFrame(320, 240, 5);
    AVFrame* b = makeSyntheticFrame(320, 240, 5);
    QVERIFY(a && b);
    QCOMPARE(memcmp(a->data[0], b->data[0], a->linesize[0] * 240), 0); // same seq → identical
    AVFrame* c = makeSyntheticFrame(320, 240, 6);
    QVERIFY(memcmp(a->data[0], c->data[0], a->linesize[0] * 240) != 0); // motion → differs
    av_frame_free(&a); av_frame_free(&b); av_frame_free(&c);
}

void TestRealCodecBenchmark::mpeg2RunnerMeasuresOneStep() {
    Mpeg2CodecRunner runner;
    QVERIFY(runner.available());
    BenchmarkConfig cfg; cfg.width = 320; cfg.height = 240; cfg.fps = 30; cfg.durationMsPerStep = 500;
    RampStepResult r = runner.runStep(1, cfg);
    QCOMPARE(r.concurrency, 1);
    QVERIFY(r.framesProcessed > 0);
    QVERIFY(r.framesRequired > 0);
}

void TestRealCodecBenchmark::h264RunnerRampsWhenAvailable() {
    H264CodecRunner runner;
    if (!runner.available()) QSKIP("no hardware H.264 on this platform");
    BenchmarkConfig cfg; cfg.width = 640; cfg.height = 480; cfg.fps = 30; cfg.durationMsPerStep = 500;
    std::atomic<bool> cancel{false};
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    QVERIFY(res.ceiling >= 1); // at least 1 feed sustains on any HW-capable device
}

QTEST_GUILESS_MAIN(TestRealCodecBenchmark)
#include "tst_realcodecbenchmark.moc"

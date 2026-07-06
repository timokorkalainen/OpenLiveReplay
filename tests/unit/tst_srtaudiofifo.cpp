// Regression test for the native SRT audio-FIFO placement (crackle fix).
//
// Real SRT audio arrives with a regular PTS (one AAC frame every ~21 ms), but the
// shared drift-corrected clock's toSessionMs() jitters by several milliseconds per
// frame: it re-projects the whole sender-delta-since-anchor through a rate estimate
// that is fit against jittery packet-arrival times, so the estimate (and thus the
// mapped timestamp) wobbles. Driving the sample-contiguous audio FIFO straight from
// that per-frame jitter fragments it into gap-fills and overlap-drops — audible
// clicks and dropouts in both the live monitor and the NDI output.
//
// advanceAudioFifoSample() must absorb that jitter: anchor once, then advance by the
// decoded sample count so consecutive frames stay sample-contiguous, re-anchoring
// only on a real discontinuity (a divergence beyond the resync window).

#include <QtTest>

#include <iterator>

#include "recorder_engine/ingest/nativesrtingestsession.h"

class TestSrtAudioFifo : public QObject {
    Q_OBJECT
private slots:
    void clockJitterStaysSampleContiguous();
    void realDiscontinuityReanchors();
    void clockJitterStaysFrameContiguous();
    void realVideoDiscontinuityReanchors();
};

// A regular audio stream (1024-sample frames) whose clock-mapped start carries
// millisecond jitter (±up to ~27 ms here) must still land perfectly contiguous.
void TestSrtAudioFifo::clockJitterStaysSampleContiguous() {
    constexpr int kDecoded = 1024;    // 48 kHz output samples per AAC frame
    constexpr int64_t kResync = 9600; // 200 ms @ 48 kHz
    // Per-frame clock jitter (samples) observed from the noisy drift slope.
    const int64_t jitter[] = {0, 600, -700, 1200, -500, 900, -1300, 400, -600, 1000, -800, 500};

    int64_t pos = -1;
    int64_t expected = -1;
    int64_t cleanStart = 48000; // where a jitter-free clock would put each frame
    for (int64_t j : jitter) {
        const int64_t start =
            NativeSrtIngestSession::advanceAudioFifoSample(&pos, cleanStart + j, kDecoded, kResync);
        if (expected < 0) {
            expected = start; // first frame anchors
        }
        QCOMPARE(start, expected); // contiguous: the clock jitter must not leak through
        expected += kDecoded;
        cleanStart += kDecoded; // the underlying clock advances one frame
    }
}

// A genuine forward jump beyond the resync window (a gap / seek, not jitter) must
// re-anchor to the clock rather than silently absorb a multi-second discontinuity.
void TestSrtAudioFifo::realDiscontinuityReanchors() {
    constexpr int kDecoded = 1024;
    constexpr int64_t kResync = 9600;

    int64_t pos = -1;
    NativeSrtIngestSession::advanceAudioFifoSample(&pos, 48000, kDecoded, kResync); // anchor
    const int64_t contiguousNext = 48000 + kDecoded;

    // Small jitter (< resync) is absorbed onto the contiguous position.
    QCOMPARE(NativeSrtIngestSession::advanceAudioFifoSample(&pos, contiguousNext + 300, kDecoded,
                                                            kResync),
             contiguousNext);

    // A 20 000-sample (~417 ms) jump exceeds the resync window -> re-anchor.
    const int64_t jumped = contiguousNext + kDecoded + 20000;
    QCOMPARE(NativeSrtIngestSession::advanceAudioFifoSample(&pos, jumped, kDecoded, kResync),
             jumped);
}

void TestSrtAudioFifo::clockJitterStaysFrameContiguous() {
    constexpr int64_t kResyncMs = 200;
    constexpr qint64 kFrame90k = 3000; // 30 fps on the MPEG-TS 90 kHz clock
    const int64_t jitterMs[] = {0, 12, -18, 31, -24, 43, -37, 16, -11, 28};

    int64_t anchorTs90k = -1;
    int64_t anchorStreamMs = -1;
    const qint64 firstPts90k = 90000;
    const int64_t firstCleanMs = 1000;

    for (int i = 0; i < int(std::size(jitterMs)); ++i) {
        const qint64 pts90k = firstPts90k + i * kFrame90k;
        const int64_t cleanMs =
            NativeSrtIngestSession::sourcePtsMsFromAnchor(pts90k, firstPts90k, firstCleanMs);
        const int64_t mappedMs = NativeSrtIngestSession::advanceVideoFramePtsMs(
            &anchorTs90k, &anchorStreamMs, pts90k, cleanMs + jitterMs[i], kResyncMs);
        QCOMPARE(mappedMs, cleanMs);
    }
}

void TestSrtAudioFifo::realVideoDiscontinuityReanchors() {
    constexpr int64_t kResyncMs = 200;
    constexpr qint64 kFrame90k = 3000;

    int64_t anchorTs90k = -1;
    int64_t anchorStreamMs = -1;
    QCOMPARE(NativeSrtIngestSession::advanceVideoFramePtsMs(&anchorTs90k, &anchorStreamMs, 90000,
                                                            1000, kResyncMs),
             1000);

    const qint64 nextPts90k = 90000 + kFrame90k;
    const int64_t nextCleanMs =
        NativeSrtIngestSession::sourcePtsMsFromAnchor(nextPts90k, 90000, 1000);
    QCOMPARE(NativeSrtIngestSession::advanceVideoFramePtsMs(
                 &anchorTs90k, &anchorStreamMs, nextPts90k, nextCleanMs + 40, kResyncMs),
             nextCleanMs);

    const qint64 jumpedPts90k = nextPts90k + kFrame90k;
    const int64_t jumpedClockMs =
        NativeSrtIngestSession::sourcePtsMsFromAnchor(jumpedPts90k, 90000, 1000) + 300;
    QCOMPARE(NativeSrtIngestSession::advanceVideoFramePtsMs(&anchorTs90k, &anchorStreamMs,
                                                            jumpedPts90k, jumpedClockMs, kResyncMs),
             jumpedClockMs);
}

QTEST_GUILESS_MAIN(TestSrtAudioFifo)
#include "tst_srtaudiofifo.moc"

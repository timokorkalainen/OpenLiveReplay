#include <QtTest>

#include "playback/audioplayer.h"

class TestAudioPlayerMuteFade : public QObject {
    Q_OBJECT
private slots:
    void muteRampsTailToZeroNotHardCut();
};

void TestAudioPlayerMuteFade::muteRampsTailToZeroNotHardCut() {
    AudioRingBuffer ring;
    ring.open(QIODevice::ReadWrite);

    // Push 600 stereo S16 frames at full scale: 600 * 2 channels * 2 bytes = 2400 bytes.
    // This exceeds the 240-frame fade tail so fadeOutAndClear will trim and ramp.
    const int kPushFrames = 600;
    const int kChannels = 2;
    QByteArray loud(kPushFrames * kChannels * int(sizeof(qint16)), Qt::Uninitialized);
    int16_t* src = reinterpret_cast<int16_t*>(loud.data());
    const int totalSamples = kPushFrames * kChannels;
    for (int i = 0; i < totalSamples; ++i)
        src[i] = qint16(0x7FFF);
    ring.push(loud.constData(), loud.size());

    ring.fadeOutAndClear(kChannels);

    // The buffer must be NON-EMPTY: fadeOutAndClear keeps a ramped tail.
    // A hard clear() would leave avail == 0 and the next QVERIFY would fail.
    const qint64 avail = ring.bytesAvailable();
    QVERIFY(avail > 0);

    // Read back EXACTLY avail bytes — no padding appended, no silence tail.
    QByteArray out(avail, Qt::Uninitialized);
    const qint64 n = ring.read(out.data(), avail);
    QCOMPARE(n, avail);

    // Reinterpret as int16.  For stereo the stride per frame is 2 samples.
    // Frame 0: gain ≈ 1.0  → magnitude near 0x7FFF.
    // Frame N-1: gain ≈ 1/N → magnitude much smaller.
    const int16_t* samples = reinterpret_cast<const int16_t*>(out.constData());
    const int nSamples = int(avail) / int(sizeof(int16_t));
    const int nFrames = nSamples / kChannels;
    QVERIFY(nFrames >= 2);

    // Left channel of the first and last retained frame.
    const int firstSample = samples[0];                        // frame 0, ch 0
    const int lastSample = samples[(nFrames - 1) * kChannels]; // last frame, ch 0

    // First frame should be near full-scale (gain == 1.0, input was 0x7FFF).
    QVERIFY(qAbs(firstSample) > 0x7FFF / 2);

    // Last frame's magnitude is strictly less than half the first frame's.
    QVERIFY(qAbs(lastSample) < qAbs(firstSample) / 2);
}

QTEST_GUILESS_MAIN(TestAudioPlayerMuteFade)
#include "tst_audioplayer_mutefade.moc"

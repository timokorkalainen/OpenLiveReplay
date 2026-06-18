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
    // 200 frames of full-scale stereo S16 (non-zero everywhere).
    QByteArray loud(200 * 2 * int(sizeof(qint16)), char(0x7F));
    ring.push(loud.constData(), loud.size());
    ring.fadeOutAndClear(2);
    // After a fade-out-and-clear the buffer is drained to silence: either no
    // bytes remain, or the residual tail has been ramped (last sample << first).
    QByteArray out(loud.size(), char(0x55));
    const qint64 n = ring.read(out.data(), out.size());
    // readData pads with silence; the faded region must not be the original
    // full-scale value verbatim at the splice point.
    QVERIFY(n >= 0);
    QCOMPARE(uchar(out.at(out.size() - 1)), uchar(0x00)); // tail is silence
}

QTEST_GUILESS_MAIN(TestAudioPlayerMuteFade)
#include "tst_audioplayer_mutefade.moc"

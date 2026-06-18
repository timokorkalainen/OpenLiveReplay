#include <QtTest>

#include "tests/e2e/ndi_output_marker.h"

class TestNdiOutputMarker : public QObject {
    Q_OBJECT
private slots:
    void lumaCounterRoundTripsForManyIndices();
    void flashCellSetOnlyOnFlashFrames();
    void audioBeepEnergyOnlyOnFlashFrames();
};

void TestNdiOutputMarker::lumaCounterRoundTripsForManyIndices() {
    NdiOutputMarkerConfig cfg;
    for (qint64 i : {qint64(0), qint64(1), qint64(2), qint64(63), qint64(1000), qint64(65535),
                     qint64(1 << 20)}) {
        const QByteArray y = ndiMarkerLumaPlane(cfg, i);
        QCOMPARE(y.size(), cfg.width * cfg.height);
        const qint64 decoded =
            ndiMarkerDecodeIndex(cfg, reinterpret_cast<const uchar*>(y.constData()), cfg.width);
        QCOMPARE(decoded, i);
    }
}

void TestNdiOutputMarker::flashCellSetOnlyOnFlashFrames() {
    NdiOutputMarkerConfig cfg;
    for (qint64 i = 0; i < 60; ++i) {
        const QByteArray y = ndiMarkerLumaPlane(cfg, i);
        const bool flash =
            ndiMarkerDecodeFlash(cfg, reinterpret_cast<const uchar*>(y.constData()), cfg.width);
        QCOMPARE(flash, ndiMarkerIsFlashFrame(cfg, i));
    }
}

void TestNdiOutputMarker::audioBeepEnergyOnlyOnFlashFrames() {
    NdiOutputMarkerConfig cfg;
    const int n = ndiMarkerSamplesPerFrame(cfg);
    for (qint64 i = 0; i < 45; ++i) {
        const QByteArray pcm = ndiMarkerAudioS16(cfg, i);
        QCOMPARE(pcm.size(), n * cfg.channels * int(sizeof(qint16)));
        // Convert interleaved S16 -> one channel of float to reuse the FLTp RMS helper.
        std::vector<float> ch0(n);
        const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
        for (int k = 0; k < n; ++k)
            ch0[k] = float(s[k * cfg.channels]) / 32768.0f;
        const double rms = ndiMarkerAudioRmsFltp(ch0.data(), n);
        if (ndiMarkerIsFlashFrame(cfg, i)) {
            QVERIFY2(rms > 0.05, "flash frame must carry an audible beep");
        } else {
            QVERIFY2(rms < 1e-4, "non-flash frame must be silent");
        }
    }
}

QTEST_GUILESS_MAIN(TestNdiOutputMarker)
#include "tst_ndioutputmarker.moc"

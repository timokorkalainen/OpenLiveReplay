// Unit tests for PlaybackTransport — the scrub/seek/step/speed state machine
// that drives replay. Tests the deterministic math and signal emissions;
// timer-driven playback advancement is intentionally left to integration tests
// to keep these fast and non-flaky.
#include <QtTest>
#include <QSignalSpy>

#include "playback/playbacktransport.h"

class TestPlaybackTransport : public QObject {
    Q_OBJECT
private slots:
    void defaults();
    void setFpsRejectsNonPositive();
    void setSpeedRoundsToTwoDecimals();
    void seekClampsNegativeToZero();
    void seekEmitsPosChanged();
    void stepUsesFrameDuration();
    void setPlayingTogglesAndDedupes();
};

void TestPlaybackTransport::defaults() {
    PlaybackTransport t;
    QCOMPARE(t.fps(), 30);
    QCOMPARE(t.speed(), 1.0);
    QVERIFY(!t.isPlaying());
    QCOMPARE(t.currentPos(), qint64(0));
}

void TestPlaybackTransport::setFpsRejectsNonPositive() {
    PlaybackTransport t;
    QSignalSpy spy(&t, &PlaybackTransport::fpsChanged);

    t.setFps(0);
    t.setFps(-5);
    QCOMPARE(t.fps(), 30);
    QCOMPARE(spy.count(), 0);

    t.setFps(60);
    QCOMPARE(t.fps(), 60);
    QCOMPARE(spy.count(), 1);
}

void TestPlaybackTransport::setSpeedRoundsToTwoDecimals() {
    PlaybackTransport t;
    QSignalSpy spy(&t, &PlaybackTransport::speedChanged);

    t.setSpeed(1.236);
    QCOMPARE(t.speed(), 1.24);
    QCOMPARE(spy.count(), 1);

    // Same effective speed -> no duplicate signal.
    t.setSpeed(1.5);
    QCOMPARE(t.speed(), 1.5);
    QCOMPARE(spy.count(), 2);
    t.setSpeed(1.5);
    QCOMPARE(spy.count(), 2);
}

void TestPlaybackTransport::seekClampsNegativeToZero() {
    PlaybackTransport t;
    t.seek(5000);
    QCOMPARE(t.currentPos(), qint64(5000));
    t.seek(-100);
    QCOMPARE(t.currentPos(), qint64(0));
}

void TestPlaybackTransport::seekEmitsPosChanged() {
    PlaybackTransport t;
    QSignalSpy spy(&t, &PlaybackTransport::posChanged);
    t.seek(1234);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toLongLong(), qint64(1234));
}

void TestPlaybackTransport::stepUsesFrameDuration() {
    PlaybackTransport t;
    t.setFps(30);
    t.seek(0);
    t.step(1); // 1000/30 = 33.33 -> 33ms
    QCOMPARE(t.currentPos(), qint64(33));
    t.step(-1); // back to 0
    QCOMPARE(t.currentPos(), qint64(0));
    t.step(3); // 3 * 1000/30 = 100ms exactly
    QCOMPARE(t.currentPos(), qint64(100));

    t.setFps(60);
    t.seek(0);
    t.step(1); // 1000/60 = 16.66 -> 16ms
    QCOMPARE(t.currentPos(), qint64(16));
}

void TestPlaybackTransport::setPlayingTogglesAndDedupes() {
    PlaybackTransport t;
    QSignalSpy spy(&t, &PlaybackTransport::playingChanged);

    t.setPlaying(true);
    QVERIFY(t.isPlaying());
    QCOMPARE(spy.count(), 1);

    t.setPlaying(true); // already playing -> no signal
    QCOMPARE(spy.count(), 1);

    t.setPlaying(false);
    QVERIFY(!t.isPlaying());
    QCOMPARE(spy.count(), 2);
}

QTEST_GUILESS_MAIN(TestPlaybackTransport)
#include "tst_playbacktransport.moc"

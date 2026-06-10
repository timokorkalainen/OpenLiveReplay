// Unit tests for Muxer — the FFmpeg Matroska container writer. Verifies the
// track layout (video / audio / subtitle offsets), stream bounds checking,
// audio channel-layout handling, and that init() actually produces a file.
//
// Hermetic: Muxer::getVideoPath() normally writes to <Documents>/videos, which
// on macOS cannot be redirected via $HOME or QStandardPaths test mode. Each
// test instead points the muxer at a per-run QTemporaryDir via
// setOutputBaseDir(), so nothing is written outside the temp dir and the whole
// tree is auto-removed when the test object is destroyed.
#include <QtTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>

#include "recorder_engine/muxer.h"

class TestMuxer : public QObject {
    Q_OBJECT
private slots:
    void initBuildsTrackLayout();
    void getStreamIsBoundsChecked();
    void stereoAudioChannelLayout();
    void monoAudioChannelLayout();
    void initProducesAFile();

private:
    QTemporaryDir m_home;
    QString videoPathFor(const QString& name) const;
};

QString TestMuxer::videoPathFor(const QString& name) const {
    return m_home.path() + "/videos/" + name + ".mkv";
}

void TestMuxer::initBuildsTrackLayout() {
    Muxer m;
    m.setOutputBaseDir(m_home.path());
    const QStringList names{QStringLiteral("A"), QStringLiteral("B")};
    QVERIFY(m.init(QStringLiteral("olr_unit_layout"), 2, 640, 480, 30, names, 48000, 2));
    // 2 video + 2 audio + 2 subtitle, in that order.
    QCOMPARE(m.audioTrackOffset(), 2);
    QCOMPARE(m.subtitleTrackOffset(), 4);
    m.close();
}

void TestMuxer::getStreamIsBoundsChecked() {
    Muxer m;
    m.setOutputBaseDir(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_bounds"), 1, 320, 240, 30, names, 48000, 2));
    // 1 video + 1 audio + 1 subtitle = 3 streams (indices 0..2).
    QVERIFY(m.getStream(0) != nullptr);
    QVERIFY(m.getStream(2) != nullptr);
    QVERIFY(m.getStream(3) == nullptr);
    QVERIFY(m.getStream(-1) == nullptr);
    m.close();
}

void TestMuxer::stereoAudioChannelLayout() {
    Muxer m;
    m.setOutputBaseDir(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_stereo"), 1, 320, 240, 30, names, 48000, 2));
    AVStream* audio = m.getStream(m.audioTrackOffset());
    QVERIFY(audio != nullptr);
    QCOMPARE(audio->codecpar->ch_layout.nb_channels, 2);
    m.close();
}

void TestMuxer::monoAudioChannelLayout() {
    Muxer m;
    m.setOutputBaseDir(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_mono"), 1, 320, 240, 30, names, 48000, 1));
    AVStream* audio = m.getStream(m.audioTrackOffset());
    QVERIFY(audio != nullptr);
    QCOMPARE(audio->codecpar->ch_layout.nb_channels, 1);
    m.close();
}

void TestMuxer::initProducesAFile() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputBaseDir(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_file"), 1, 320, 240, 30, names, 48000, 2));
    m.close();
    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_file")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));
    // The path must be inside the temp dir — guards against a regression where
    // the muxer ignores the override and writes to the real Documents tree.
    QVERIFY2(fi.filePath().startsWith(m_home.path()), "output escaped the temp dir");
    QVERIFY(fi.size() > 0);
}

QTEST_GUILESS_MAIN(TestMuxer)
#include "tst_muxer.moc"

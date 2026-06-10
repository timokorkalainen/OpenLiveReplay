// Unit tests for Muxer — the FFmpeg Matroska container writer. Verifies the
// track layout (video / audio / subtitle offsets), stream bounds checking,
// audio channel-layout handling, and that init() actually produces a file.
//
// Muxer::getVideoPath() hardcodes <Documents>/videos/<name>.mkv, so the test
// redirects $HOME to a temporary directory to stay hermetic, and removes any
// file it produces in cleanup.
#include <QtTest>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>

#include "recorder_engine/muxer.h"

class TestMuxer : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanup();
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
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return docs + "/videos/" + name + ".mkv";
}

void TestMuxer::initTestCase() {
    QVERIFY(m_home.isValid());
    // Redirect the home dir so <Documents>/videos resolves inside the temp dir.
    qputenv("HOME", m_home.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(QDir(m_home.path()).mkpath(QStringLiteral("Documents/videos")));
}

void TestMuxer::cleanup() {
    // Remove any .mkv produced during a test, wherever it landed.
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QDir videos(docs + "/videos");
    for (const QString& f : videos.entryList({QStringLiteral("olr_unit_*.mkv")}, QDir::Files)) {
        videos.remove(f);
    }
}

void TestMuxer::initBuildsTrackLayout() {
    Muxer m;
    const QStringList names{QStringLiteral("A"), QStringLiteral("B")};
    QVERIFY(m.init(QStringLiteral("olr_unit_layout"), 2, 640, 480, 30, names, 48000, 2));
    // 2 video + 2 audio + 2 subtitle, in that order.
    QCOMPARE(m.audioTrackOffset(), 2);
    QCOMPARE(m.subtitleTrackOffset(), 4);
    m.close();
}

void TestMuxer::getStreamIsBoundsChecked() {
    Muxer m;
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
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_stereo"), 1, 320, 240, 30, names, 48000, 2));
    AVStream* audio = m.getStream(m.audioTrackOffset());
    QVERIFY(audio != nullptr);
    QCOMPARE(audio->codecpar->ch_layout.nb_channels, 2);
    m.close();
}

void TestMuxer::monoAudioChannelLayout() {
    Muxer m;
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_mono"), 1, 320, 240, 30, names, 48000, 1));
    AVStream* audio = m.getStream(m.audioTrackOffset());
    QVERIFY(audio != nullptr);
    QCOMPARE(audio->codecpar->ch_layout.nb_channels, 1);
    m.close();
}

void TestMuxer::initProducesAFile() {
    Muxer m;
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_file"), 1, 320, 240, 30, names, 48000, 2));
    m.close();
    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_file")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));
    QVERIFY(fi.size() > 0);
}

QTEST_GUILESS_MAIN(TestMuxer)
#include "tst_muxer.moc"

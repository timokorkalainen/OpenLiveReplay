// Unit tests for SettingsManager — JSON persistence of app/source/MIDI config.
// Focuses on the save->load round trip and graceful failure on bad input.
#include <QtTest>
#include <QTemporaryDir>

#include "settingsmanager.h"

class TestSettingsManager : public QObject {
    Q_OBJECT
private slots:
    void roundTripPreservesEverything();
    void loadMissingFileReturnsFalse();
    void loadMalformedJsonReturnsFalse();
    void saveToUnwritablePathReturnsFalse();

private:
    static AppSettings sampleSettings();
};

AppSettings TestSettingsManager::sampleSettings() {
    AppSettings s;
    s.saveLocation = QStringLiteral("/tmp/recordings");
    s.fileName = QStringLiteral("match_01");
    s.videoWidth = 1280;
    s.videoHeight = 720;
    s.fps = 50;
    s.multiviewCount = 6;
    s.showTimeOfDay = true;
    s.midiPortName = QStringLiteral("X-Touch One");
    s.audioOutputLatencyMs = 180;

    SourceSettings a;
    a.id = QStringLiteral("src-a");
    a.name = QStringLiteral("Cam A");
    a.url = QStringLiteral("srt://10.0.0.2:9000");
    a.metadata = QJsonArray{QJsonObject{{"k", "angle"}, {"v", "wide"}}};
    a.trimOffsetMs = -66; // advance
    SourceSettings b;
    b.id = QStringLiteral("src-b");
    b.name = QStringLiteral("Cam B");
    b.url = QStringLiteral("udp://10.0.0.3:9001");
    b.trimOffsetMs = 132; // delay
    s.sources = {a, b};

    s.metadataFields = QJsonArray{QJsonObject{{"name", "angle"}, {"display", "Angle"}}};

    // action 1 -> (status 144, data1 60), with all three data2 variants set
    s.midiBindings.insert(1, qMakePair(144, 60));
    s.midiBindingData2.insert(1, 100);
    s.midiBindingData2Forward.insert(1, 1);
    s.midiBindingData2Backward.insert(1, 127);

    // Stream Deck per-model mapping tables (model id -> index -> action id).
    s.streamDeckKeyMaps.insert(QStringLiteral("plusXL"),
                               QList<int>{9, 0, 4, -1, -1});
    s.streamDeckDialPressMaps.insert(QStringLiteral("plusXL"),
                                     QList<int>{0, 5, -1, -1, -1, -1});
    s.streamDeckDialRotateMaps.insert(QStringLiteral("plusXL"),
                                      QList<int>{8, 10, -1, -1, -1, -1});
    return s;
}

void TestSettingsManager::roundTripPreservesEverything() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));

    SettingsManager mgr;
    const AppSettings in = sampleSettings();
    QVERIFY(mgr.save(path, in));

    AppSettings out;
    QVERIFY(mgr.load(path, out));

    QCOMPARE(out.saveLocation, in.saveLocation);
    QCOMPARE(out.fileName, in.fileName);
    QCOMPARE(out.videoWidth, in.videoWidth);
    QCOMPARE(out.videoHeight, in.videoHeight);
    QCOMPARE(out.fps, in.fps);
    QCOMPARE(out.audioOutputLatencyMs, in.audioOutputLatencyMs);
    QCOMPARE(out.multiviewCount, in.multiviewCount);
    QCOMPARE(out.showTimeOfDay, in.showTimeOfDay);
    QCOMPARE(out.midiPortName, in.midiPortName);

    QCOMPARE(out.sources.size(), 2);
    QCOMPARE(out.sources[0].id, in.sources[0].id);
    QCOMPARE(out.sources[0].name, in.sources[0].name);
    QCOMPARE(out.sources[0].url, in.sources[0].url);
    QCOMPARE(out.sources[0].metadata, in.sources[0].metadata);
    QCOMPARE(out.sources[1].url, in.sources[1].url);
    QCOMPARE(out.sources[0].trimOffsetMs, in.sources[0].trimOffsetMs);
    QCOMPARE(out.sources[1].trimOffsetMs, in.sources[1].trimOffsetMs);

    QCOMPARE(out.metadataFields, in.metadataFields);

    QVERIFY(out.midiBindings.contains(1));
    QCOMPARE(out.midiBindings.value(1), qMakePair(144, 60));
    QCOMPARE(out.midiBindingData2.value(1), 100);
    QCOMPARE(out.midiBindingData2Forward.value(1), 1);
    QCOMPARE(out.midiBindingData2Backward.value(1), 127);

    QCOMPARE(out.streamDeckKeyMaps, in.streamDeckKeyMaps);
    QCOMPARE(out.streamDeckDialPressMaps, in.streamDeckDialPressMaps);
    QCOMPARE(out.streamDeckDialRotateMaps, in.streamDeckDialRotateMaps);
}

void TestSettingsManager::loadMissingFileReturnsFalse() {
    QTemporaryDir dir;
    SettingsManager mgr;
    AppSettings out;
    QVERIFY(!mgr.load(dir.filePath(QStringLiteral("nope.json")), out));
}

void TestSettingsManager::loadMalformedJsonReturnsFalse() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("bad.json"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ this is not valid json ");
    f.close();

    SettingsManager mgr;
    AppSettings out;
    QVERIFY(!mgr.load(path, out));
}

void TestSettingsManager::saveToUnwritablePathReturnsFalse() {
    SettingsManager mgr;
    // A directory that does not exist -> open for write fails.
    QVERIFY(!mgr.save(QStringLiteral("/nonexistent-dir-xyz/settings.json"), sampleSettings()));
}

QTEST_GUILESS_MAIN(TestSettingsManager)
#include "tst_settingsmanager.moc"

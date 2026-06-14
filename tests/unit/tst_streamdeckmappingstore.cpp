// Unit tests for StreamDeckMappingStore: move/displace semantics, validation,
// defaults, per-model isolation, AppSettings round trip, and the shuttle ladder.
#include <QtTest>

#include "streamdeck/streamdeckmappingstore.h"

using ET = StreamDeckMappingStore::ElementType;

class TestStreamDeckMappingStore : public QObject {
    Q_OBJECT
private slots:
    void defaultLayoutMatchesPriority();
    void bindMovesActionAndDisplacesOccupant();
    void bindToDialPressAndTurn();
    void validationRejectsBadPairings();
    void clearUnbinds();
    void perModelIsolation();
    void appSettingsRoundTrip();
    void loadPrunesOutOfRangeIndices();
    void resetRestoresDefault();
    void shuttleLadderSnapsClampsAndPauses();
};

void TestStreamDeckMappingStore::defaultLayoutMatchesPriority() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    const QList<int> keys = s.keyMap("plusXL");
    QCOMPARE(keys.size(), 36);
    QCOMPARE(keys.mid(0, 11), (QList<int>{9, 0, 4, 5, 7, 3, 1, 2, 6, 20, 21}));
    QCOMPARE(keys.at(11), -1);
    QCOMPARE(s.dialRotateMap("plusXL").at(0), 8);
    QCOMPARE(s.dialPressMap("plusXL").at(0), 0);
    QCOMPARE(s.dialRotateMap("plusXL").at(1), -1);
}

void TestStreamDeckMappingStore::bindMovesActionAndDisplacesOccupant() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    QVERIFY(s.bind("plusXL", 5, ET::Key, 20, 36, 6));
    QCOMPARE(s.keyMap("plusXL").at(20), 5);
    QCOMPARE(s.keyMap("plusXL").at(3), -1);
    QVERIFY(s.bind("plusXL", 0, ET::Key, 0, 36, 6));
    QCOMPARE(s.keyMap("plusXL").at(0), 0);
    QCOMPARE(s.bindingLabel("plusXL", 9), QStringLiteral("Unassigned"));
}

void TestStreamDeckMappingStore::bindToDialPressAndTurn() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    QVERIFY(s.bind("plusXL", 5, ET::DialPress, 2, 36, 6));
    QCOMPARE(s.dialPressMap("plusXL").at(2), 5);
    QCOMPARE(s.bindingLabel("plusXL", 5), QStringLiteral("Dial 2 press"));
    QVERIFY(s.bind("plusXL", 10, ET::DialTurn, 3, 36, 6));
    QCOMPARE(s.dialRotateMap("plusXL").at(3), 10);
    QCOMPARE(s.bindingLabel("plusXL", 10), QStringLiteral("Dial 3 turn"));
}

void TestStreamDeckMappingStore::validationRejectsBadPairings() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    QVERIFY(!s.bind("plusXL", 8, ET::Key, 5, 36, 6));
    QVERIFY(!s.bind("plusXL", 10, ET::DialPress, 1, 36, 6));
    QVERIFY(!s.bind("plusXL", 20, ET::DialPress, 1, 36, 6));
    QVERIFY(!s.bind("plusXL", 0, ET::DialTurn, 1, 36, 6));
    QVERIFY(s.bind("plusXL", 0, ET::DialPress, 1, 36, 6));
}

void TestStreamDeckMappingStore::clearUnbinds() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    s.clear("plusXL", 9);
    QCOMPARE(s.keyMap("plusXL").at(0), -1);
    QCOMPARE(s.bindingLabel("plusXL", 9), QStringLiteral("Unassigned"));
}

void TestStreamDeckMappingStore::perModelIsolation() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    s.resetToDefault("mini", 6, 0);
    s.bind("plusXL", 0, ET::Key, 30, 36, 6);
    QCOMPARE(s.keyMap("mini").size(), 6);
    QVERIFY(s.keyMap("mini").indexOf(0) != 30);
}

void TestStreamDeckMappingStore::appSettingsRoundTrip() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    s.bind("plusXL", 10, ET::DialTurn, 4, 36, 6);
    AppSettings settings;
    s.writeTo(settings);
    StreamDeckMappingStore s2;
    s2.loadFrom(settings);
    QCOMPARE(s2.keyMap("plusXL"), s.keyMap("plusXL"));
    QCOMPARE(s2.dialRotateMap("plusXL"), s.dialRotateMap("plusXL"));
    QCOMPARE(s2.dialPressMap("plusXL"), s.dialPressMap("plusXL"));
}

void TestStreamDeckMappingStore::loadPrunesOutOfRangeIndices() {
    AppSettings settings;
    settings.streamDeckKeyMaps.insert("mini", QList<int>{9, 0, 4, 5, 7, 3, 6, 20});
    StreamDeckMappingStore s;
    s.loadFrom(settings);
    s.clampToGeometry("mini", 6, 0);
    QCOMPARE(s.keyMap("mini").size(), 6);
    QCOMPARE(s.keyMap("mini"), (QList<int>{9, 0, 4, 5, 7, 3}));
}

void TestStreamDeckMappingStore::resetRestoresDefault() {
    StreamDeckMappingStore s;
    s.resetToDefault("plusXL", 36, 6);
    const QList<int> original = s.keyMap("plusXL");
    s.bind("plusXL", 0, ET::Key, 30, 36, 6);
    s.resetToDefault("plusXL", 36, 6);
    QCOMPARE(s.keyMap("plusXL"), original);
}

void TestStreamDeckMappingStore::shuttleLadderSnapsClampsAndPauses() {
    ShuttleResult r = shuttleLadderStep(1.0, +1);
    QCOMPARE(r.speed, 2.0);
    QVERIFY(r.playing);
    r = shuttleLadderStep(1.0, -1);
    QCOMPARE(r.speed, 0.0);
    QVERIFY(!r.playing);
    r = shuttleLadderStep(5.0, +3);
    QCOMPARE(r.speed, 5.0);
    r = shuttleLadderStep(3.0, +1);
    QCOMPARE(r.speed, 5.0);
}

QTEST_MAIN(TestStreamDeckMappingStore)
#include "tst_streamdeckmappingstore.moc"

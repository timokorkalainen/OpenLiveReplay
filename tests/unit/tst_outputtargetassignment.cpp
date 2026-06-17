#include <QtTest>

#include "playback/output/outputtargetassignment.h"

class TestOutputTargetAssignment : public QObject {
    Q_OBJECT
private slots:
    void assignmentStoresLogicalBusSeparatelyFromTargetKind();
    void togglingEnabledDoesNotChangeSourceBus();
    void targetKindNamesAreStable();
};

void TestOutputTargetAssignment::assignmentStoresLogicalBusSeparatelyFromTargetKind() {
    OutputTargetAssignment ndi;
    ndi.id = QStringLiteral("feed1-ndi");
    ndi.sourceBus = OutputBusId::feed(0);
    ndi.kind = OutputTargetKind::Ndi;
    ndi.enabled = true;
    ndi.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QCOMPARE(ndi.sourceBus, OutputBusId::feed(0));
    QCOMPARE(ndi.kind, OutputTargetKind::Ndi);
    QCOMPARE(ndi.settings.value(QStringLiteral("senderName")).toString(),
             QStringLiteral("OLR Feed 1"));
}

void TestOutputTargetAssignment::togglingEnabledDoesNotChangeSourceBus() {
    OutputTargetAssignment target;
    target.sourceBus = OutputBusId::pgm();
    target.kind = OutputTargetKind::DeckLinkSdiHdmi;
    target.enabled = true;
    target.setEnabled(false);

    QVERIFY(!target.enabled);
    QCOMPARE(target.sourceBus, OutputBusId::pgm());
    QCOMPARE(target.kind, OutputTargetKind::DeckLinkSdiHdmi);
}

void TestOutputTargetAssignment::targetKindNamesAreStable() {
    QCOMPARE(outputTargetKindName(OutputTargetKind::DeckLinkSdiHdmi),
             QStringLiteral("decklink-sdi-hdmi"));
    QCOMPARE(outputTargetKindName(OutputTargetKind::DeckLinkIpSt2110),
             QStringLiteral("decklink-ip-st2110"));
    QCOMPARE(outputTargetKindName(OutputTargetKind::Ndi), QStringLiteral("ndi"));
    QCOMPARE(outputTargetKindName(OutputTargetKind::Omt), QStringLiteral("omt"));
    QCOMPARE(outputTargetKindName(OutputTargetKind::Aja), QStringLiteral("aja"));
}

QTEST_GUILESS_MAIN(TestOutputTargetAssignment)
#include "tst_outputtargetassignment.moc"

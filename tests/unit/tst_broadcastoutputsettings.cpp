#include <QtTest>

#include "playback/output/broadcastoutputsettings.h"

class TestBroadcastOutputSettings : public QObject {
    Q_OBJECT
private slots:
    void ensureTargetsCreatesOneNdiTargetPerBus();
    void setEnabledPreservesExistingSettingsAndOnlyTouchesSelectedBus();
    void rowsExposeStableQmlMetadata();
    void rowsIncludeRuntimeStatusDiagnostics();
    void rowsDoNotWarnForExpectedSilentAudio();
    void rowsUseLastSubmitResultForCurrentHealth();
    void qtPreviewAssignmentsCoverFeedsMultiviewAndPgm();
};

void TestBroadcastOutputSettings::ensureTargetsCreatesOneNdiTargetPerBus() {
    const QList<OutputTargetAssignment> outputs =
        BroadcastOutputSettings::ensureTargets({}, 2, OutputTargetKind::Ndi);

    QCOMPARE(outputs.size(), 4);
    QCOMPARE(outputs[0].id, QStringLiteral("feed0-ndi"));
    QCOMPARE(outputs[0].sourceBus, OutputBusId::feed(0));
    QCOMPARE(outputs[0].kind, OutputTargetKind::Ndi);
    QVERIFY(!outputs[0].enabled);
    QCOMPARE(outputs[0].settings.value(QStringLiteral("senderName")).toString(),
             QStringLiteral("OpenLiveReplay Feed 1"));

    QCOMPARE(outputs[1].id, QStringLiteral("feed1-ndi"));
    QCOMPARE(outputs[1].sourceBus, OutputBusId::feed(1));
    QCOMPARE(outputs[1].settings.value(QStringLiteral("senderName")).toString(),
             QStringLiteral("OpenLiveReplay Feed 2"));

    QCOMPARE(outputs[2].id, QStringLiteral("multiview-ndi"));
    QCOMPARE(outputs[2].sourceBus, OutputBusId::multiview());
    QCOMPARE(outputs[2].settings.value(QStringLiteral("senderName")).toString(),
             QStringLiteral("OpenLiveReplay Multiview"));

    QCOMPARE(outputs[3].id, QStringLiteral("pgm-ndi"));
    QCOMPARE(outputs[3].sourceBus, OutputBusId::pgm());
    QCOMPARE(outputs[3].settings.value(QStringLiteral("senderName")).toString(),
             QStringLiteral("OpenLiveReplay PGM"));
}

void TestBroadcastOutputSettings::setEnabledPreservesExistingSettingsAndOnlyTouchesSelectedBus() {
    OutputTargetAssignment existing;
    existing.id = QStringLiteral("feed1-ndi");
    existing.sourceBus = OutputBusId::feed(1);
    existing.kind = OutputTargetKind::Ndi;
    existing.enabled = false;
    existing.settings.insert(QStringLiteral("senderName"), QStringLiteral("Director Iso 2"));
    existing.settings.insert(QStringLiteral("groups"), QStringLiteral("OB1"));

    const QList<OutputTargetAssignment> outputs = BroadcastOutputSettings::setEnabled(
        {existing}, 3, OutputTargetKind::Ndi, OutputBusId::feed(1), true);

    QCOMPARE(outputs.size(), 5);
    QVERIFY(
        !BroadcastOutputSettings::isEnabled(outputs, OutputTargetKind::Ndi, OutputBusId::feed(0)));
    QVERIFY(
        BroadcastOutputSettings::isEnabled(outputs, OutputTargetKind::Ndi, OutputBusId::feed(1)));
    QVERIFY(
        !BroadcastOutputSettings::isEnabled(outputs, OutputTargetKind::Ndi, OutputBusId::feed(2)));
    QCOMPARE(
        BroadcastOutputSettings::senderName(outputs, OutputTargetKind::Ndi, OutputBusId::feed(1)),
        QStringLiteral("Director Iso 2"));

    const auto feed1 = std::find_if(outputs.cbegin(), outputs.cend(), [](const auto& assignment) {
        return assignment.kind == OutputTargetKind::Ndi &&
               assignment.sourceBus == OutputBusId::feed(1);
    });
    QVERIFY(feed1 != outputs.cend());
    QCOMPARE(feed1->settings.value(QStringLiteral("groups")).toString(), QStringLiteral("OB1"));
}

void TestBroadcastOutputSettings::rowsExposeStableQmlMetadata() {
    QList<OutputTargetAssignment> outputs =
        BroadcastOutputSettings::setEnabled({}, 1, OutputTargetKind::Ndi, OutputBusId::pgm(), true);
    outputs = BroadcastOutputSettings::setSenderName(
        outputs, 1, OutputTargetKind::Ndi, OutputBusId::pgm(), QStringLiteral(" World Feed "));

    const QVariantList rows = BroadcastOutputSettings::rows(outputs, 1, OutputTargetKind::Ndi);

    QCOMPARE(rows.size(), 3);
    const QVariantMap feed = rows[0].toMap();
    QCOMPARE(feed.value(QStringLiteral("id")).toString(), QStringLiteral("feed0-ndi"));
    QCOMPARE(feed.value(QStringLiteral("busKind")).toString(), QStringLiteral("feed"));
    QCOMPARE(feed.value(QStringLiteral("feedIndex")).toInt(), 0);
    QCOMPARE(feed.value(QStringLiteral("label")).toString(), QStringLiteral("Feed 1"));
    QVERIFY(!feed.value(QStringLiteral("enabled")).toBool());

    const QVariantMap pgm = rows[2].toMap();
    QCOMPARE(pgm.value(QStringLiteral("id")).toString(), QStringLiteral("pgm-ndi"));
    QCOMPARE(pgm.value(QStringLiteral("busKind")).toString(), QStringLiteral("pgm"));
    QCOMPARE(pgm.value(QStringLiteral("feedIndex")).toInt(), -1);
    QCOMPARE(pgm.value(QStringLiteral("label")).toString(), QStringLiteral("PGM"));
    QVERIFY(pgm.value(QStringLiteral("enabled")).toBool());
    QCOMPARE(pgm.value(QStringLiteral("senderName")).toString(), QStringLiteral("World Feed"));
}

void TestBroadcastOutputSettings::rowsIncludeRuntimeStatusDiagnostics() {
    QList<OutputTargetAssignment> outputs = BroadcastOutputSettings::setEnabled(
        {}, 1, OutputTargetKind::Ndi, OutputBusId::feed(0), true);

    BroadcastOutputTargetStatus status;
    status.attemptedFrames = 12;
    status.framesSubmitted = 11;
    status.sinkFailures = 1;
    status.placeholderFrames = 2;
    status.silentAudioFrames = 3;
    status.repeatedPayloadFrames = 4;
    status.hasLastIdentity = true;
    status.lastIdentity.bus = OutputBusId::feed(0);
    status.lastIdentity.outputFrameIndex = 99;
    status.lastIdentity.sampledPlayheadMs = 4200;
    status.lastIdentity.sourceFeedIndex = 0;
    status.lastIdentity.sourcePtsMs = 4160;
    status.lastIdentity.videoPlaceholder = false;
    status.lastIdentity.audioSilent = true;

    QHash<QString, BroadcastOutputTargetStatus> statuses;
    statuses.insert(QStringLiteral("feed0-ndi"), status);

    const QVariantList rows =
        BroadcastOutputSettings::rows(outputs, 1, OutputTargetKind::Ndi, statuses);

    QCOMPARE(rows.size(), 3);
    const QVariantMap feed = rows[0].toMap();
    QCOMPARE(feed.value(QStringLiteral("statusState")).toString(), QStringLiteral("Active"));
    QCOMPARE(feed.value(QStringLiteral("statusSeverity")).toString(), QStringLiteral("ok"));
    QCOMPARE(feed.value(QStringLiteral("attemptedFrames")).toLongLong(), qint64(12));
    QCOMPARE(feed.value(QStringLiteral("framesSubmitted")).toLongLong(), qint64(11));
    QCOMPARE(feed.value(QStringLiteral("sinkFailures")).toLongLong(), qint64(1));
    QCOMPARE(feed.value(QStringLiteral("placeholderFrames")).toLongLong(), qint64(2));
    QCOMPARE(feed.value(QStringLiteral("silentAudioFrames")).toLongLong(), qint64(3));
    QCOMPARE(feed.value(QStringLiteral("repeatedPayloadFrames")).toLongLong(), qint64(4));
    QCOMPARE(feed.value(QStringLiteral("lastOutputFrameIndex")).toLongLong(), qint64(99));
    QCOMPARE(feed.value(QStringLiteral("lastPlayheadMs")).toLongLong(), qint64(4200));
    QCOMPARE(feed.value(QStringLiteral("lastSourceFeedIndex")).toInt(), 0);
    QCOMPARE(feed.value(QStringLiteral("lastSourcePtsMs")).toLongLong(), qint64(4160));
    QVERIFY(feed.value(QStringLiteral("diagnostic")).toString().contains(QStringLiteral("fail=1")));
    QVERIFY(feed.value(QStringLiteral("diagnostic")).toString().contains(QStringLiteral("silent=3")));

    const QVariantMap pgm = rows[2].toMap();
    QCOMPARE(pgm.value(QStringLiteral("statusState")).toString(), QStringLiteral("Off"));
    QCOMPARE(pgm.value(QStringLiteral("statusSeverity")).toString(), QStringLiteral("off"));
    QCOMPARE(pgm.value(QStringLiteral("framesSubmitted")).toLongLong(), qint64(0));
}

void TestBroadcastOutputSettings::rowsDoNotWarnForExpectedSilentAudio() {
    QList<OutputTargetAssignment> outputs = BroadcastOutputSettings::setEnabled(
        {}, 1, OutputTargetKind::Ndi, OutputBusId::feed(0), true);

    BroadcastOutputTargetStatus status;
    status.attemptedFrames = 4;
    status.framesSubmitted = 4;
    status.silentAudioFrames = 4;
    status.hasLastIdentity = true;
    status.lastIdentity.bus = OutputBusId::feed(0);
    status.lastIdentity.outputFrameIndex = 3;
    status.lastIdentity.audioSilent = true;
    status.hasLastSubmitResult = true;
    status.lastSubmitSucceeded = true;

    QHash<QString, BroadcastOutputTargetStatus> statuses;
    statuses.insert(QStringLiteral("feed0-ndi"), status);

    const QVariantMap feed =
        BroadcastOutputSettings::rows(outputs, 1, OutputTargetKind::Ndi, statuses)[0].toMap();
    QCOMPARE(feed.value(QStringLiteral("statusState")).toString(), QStringLiteral("Active"));
    QCOMPARE(feed.value(QStringLiteral("statusSeverity")).toString(), QStringLiteral("ok"));
    QCOMPARE(feed.value(QStringLiteral("silentAudioFrames")).toLongLong(), qint64(4));
}

void TestBroadcastOutputSettings::rowsUseLastSubmitResultForCurrentHealth() {
    QList<OutputTargetAssignment> outputs = BroadcastOutputSettings::setEnabled(
        {}, 1, OutputTargetKind::Ndi, OutputBusId::feed(0), true);

    BroadcastOutputTargetStatus recovered;
    recovered.attemptedFrames = 8;
    recovered.framesSubmitted = 7;
    recovered.sinkFailures = 1;
    recovered.hasLastIdentity = true;
    recovered.lastIdentity.bus = OutputBusId::feed(0);
    recovered.lastIdentity.outputFrameIndex = 7;
    recovered.hasLastSubmitResult = true;
    recovered.lastSubmitSucceeded = true;

    QHash<QString, BroadcastOutputTargetStatus> statuses;
    statuses.insert(QStringLiteral("feed0-ndi"), recovered);

    QVariantMap feed =
        BroadcastOutputSettings::rows(outputs, 1, OutputTargetKind::Ndi, statuses)[0].toMap();
    QCOMPARE(feed.value(QStringLiteral("statusState")).toString(), QStringLiteral("Active"));
    QCOMPARE(feed.value(QStringLiteral("statusSeverity")).toString(), QStringLiteral("ok"));

    recovered.lastSubmitSucceeded = false;
    statuses.insert(QStringLiteral("feed0-ndi"), recovered);
    feed = BroadcastOutputSettings::rows(outputs, 1, OutputTargetKind::Ndi, statuses)[0].toMap();
    QCOMPARE(feed.value(QStringLiteral("statusState")).toString(), QStringLiteral("Error"));
    QCOMPARE(feed.value(QStringLiteral("statusSeverity")).toString(), QStringLiteral("error"));
}

void TestBroadcastOutputSettings::qtPreviewAssignmentsCoverFeedsMultiviewAndPgm() {
    const QList<OutputTargetAssignment> previews =
        BroadcastOutputSettings::qtPreviewAssignments(2, true, true);

    QCOMPARE(previews.size(), 4);
    QCOMPARE(previews[0].id, QStringLiteral("qt-preview-feed-0"));
    QCOMPARE(previews[0].sourceBus, OutputBusId::feed(0));
    QCOMPARE(previews[0].kind, OutputTargetKind::QtPreview);
    QVERIFY(previews[0].enabled);

    QCOMPARE(previews[1].id, QStringLiteral("qt-preview-feed-1"));
    QCOMPARE(previews[1].sourceBus, OutputBusId::feed(1));

    QCOMPARE(previews[2].id, QStringLiteral("qt-preview-multiview"));
    QCOMPARE(previews[2].sourceBus, OutputBusId::multiview());

    QCOMPARE(previews[3].id, QStringLiteral("qt-preview-pgm"));
    QCOMPARE(previews[3].sourceBus, OutputBusId::pgm());
}

QTEST_GUILESS_MAIN(TestBroadcastOutputSettings)
#include "tst_broadcastoutputsettings.moc"

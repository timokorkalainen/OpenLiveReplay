#include <QtTest>

#include "playback/playlistentriesmodel.h"

static ReplayEntry entry(const QString& clipPath, qint64 inMs, qint64 outMs, double speed = 1.0) {
    ReplayEntry e;
    e.clipPath = clipPath;
    e.inMs = inMs;
    e.outMs = outMs;
    e.speed = speed;
    return e;
}

class TestPlaylistEntriesModel : public QObject {
    Q_OBJECT
private slots:
    void exposesExpectedRoles();
    void setEntriesResetsRowsAndData();
    void openOutPointHasNoDuration();
    void invalidIndexReturnsInvalidVariant();
};

void TestPlaylistEntriesModel::exposesExpectedRoles() {
    PlaylistEntriesModel model;
    const QHash<int, QByteArray> roles = model.roleNames();

    QCOMPARE(roles.value(PlaylistEntriesModel::IndexRole), QByteArray("index"));
    QCOMPARE(roles.value(PlaylistEntriesModel::ClipPathRole), QByteArray("clipPath"));
    QCOMPARE(roles.value(PlaylistEntriesModel::LabelRole), QByteArray("label"));
    QCOMPARE(roles.value(PlaylistEntriesModel::InMsRole), QByteArray("inMs"));
    QCOMPARE(roles.value(PlaylistEntriesModel::OutMsRole), QByteArray("outMs"));
    QCOMPARE(roles.value(PlaylistEntriesModel::DurationMsRole), QByteArray("durationMs"));
    QCOMPARE(roles.value(PlaylistEntriesModel::SpeedRole), QByteArray("speed"));
    QCOMPARE(roles.value(PlaylistEntriesModel::HasOutRole), QByteArray("hasOut"));
    QCOMPARE(roles.value(PlaylistEntriesModel::BoundaryReadyRole), QByteArray("boundaryReady"));
}

void TestPlaylistEntriesModel::setEntriesResetsRowsAndData() {
    PlaylistEntriesModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    model.setEntries({entry(QStringLiteral("/clips/cam-a.mkv"), 1000, 2500, 0.5),
                      entry(QStringLiteral("/clips/cam-b.mkv"), 3000, 4300, 2.0)});

    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(model.rowCount(), 2);

    const QModelIndex first = model.index(0, 0);
    QCOMPARE(model.data(first, PlaylistEntriesModel::IndexRole).toInt(), 0);
    QCOMPARE(model.data(first, PlaylistEntriesModel::ClipPathRole).toString(),
             QStringLiteral("/clips/cam-a.mkv"));
    QCOMPARE(model.data(first, PlaylistEntriesModel::LabelRole).toString(),
             QStringLiteral("cam-a.mkv"));
    QCOMPARE(model.data(first, PlaylistEntriesModel::InMsRole).toLongLong(), qint64(1000));
    QCOMPARE(model.data(first, PlaylistEntriesModel::OutMsRole).toLongLong(), qint64(2500));
    QCOMPARE(model.data(first, PlaylistEntriesModel::DurationMsRole).toLongLong(), qint64(1500));
    QCOMPARE(model.data(first, PlaylistEntriesModel::SpeedRole).toDouble(), 0.5);
    QCOMPARE(model.data(first, PlaylistEntriesModel::HasOutRole).toBool(), true);
    QCOMPARE(model.data(first, PlaylistEntriesModel::BoundaryReadyRole).toBool(), true);
}

void TestPlaylistEntriesModel::openOutPointHasNoDuration() {
    PlaylistEntriesModel model;
    model.setEntries({entry(QStringLiteral("/clips/open.mkv"), 1000, -1)});

    const QModelIndex first = model.index(0, 0);
    QCOMPARE(model.data(first, PlaylistEntriesModel::DurationMsRole).toLongLong(), qint64(-1));
    QCOMPARE(model.data(first, PlaylistEntriesModel::HasOutRole).toBool(), false);
    QCOMPARE(model.data(first, PlaylistEntriesModel::BoundaryReadyRole).toBool(), false);
}

void TestPlaylistEntriesModel::invalidIndexReturnsInvalidVariant() {
    PlaylistEntriesModel model;
    model.setEntries({entry(QStringLiteral("/clips/open.mkv"), 1000, -1)});

    QVERIFY(!model.data(QModelIndex(), PlaylistEntriesModel::LabelRole).isValid());
    QVERIFY(!model.data(model.index(3, 0), PlaylistEntriesModel::LabelRole).isValid());
}

QTEST_GUILESS_MAIN(TestPlaylistEntriesModel)
#include "tst_playlistentriesmodel.moc"

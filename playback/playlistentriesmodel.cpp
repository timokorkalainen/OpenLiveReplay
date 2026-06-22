#include "playback/playlistentriesmodel.h"

#include <QFileInfo>

PlaylistEntriesModel::PlaylistEntriesModel(QObject* parent) : QAbstractListModel(parent) {}

void PlaylistEntriesModel::setEntries(const QVector<ReplayEntry>& entries) {
    beginResetModel();
    m_rows = entries;
    endResetModel();
}

int PlaylistEntriesModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant PlaylistEntriesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }

    const ReplayEntry& entry = m_rows.at(index.row());
    switch (role) {
    case IndexRole:
        return index.row();
    case ClipPathRole:
        return entry.clipPath;
    case LabelRole: {
        const QString fileName = QFileInfo(entry.clipPath).fileName();
        return fileName.isEmpty() ? entry.clipPath : fileName;
    }
    case InMsRole:
        return entry.inMs;
    case OutMsRole:
        return entry.outMs;
    case DurationMsRole:
        return entry.outMs < 0 ? qint64(-1) : entry.outMs - entry.inMs;
    case SpeedRole:
        return entry.speed;
    case HasOutRole:
        return entry.outMs >= 0;
    case BoundaryReadyRole:
        return entry.outMs >= entry.inMs;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> PlaylistEntriesModel::roleNames() const {
    return {
        {IndexRole, QByteArrayLiteral("index")},
        {ClipPathRole, QByteArrayLiteral("clipPath")},
        {LabelRole, QByteArrayLiteral("label")},
        {InMsRole, QByteArrayLiteral("inMs")},
        {OutMsRole, QByteArrayLiteral("outMs")},
        {DurationMsRole, QByteArrayLiteral("durationMs")},
        {SpeedRole, QByteArrayLiteral("speed")},
        {HasOutRole, QByteArrayLiteral("hasOut")},
        {BoundaryReadyRole, QByteArrayLiteral("boundaryReady")},
    };
}

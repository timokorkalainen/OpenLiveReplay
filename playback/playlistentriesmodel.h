#ifndef PLAYLISTENTRIESMODEL_H
#define PLAYLISTENTRIESMODEL_H

#include "playback/replayplaylist.h"

#include <QAbstractListModel>
#include <QVector>

class PlaylistEntriesModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        IndexRole = Qt::UserRole + 1,
        ClipPathRole,
        LabelRole,
        InMsRole,
        OutMsRole,
        DurationMsRole,
        SpeedRole,
        HasOutRole,
        BoundaryReadyRole,
    };
    Q_ENUM(Roles)

    explicit PlaylistEntriesModel(QObject* parent = nullptr);

    void setEntries(const QVector<ReplayEntry>& entries);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    QVector<ReplayEntry> m_rows;
};

#endif // PLAYLISTENTRIESMODEL_H

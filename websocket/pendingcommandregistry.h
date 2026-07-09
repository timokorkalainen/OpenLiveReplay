#ifndef PENDINGCOMMANDREGISTRY_H
#define PENDINGCOMMANDREGISTRY_H

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>

// Correlates accepted transactional operator commands (keyed by playback-worker
// epoch + seek generation) with the client/command that issued them, and delivers
// exactly one completion per entry: worker resolution, supersession, client
// departure, or deadline — whichever comes first. Protocol-layer types only
// (no playback dependencies) so it unit-tests in isolation.
class PendingCommandRegistry : public QObject {
    Q_OBJECT
public:
    explicit PendingCommandRegistry(QObject* parent = nullptr);

    void setDeadlineMs(int ms);
    void registerPending(quint64 epoch, quint64 generation, const QString& clientId,
                         const QString& commandId);
    void resolve(quint64 epoch, quint64 generation, QJsonObject completion);
    void noteWorkerEpoch(quint64 epoch);
    void dropClient(const QString& clientId);

    // Deadline scan against a caller-supplied monotonic timestamp; the internal
    // timer calls this with nowMsForTest(). Public so tests drive it directly.
    void checkDeadlines(qint64 nowMs);
    qint64 nowMsForTest() const { return m_clock.elapsed(); }

signals:
    void commandCompleted(const QString& clientId, const QString& commandId,
                          const QJsonObject& completion);
    void pendingExpired(quint64 epoch, quint64 generation);

private:
    struct Entry {
        QString clientId;
        QString commandId;
        qint64 registeredAtMs = 0;
    };
    void deliverLocked(const QPair<quint64, quint64>& key, const Entry& entry,
                       QJsonObject completion, qint64 nowMs);
    void updateTimer();

    QHash<QPair<quint64, quint64>, Entry> m_entries;
    QElapsedTimer m_clock;
    QTimer m_timer;
    int m_deadlineMs = 1000;
    quint64 m_currentEpoch = 0;
};

#endif

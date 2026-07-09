#include "pendingcommandregistry.h"

PendingCommandRegistry::PendingCommandRegistry(QObject* parent) : QObject(parent) {
    m_clock.start();
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, [this]() { checkDeadlines(m_clock.elapsed()); });
}

void PendingCommandRegistry::setDeadlineMs(int ms) {
    m_deadlineMs = ms > 0 ? ms : 1000;
}

void PendingCommandRegistry::registerPending(quint64 epoch, quint64 generation,
                                             const QString& clientId, const QString& commandId) {
    if (epoch > m_currentEpoch) m_currentEpoch = epoch;
    Entry entry;
    entry.clientId = clientId;
    entry.commandId = commandId;
    entry.registeredAtMs = m_clock.elapsed();
    m_entries.insert({epoch, generation}, entry);
    updateTimer();
}

void PendingCommandRegistry::resolve(quint64 epoch, quint64 generation, QJsonObject completion) {
    const auto key = QPair<quint64, quint64>{epoch, generation};
    const auto it = m_entries.constFind(key);
    if (it == m_entries.cend()) return; // one-shot: already resolved, or never ours
    // Must copy: the entry is erased on the next line, so a reference would dangle
    // when deliverLocked reads it below.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const Entry entry = it.value();
    m_entries.erase(it);
    deliverLocked(key, entry, std::move(completion), m_clock.elapsed());
    updateTimer();
}

void PendingCommandRegistry::noteWorkerEpoch(quint64 epoch) {
    if (epoch <= m_currentEpoch) return;
    m_currentEpoch = epoch;
    const auto entries = m_entries;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (it.key().first >= epoch) continue;
        m_entries.remove(it.key());
        QJsonObject completion{{QStringLiteral("done"), false},
                               {QStringLiteral("reason"), QStringLiteral("superseded")}};
        deliverLocked(it.key(), it.value(), std::move(completion), m_clock.elapsed());
    }
    updateTimer();
}

void PendingCommandRegistry::dropClient(const QString& clientId) {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        it = it.value().clientId == clientId ? m_entries.erase(it) : std::next(it);
    }
    updateTimer();
}

void PendingCommandRegistry::checkDeadlines(qint64 nowMs) {
    const auto entries = m_entries;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (nowMs - it.value().registeredAtMs < m_deadlineMs) continue;
        m_entries.remove(it.key());
        emit pendingExpired(it.key().first, it.key().second);
        QJsonObject completion{{QStringLiteral("done"), false},
                               {QStringLiteral("reason"), QStringLiteral("timeout")}};
        deliverLocked(it.key(), it.value(), std::move(completion), nowMs);
    }
    updateTimer();
}

void PendingCommandRegistry::deliverLocked(const QPair<quint64, quint64>& key, const Entry& entry,
                                           QJsonObject completion, qint64 nowMs) {
    Q_UNUSED(key);
    completion.insert(QStringLiteral("latencyMs"),
                      static_cast<double>(nowMs - entry.registeredAtMs));
    emit commandCompleted(entry.clientId, entry.commandId, completion);
}

void PendingCommandRegistry::updateTimer() {
    if (m_entries.isEmpty()) {
        m_timer.stop();
    } else if (!m_timer.isActive()) {
        m_timer.start();
    }
}

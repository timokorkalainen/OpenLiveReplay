#include "playback/playlistplayout.h"

#include <QtGlobal>

void PlaylistPlayout::start(const QVector<ReplayEntry>& entries, int index) {
    m_entries = entries;
    m_index = (index >= 0 && index < m_entries.size()) ? index : -1;
    m_active = (m_index >= 0);
    m_armedCurrent = false;
}

void PlaylistPlayout::stop() {
    m_active = false;
    m_armedCurrent = false;
    m_index = -1;
    m_entries.clear();
}

std::optional<ReplayEntry> PlaylistPlayout::entryAt(int index) const {
    if (index < 0 || index >= m_entries.size()) return std::nullopt;
    return m_entries[index];
}

PlaylistPlayout::Boundary PlaylistPlayout::evaluate(qint64 playheadMs, double currentSpeed,
                                                    qint64 armLeadRealtimeMs) {
    Boundary none;
    if (!m_active || m_armedCurrent || !hasNext()) return none;

    const ReplayEntry cur = m_entries[m_index];
    if (cur.outMs < 0) return none; // no out-point marked — let playback flow forward

    // Convert the wall-clock lead into a clip-time distance so the realtime pre-roll
    // headroom is constant regardless of speed: at 2x we must arm twice as early in
    // clip time; at 0.5x half as early (and there is ample realtime either way).
    const double speed = currentSpeed > 0.0 ? currentSpeed : 1.0;
    const qint64 armClipLead = qint64(double(armLeadRealtimeMs) * speed);
    if (playheadMs < cur.outMs - armClipLead) return none; // not within the lead yet

    const ReplayEntry next = m_entries[m_index + 1];
    m_armedCurrent = true;
    return Boundary{/*valid*/ true, cur.outMs, next.inMs, next.speed};
}

std::optional<ReplayEntry> PlaylistPlayout::onBoundaryFired() {
    if (!m_active || m_index < 0 || m_index + 1 >= m_entries.size()) return std::nullopt;
    ++m_index;
    m_armedCurrent = false;
    return m_entries[m_index];
}

#ifndef PLAYLISTPLAYOUT_H
#define PLAYLISTPLAYOUT_H

#include <QVector>
#include <optional>

#include "playback/replayplaylist.h"

// Pure EVS-rundown decision logic for automatic playlist playout. No Qt I/O, no
// threads. Given a snapshot of playlist entries it decides, as playback advances,
// which boundary cut to arm and when, and advances to the next entry as each
// boundary cut fires. The caller performs the side effects (seek, set speed, arm the
// armed cut) and feeds back the live playhead and "boundary fired" events.
//
// End of list: the final entry has no successor, so no boundary is armed for it —
// the caller simply lets normal playback continue forward (the recording is a live,
// growing file). There is no loop/stop state here.
class PlaylistPlayout {
public:
    // A boundary cut to arm: fire when the current entry's playhead reaches fireAtMs
    // (its out-point), jumping to targetMs (the next entry's in-point), after which
    // the next entry plays at targetSpeed.
    struct Boundary {
        bool valid = false;
        qint64 fireAtMs = -1;
        qint64 targetMs = 0;
        double targetSpeed = 1.0;
    };

    // Begin playout at `index` over `entries`. The caller seeks to the entry's
    // in-point and applies its speed (see currentEntry()).
    void start(const QVector<ReplayEntry>& entries, int index);
    void stop();

    bool active() const { return m_active; }
    int currentIndex() const { return m_index; }
    int count() const { return m_entries.size(); }
    bool onFinalEntry() const { return m_active && !hasNext(); }
    std::optional<ReplayEntry> entryAt(int index) const;
    std::optional<ReplayEntry> currentEntry() const { return entryAt(m_index); }

    // Decide whether to arm the boundary cut for the current entry now. Returns a
    // valid Boundary exactly ONCE per boundary — when the playhead is within the
    // speed-adjusted lead of the current entry's out-point and a next entry exists.
    // armLeadRealtimeMs is the wall-clock lead before the out-point at which to arm;
    // it is scaled by the current speed into a clip-time distance so the realtime
    // pre-roll headroom is constant across playout speeds (incl. slow-motion).
    Boundary evaluate(qint64 playheadMs, double currentSpeed, qint64 armLeadRealtimeMs);

    // Call when the armed boundary cut fires. Advances to the next entry and returns
    // it (apply its speed). Returns nullopt only if there is no next entry.
    std::optional<ReplayEntry> onBoundaryFired();

private:
    bool hasNext() const { return m_active && m_index >= 0 && m_index + 1 < m_entries.size(); }

    QVector<ReplayEntry> m_entries;
    int m_index = -1;
    bool m_active = false;
    bool m_armedCurrent = false; // a boundary has already been armed for m_index
};

#endif // PLAYLISTPLAYOUT_H

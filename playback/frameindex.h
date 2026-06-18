#ifndef FRAMEINDEX_H
#define FRAMEINDEX_H

#include <QtGlobal>
#include <optional>
#include <vector>

// PTS(ms) -> byte-offset map, appended incrementally as packets are read.
// Recordings are ALL-INTRA, so any indexed offset is a valid decode start.
class FrameIndex {
public:
    void append(qint64 ptsMs, qint64 byteOffset);
    std::optional<qint64> nearestAtOrBefore(qint64 ptsMs) const;
    std::optional<qint64> newestPtsMs() const;
    int size() const { return static_cast<int>(m_entries.size()); }
    void clear() { m_entries.clear(); }

private:
    struct Entry {
        qint64 ptsMs;
        qint64 byteOffset;
    };
    std::vector<Entry> m_entries; // strictly increasing ptsMs
};

#endif // FRAMEINDEX_H

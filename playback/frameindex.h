#ifndef FRAMEINDEX_H
#define FRAMEINDEX_H

#include <QtGlobal>
#include <optional>
#include <vector>

// PTS(ms) -> byte-offset map, appended incrementally as packets are read.
// The offsets identify known packets, but container demuxers may still require
// timestamp-based entry points before decoding from those packets.
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

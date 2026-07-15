#ifndef DECODEDFRAMEEVIDENCEQUEUE_H
#define DECODEDFRAMEEVIDENCEQUEUE_H

#include "recorder_engine/timing/timecodeevidence.h"

#include <QtGlobal>

#include <cstdint>
#include <deque>
#include <optional>

struct DecodedFrameEvidence {
    qint64 codecPts90k = 0;
    int64_t sourcePtsMs = -1;
    int64_t sourceTimecode100ns = -1;
    std::optional<TimecodeEvidence> timecodeEvidence;
    uint64_t carrierGeneration = 0;
};

class DecodedFrameEvidenceQueue {
public:
    explicit DecodedFrameEvidenceQueue(qsizetype maximumEntries = 64);

    uint64_t enqueue(DecodedFrameEvidence evidence);
    std::optional<DecodedFrameEvidence> takeForOutputPts(qint64 outputPts90k);
    bool discard(uint64_t submissionId);
    void clear();
    qsizetype size() const { return qsizetype(m_entries.size()); }

private:
    struct Entry {
        uint64_t submissionId = 0;
        DecodedFrameEvidence evidence;
    };

    qsizetype m_maximumEntries = 64;
    uint64_t m_nextSubmissionId = 1;
    std::deque<Entry> m_entries;
};

#endif // DECODEDFRAMEEVIDENCEQUEUE_H

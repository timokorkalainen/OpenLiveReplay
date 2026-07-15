#ifndef DECODEDFRAMEEVIDENCEQUEUE_H
#define DECODEDFRAMEEVIDENCEQUEUE_H

#include "recorder_engine/timing/timecodeevidence.h"

#include <QtGlobal>

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

struct DecodedFrameEvidence {
    struct CarrierSessionIdentity {
        uint64_t value = 0;
    };
    struct CarrierGeneration {
        uint64_t value = 0;
    };

    DecodedFrameEvidence(qint64 codecPts, int64_t sourcePts, int64_t sourceTimecode,
                         std::optional<TimecodeEvidence> timingEvidence)
        : DecodedFrameEvidence(codecPts, sourcePts, sourceTimecode, std::move(timingEvidence),
                               CarrierSessionIdentity{}, CarrierGeneration{}) {}

    DecodedFrameEvidence(qint64 codecPts, int64_t sourcePts, int64_t sourceTimecode,
                         std::optional<TimecodeEvidence> timingEvidence,
                         CarrierSessionIdentity sessionIdentity, CarrierGeneration generation)
        : codecPts90k(codecPts), sourcePtsMs(sourcePts), sourceTimecode100ns(sourceTimecode),
          timecodeEvidence(std::move(timingEvidence)),
          carrierSessionIdentity(sessionIdentity.value), carrierGeneration(generation.value) {}

    qint64 codecPts90k = 0;
    int64_t sourcePtsMs = -1;
    int64_t sourceTimecode100ns = -1;
    std::optional<TimecodeEvidence> timecodeEvidence;
    uint64_t carrierSessionIdentity = 0;
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

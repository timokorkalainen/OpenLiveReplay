#include "decodedframeevidencequeue.h"

#include <algorithm>
#include <limits>

extern "C" {
#include <libavutil/avutil.h>
}

DecodedFrameEvidenceQueue::DecodedFrameEvidenceQueue(qsizetype maximumEntries)
    : m_maximumEntries(std::max<qsizetype>(1, maximumEntries)) {}

uint64_t DecodedFrameEvidenceQueue::enqueue(DecodedFrameEvidence evidence) {
    if (evidence.codecPts90k == AV_NOPTS_VALUE) return 0;
    while (qsizetype(m_entries.size()) >= m_maximumEntries) {
        m_entries.pop_front();
    }
    const uint64_t submissionId = m_nextSubmissionId;
    if (m_nextSubmissionId != std::numeric_limits<uint64_t>::max()) ++m_nextSubmissionId;
    m_entries.push_back({submissionId, std::move(evidence)});
    return submissionId;
}

std::optional<DecodedFrameEvidence>
DecodedFrameEvidenceQueue::takeForOutputPts(qint64 outputPts90k) {
    if (outputPts90k == AV_NOPTS_VALUE) return std::nullopt;
    const auto found =
        std::find_if(m_entries.begin(), m_entries.end(), [outputPts90k](const Entry& entry) {
            return entry.evidence.codecPts90k == outputPts90k;
        });
    if (found == m_entries.end()) return std::nullopt;
    DecodedFrameEvidence evidence = std::move(found->evidence);
    m_entries.erase(found);
    return evidence;
}

bool DecodedFrameEvidenceQueue::discard(uint64_t submissionId) {
    if (submissionId == 0) return false;
    const auto found =
        std::find_if(m_entries.begin(), m_entries.end(), [submissionId](const Entry& entry) {
            return entry.submissionId == submissionId;
        });
    if (found == m_entries.end()) return false;
    m_entries.erase(found);
    return true;
}

void DecodedFrameEvidenceQueue::clear() {
    m_entries.clear();
}

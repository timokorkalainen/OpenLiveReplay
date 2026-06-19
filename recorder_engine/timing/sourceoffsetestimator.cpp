#include "sourceoffsetestimator.h"

SourceOffsetEstimator::SourceOffsetEstimator(int boundedBaseMs, int boundedPpmMsPerSec)
    : m_boundedBaseMs(boundedBaseMs > 0 ? boundedBaseMs : 0),
      m_boundedPpmMsPerSec(boundedPpmMsPerSec > 0 ? boundedPpmMsPerSec : 0) {}

void SourceOffsetEstimator::update(int sourceIndex, const SourcePhaseEvidence& ev) {
    if (sourceIndex < 0 || sourceIndex >= kMaxSources) return;

    Entry& entry = m_entries[sourceIndex];
    entry.set = true;
    entry.offsetMs = ev.measuredOffsetMs;

    // Tier grading, descending: a common timecode (or an external PTP reference)
    // makes equal frames coincide -> FrameAccurate; any recovered clock lock
    // (Pcr/Ndi/FlvPll) gives a numerically bounded estimate -> Bounded; otherwise
    // arrival timing only -> Approximate.
    if (ev.timecodeAlignedToReference || ev.externalReference) {
        entry.tier = ConfidenceTier::FrameAccurate;
        entry.boundMs = 0;
        return;
    }

    if (ev.clockLocked) {
        entry.tier = ConfidenceTier::Bounded;
        // Residual drift the servo cannot remove within the integration window,
        // in ms: |ppm| * window / 1e6. 0 by default (no ppm term configured).
        const double ppm = ev.clockPpm < 0.0 ? -ev.clockPpm : ev.clockPpm;
        const int ppmTerm = static_cast<int>((ppm * m_boundedPpmMsPerSec) / 1'000'000.0);
        int bound = m_boundedBaseMs + ppmTerm;
        // RTMP's ms-resolution FLV timestamps are noisier -> wider base bound.
        if (ev.clockQuality == ClockQuality::FlvPll) bound += kFlvExtraBoundMs;
        entry.boundMs = bound;
        return;
    }

    entry.tier = ConfidenceTier::Approximate;
    entry.boundMs = kApproximateBoundMs;
}

ConfidenceTier SourceOffsetEstimator::tier(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= kMaxSources) return ConfidenceTier::Approximate;
    const Entry& entry = m_entries[sourceIndex];
    if (!entry.set) return ConfidenceTier::Approximate;
    return entry.tier;
}

int64_t SourceOffsetEstimator::offsetMs(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= kMaxSources) return 0;
    return m_entries[sourceIndex].offsetMs;
}

int SourceOffsetEstimator::boundMs(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= kMaxSources) return kApproximateBoundMs;
    const Entry& entry = m_entries[sourceIndex];
    if (!entry.set) return kApproximateBoundMs;
    return entry.boundMs;
}

bool SourceOffsetEstimator::hasEvidence(int sourceIndex) const {
    if (sourceIndex < 0 || sourceIndex >= kMaxSources) return false;
    return m_entries[sourceIndex].set;
}

void SourceOffsetEstimator::reset() {
    for (int i = 0; i < kMaxSources; ++i)
        m_entries[i] = Entry{};
}

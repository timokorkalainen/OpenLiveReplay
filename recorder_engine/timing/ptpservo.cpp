#include "ptpservo.h"

#include <algorithm>
#include <cstdlib>

PtpServo::PtpServo(int minExchanges) : m_minExchanges(std::max(1, minExchanges)) {}

void PtpServo::observe(const PtpExchange& ex) {
    if (!ex.valid) {
        return;
    }

    // IEEE 1588 two-way time transfer: the symmetric-path mean delay and the
    // local->master offset. Both fall out of the four timestamps directly.
    const int64_t meanPathDelay = ((ex.t2 - ex.t1) + (ex.t4 - ex.t3)) / 2;
    const int64_t offset = (ex.t2 - ex.t1) - meanPathDelay;

    // Outlier rejection: once we have a converged window, a single sample whose
    // offset is wildly off the running mean is a packet-delay spike, not a real
    // clock move -> drop it so it neither jerks the estimate nor breaks lock. The
    // guard is intentionally wide so a genuine step in the true offset (which
    // arrives as a *run* of consistent new samples, not one spike) is NOT rejected
    // forever: the first post-step sample may be skipped, but subsequent ones land
    // within the guard of each other and are absorbed (ramped via the running mean).
    if (m_locked && int(m_window.size()) >= m_minExchanges) {
        recompute();
        const int64_t deviation = std::llabs(offset - m_offsetNs);
        // Compare against the recent sample spread plus a fixed guard band.
        int64_t lo = m_window.front().offsetNs;
        int64_t hi = lo;
        for (const Sample& s : m_window) {
            lo = std::min(lo, s.offsetNs);
            hi = std::max(hi, s.offsetNs);
        }
        const int64_t spread = hi - lo;
        if (deviation > spread + kOutlierGuardNs) {
            // Far from the running estimate. A lone spike: reject and keep the lock +
            // estimate. But if a whole window's worth of samples are ALL consistently
            // far, this is a real step (failover/leap/re-lock), not jitter -> re-arm
            // onto the new level rather than locking the estimate out forever.
            if (++m_consecutiveRejects < m_minExchanges) {
                return; // lone spike (or not yet enough consistent samples to be sure)
            }
            m_window.clear(); // drop the stale level; refill from the new one
            m_nextIndex = 0;
            m_dirty = true;
            m_consecutiveRejects = 0;
            // fall through to seed the new level
        } else {
            m_consecutiveRejects = 0; // sample agrees with the estimate; not a step
        }
    }

    pushSample(offset, meanPathDelay);
}

void PtpServo::pushSample(int64_t offsetNs, int64_t pathDelayNs) {
    const Sample sample{offsetNs, pathDelayNs};
    if (int(m_window.size()) < m_minExchanges) {
        m_window.push_back(sample);
    } else {
        m_window[m_nextIndex] = sample;
        m_nextIndex = (m_nextIndex + 1) % m_window.size();
    }
    ++m_count;
    m_dirty = true;
    if (m_count >= m_minExchanges) {
        m_locked = true;
    }
}

void PtpServo::recompute() const {
    if (!m_dirty) {
        return;
    }
    m_dirty = false;
    if (m_window.empty()) {
        m_offsetNs = 0;
        m_pathDelayNs = 0;
        return;
    }
    // Simple low-pass: the running mean over the windowed accepted samples. A step
    // in the true offset ramps in as the window fills with the new level rather
    // than snapping instantly.
    //
    // Baseline-relative accumulation (the DriftEstimator "relative-to-first-point"
    // pattern): with a REAL grandmaster the offset is `(t2-t1) - meanPathDelay`
    // where t1 is ABSOLUTE PTP ns (~1.78e18) and t2 is local-monotonic ns (small),
    // so offset ~= -1.78e18. Summing minExchanges of those directly would overflow
    // int64 (8 * 1.78e18 = 1.4e19 > INT64_MAX). Subtracting a per-field baseline
    // first leaves only ns-scale jitter in the sum, then re-adding the baseline
    // recovers the true mean. Numerically identical to the naive mean for the
    // small-offset case (the existing test vectors), with no overflow for large
    // absolute offsets.
    const int64_t baseOffset = m_window.front().offsetNs;
    const int64_t baseDelay = m_window.front().pathDelayNs;
    int64_t sumOffset = 0; // Σ(s.offsetNs - baseOffset): ns-scale, no overflow
    int64_t sumDelay = 0;  // Σ(s.pathDelayNs - baseDelay): ns-scale, no overflow
    for (const Sample& s : m_window) {
        sumOffset += s.offsetNs - baseOffset;
        sumDelay += s.pathDelayNs - baseDelay;
    }
    const int64_t n = int64_t(m_window.size());
    m_offsetNs = baseOffset + sumOffset / n;
    m_pathDelayNs = baseDelay + sumDelay / n;
}

bool PtpServo::locked() const {
    return m_locked;
}

int64_t PtpServo::offsetNs() const {
    if (!m_locked) {
        return 0;
    }
    recompute();
    return m_offsetNs;
}

int64_t PtpServo::meanPathDelayNs() const {
    if (!m_locked) {
        return 0;
    }
    recompute();
    return m_pathDelayNs;
}

int64_t PtpServo::masterNsFromLocal(int64_t localNs) const {
    return localNs - offsetNs();
}

void PtpServo::reset() {
    m_count = 0;
    m_window.clear();
    m_nextIndex = 0;
    m_dirty = true;
    m_offsetNs = 0;
    m_pathDelayNs = 0;
    m_locked = false;
    m_consecutiveRejects = 0;
}

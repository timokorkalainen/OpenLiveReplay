#include "sourceclock.h"

#include <algorithm>
#include <cmath>

AnchoredSourceClock::AnchoredSourceClock(ClockQuality quality, int64_t unitsPerMs,
                                         int64_t forwardJumpMs,
                                         int64_t backwardToleranceMs)
    : m_quality(quality)
    , m_unitsPerMs(std::max<int64_t>(1, unitsPerMs))
    , m_forwardJumpMs(forwardJumpMs)
    , m_backwardToleranceMs(backwardToleranceMs) {}

void AnchoredSourceClock::observe(int64_t senderUnits, int64_t sessionNowMs, bool discontinuity,
                                  ClockObservationRole role) {
    if (senderUnits < 0) {
        return;
    }

    const bool authority = role == ClockObservationRole::Authority;
    bool needAnchor = m_anchorSenderUnits < 0 || (authority && discontinuity);
    if (!needAnchor && authority && m_prevAuthorityUnits >= 0) {
        const int64_t deltaMs = unitsToMs(senderUnits - m_prevAuthorityUnits);
        if (deltaMs > m_forwardJumpMs || deltaMs < m_backwardToleranceMs) {
            needAnchor = true;
        }
    }

    if (needAnchor) {
        m_anchorSenderUnits = senderUnits;
        m_anchorSessionMs = sessionNowMs;
    }

    if (authority) {
        m_prevAuthorityUnits = senderUnits;
    } else {
        m_prevFollowerUnits = senderUnits;
    }

    if (sessionNowMs >= 0) {
        m_drift.addSample(senderUnitsToNs(senderUnits), sessionNowMs * 1000000LL);
    }
}

int64_t AnchoredSourceClock::toSessionMs(int64_t mediaSenderUnits) const {
    if (!locked() || mediaSenderUnits < 0) {
        return -1;
    }
    return m_anchorSessionMs + unitsToMs(mediaSenderUnits - m_anchorSenderUnits);
}

void AnchoredSourceClock::reset() {
    m_anchorSenderUnits = -1;
    m_anchorSessionMs = -1;
    m_prevAuthorityUnits = -1;
    m_prevFollowerUnits = -1;
    m_drift.reset();
}

int64_t AnchoredSourceClock::unitsToMs(int64_t units) const {
    return units / m_unitsPerMs;
}

int64_t AnchoredSourceClock::senderUnitsToNs(int64_t units) const {
    return int64_t(std::llround((static_cast<long double>(units) * 1000000.0L) /
                                static_cast<long double>(m_unitsPerMs)));
}

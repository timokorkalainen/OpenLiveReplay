#ifndef SOURCECLOCK_H
#define SOURCECLOCK_H

#include "driftestimator.h"

#include <cstdint>

enum class ClockQuality {
    Arrival = 0,
    FlvPll = 1,
    Ndi = 2,
    Pcr = 3,
    Reference = 4,
};

enum class ClockObservationRole {
    Authority,
    Follower,
};

class SourceClock {
public:
    virtual ~SourceClock() = default;

    virtual void observe(int64_t senderUnits, int64_t sessionNowMs, bool discontinuity,
                         ClockObservationRole role = ClockObservationRole::Authority) = 0;
    virtual int64_t toSessionMs(int64_t mediaSenderUnits) const = 0;
    virtual ClockQuality quality() const = 0;
    virtual double ppm() const = 0;
    virtual bool locked() const = 0;
    virtual void reset() = 0;
};

class AnchoredSourceClock final : public SourceClock {
public:
    explicit AnchoredSourceClock(ClockQuality quality, int64_t unitsPerMs = 1,
                                 int64_t forwardJumpMs = 3000, int64_t backwardToleranceMs = -200);

    void observe(int64_t senderUnits, int64_t sessionNowMs, bool discontinuity,
                 ClockObservationRole role = ClockObservationRole::Authority) override;
    void addRateSample(int64_t senderUnits, int64_t sessionNowMs);
    int64_t toSessionMs(int64_t mediaSenderUnits) const override;
    ClockQuality quality() const override { return m_quality; }
    double ppm() const override { return m_drift.ppm(); }
    bool locked() const override { return m_anchorSenderUnits >= 0 && m_anchorSessionMs >= 0; }
    void reset() override;

private:
    int64_t unitsToMs(int64_t units) const;
    int64_t senderUnitsToNs(int64_t units) const;

    ClockQuality m_quality = ClockQuality::Arrival;
    int64_t m_unitsPerMs = 1;
    int64_t m_forwardJumpMs = 3000;
    int64_t m_backwardToleranceMs = -200;
    int64_t m_anchorSenderUnits = -1;
    int64_t m_anchorSessionMs = -1;
    int64_t m_prevAuthorityUnits = -1;
    int64_t m_prevFollowerUnits = -1;
    DriftEstimator m_drift;
};

#endif // SOURCECLOCK_H

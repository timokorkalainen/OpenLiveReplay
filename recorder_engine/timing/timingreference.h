#ifndef TIMINGREFERENCE_H
#define TIMINGREFERENCE_H
#include <cstdint>

class RecordingClock;

// The best-available session timebase, ascending trust. LocalMonotonic = the
// RecordingClock (no external reference); RecoveredConsensus = a consensus of the
// recovered sender clocks (reserved); Ptp = an ST 2059 / IEEE 1588 facility clock.
enum class ReferenceTier { LocalMonotonic = 0, RecoveredConsensus = 1, Ptp = 2 };

// The single source of "session now" the whole pipeline reads. Swapping the
// implementation (Local -> Ptp) re-times the session without caller changes.
class TimingReference {
public:
    virtual ~TimingReference() = default;
    virtual int64_t nowSessionNs() const = 0;
    virtual ReferenceTier tier() const = 0;
    virtual bool isExternal() const = 0; // true once a real reference (PTP) is locked
    // Convenience: ms since the session epoch (the timeline the muxer/heartbeat use).
    int64_t nowSessionMs() const { return nowSessionNs() / 1'000'000; }
};

// Wraps the existing RecordingClock: nowSessionNs == elapsedMs()*1e6. The default
// reference; byte-identical to today's behavior. Not external.
class LocalMonotonicReference final : public TimingReference {
public:
    explicit LocalMonotonicReference(const RecordingClock* clock);
    int64_t nowSessionNs() const override;
    ReferenceTier tier() const override { return ReferenceTier::LocalMonotonic; }
    bool isExternal() const override { return false; }

private:
    const RecordingClock* m_clock;
};
#endif // TIMINGREFERENCE_H

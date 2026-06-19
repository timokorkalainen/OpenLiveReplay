#include "timingreference.h"

#include "../recordingclock.h"

LocalMonotonicReference::LocalMonotonicReference(const RecordingClock* clock) : m_clock(clock) {}

int64_t LocalMonotonicReference::nowSessionNs() const {
    // Byte-identical to today's session-now: the RecordingClock is sampled in ms
    // (its native unit) and converted to the seam's ns by an exact x1'000'000.
    // Until the clock is started (isValid()) the session epoch is 0.
    return m_clock && m_clock->isValid() ? m_clock->elapsedMs() * 1'000'000LL : 0;
}

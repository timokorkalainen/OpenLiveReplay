#ifndef SEEKCOALESCER_H
#define SEEKCOALESCER_H

#include <cstdint>

// Pure logic for coalescing a burst of scrub targets. UIManager owns the timer
// and the actual worker->seekTo call; this class just decides "seek now vs.
// defer the latest target". The first offer of a gesture seeks immediately;
// subsequent offers are deferred (only the latest survives) until takePending()
// drains them; reset() ends the gesture so the next first-offer is immediate.
class SeekCoalescer {
public:
    // Returns true iff the caller should seek to `target` NOW.
    bool offer(int64_t target) {
        if (!m_armed) {
            m_armed = true;
            m_hasPending = false;
            return true;
        }
        m_pending = target;
        m_hasPending = true;
        return false;
    }

    // Drains the latest deferred target. Sets `has` to whether one was pending.
    int64_t takePending(bool& has) {
        has = m_hasPending;
        m_hasPending = false;
        return m_pending;
    }

    // End the gesture: the next offer() seeks immediately again.
    void reset() {
        m_armed = false;
        m_hasPending = false;
    }

private:
    bool m_armed = false;
    bool m_hasPending = false;
    int64_t m_pending = 0;
};

#endif // SEEKCOALESCER_H

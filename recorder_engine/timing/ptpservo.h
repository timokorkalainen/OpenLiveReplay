#ifndef PTPSERVO_H
#define PTPSERVO_H
#include "iptpclient.h"
#include <cstdint>
#include <vector>

// Pure IEEE 1588 offset/path-delay discipline. No Qt/sockets. Per exchange:
//   meanPathDelay = ((t2 - t1) + (t4 - t3)) / 2
//   offset        = (t2 - t1) - meanPathDelay     (local - master)
// The raw per-exchange offset is noisy (network jitter), so the servo smooths it
// with a running mean over the last minExchanges samples and rejects gross
// outliers (a single spike does not jerk the estimate or break lock). locked()
// only becomes true after minExchanges consistent samples have converged.
// masterNsFromLocal(localNs) converts a local monotonic ns to estimated master/
// facility ns. Conceptually a DriftEstimator-style windowed estimate + locked().
class PtpServo {
public:
    explicit PtpServo(int minExchanges = 8);
    void observe(const PtpExchange& ex);
    bool locked() const;
    int64_t offsetNs() const;                         // local - master (0 until locked)
    int64_t meanPathDelayNs() const;                  // 0 until locked
    int64_t masterNsFromLocal(int64_t localNs) const; // localNs - offsetNs
    void reset();

private:
    // Reject a raw sample whose offset is absurdly far from the running estimate
    // once we have a few samples to compare against (PTP jitter is bounded; a
    // packet-delay spike is not). A lone spike is dropped, BUT a genuine step in the
    // true offset (grandmaster failover, leap second, coarse re-lock) arrives as a
    // RUN of consistent off-estimate samples: after kStepReAdmitCount consecutive
    // rejections the servo treats the new level as real and re-arms onto it, so a
    // large step can never lock the estimate out forever.
    static constexpr int64_t kOutlierGuardNs = 2'000'000; // 2 ms beyond the spread

    void pushSample(int64_t offsetNs, int64_t pathDelayNs);
    void recompute() const;

    int m_minExchanges = 8;
    int m_count = 0; // total accepted samples (monotonic; not the window size)

    struct Sample {
        int64_t offsetNs = 0;
        int64_t pathDelayNs = 0;
    };
    std::vector<Sample> m_window; // last minExchanges accepted samples (ring)
    size_t m_nextIndex = 0;

    mutable bool m_dirty = true;
    mutable int64_t m_offsetNs = 0;
    mutable int64_t m_pathDelayNs = 0;
    bool m_locked = false;
    // Consecutive outlier-rejected samples. Once this reaches the window size, the
    // "outliers" are a sustained step, not jitter -> re-arm onto the new level.
    int m_consecutiveRejects = 0;
};
#endif // PTPSERVO_H

#ifndef DRIFTESTIMATOR_H
#define DRIFTESTIMATOR_H

#include <cstdint>
#include <vector>

class DriftEstimator {
public:
    explicit DriftEstimator(int windowSize = 256, int minSamples = 64);

    void addSample(int64_t senderNs, int64_t sessionNs);
    bool locked() const { return int(m_samples.size()) >= m_minSamples; }
    double ppm() const;
    int64_t offsetNs() const;
    void reset();

private:
    struct Sample {
        int64_t senderNs = 0;
        int64_t sessionNs = 0;
    };

    void recompute() const;

    int m_windowSize = 256;
    int m_minSamples = 64;
    std::vector<Sample> m_samples;
    size_t m_nextIndex = 0;
    mutable bool m_dirty = true;
    mutable double m_slope = 1.0;
    mutable int64_t m_offsetNs = 0;
};

#endif // DRIFTESTIMATOR_H

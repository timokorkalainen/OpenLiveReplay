#include "driftestimator.h"

#include <algorithm>
#include <cmath>

DriftEstimator::DriftEstimator(int windowSize, int minSamples)
    : m_windowSize(std::max(2, windowSize))
    , m_minSamples(std::max(2, std::min(minSamples, std::max(2, windowSize)))) {}

void DriftEstimator::addSample(int64_t senderNs, int64_t sessionNs) {
    const Sample sample{senderNs, sessionNs};
    if (int(m_samples.size()) < m_windowSize) {
        m_samples.push_back(sample);
    } else {
        m_samples[m_nextIndex] = sample;
        m_nextIndex = (m_nextIndex + 1) % m_samples.size();
    }
    m_dirty = true;
}

double DriftEstimator::ppm() const {
    if (!locked()) {
        return 0.0;
    }
    recompute();
    return (m_slope - 1.0) * 1000000.0;
}

int64_t DriftEstimator::offsetNs() const {
    if (!locked()) {
        return 0;
    }
    recompute();
    return m_offsetNs;
}

void DriftEstimator::reset() {
    m_samples.clear();
    m_nextIndex = 0;
    m_dirty = true;
    m_slope = 1.0;
    m_offsetNs = 0;
}

void DriftEstimator::recompute() const {
    if (!m_dirty) {
        return;
    }
    m_dirty = false;
    m_slope = 1.0;
    m_offsetNs = 0;

    const int n = int(m_samples.size());
    if (n < m_minSamples) {
        return;
    }

    const double baseSender = double(m_samples.front().senderNs);
    const double baseSession = double(m_samples.front().sessionNs);
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXX = 0.0;
    double sumXY = 0.0;
    double meanSenderAbs = 0.0;
    double meanSessionAbs = 0.0;

    for (const Sample& sample : m_samples) {
        const double x = double(sample.sessionNs) - baseSession;
        const double y = double(sample.senderNs) - baseSender;
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        meanSenderAbs += double(sample.senderNs);
        meanSessionAbs += double(sample.sessionNs);
    }

    const double denom = double(n) * sumXX - sumX * sumX;
    if (std::abs(denom) > 0.0) {
        m_slope = (double(n) * sumXY - sumX * sumY) / denom;
    }
    meanSenderAbs /= double(n);
    meanSessionAbs /= double(n);
    m_offsetNs = int64_t(std::llround(meanSessionAbs - meanSenderAbs));
}

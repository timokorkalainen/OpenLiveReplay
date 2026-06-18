#include "tests/e2e/ndi_recv_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

NdiContinuity ndiAnalyzeContinuity(const std::vector<qint64>& decodedIndices) {
    NdiContinuity out;
    out.framesReceived = qint64(decodedIndices.size());
    for (size_t i = 1; i < decodedIndices.size(); ++i) {
        const qint64 a = decodedIndices[i - 1];
        const qint64 b = decodedIndices[i];
        if (b == a) {
            out.dupes++;
        } else if (b < a) {
            out.reorders++;
        } else if (b > a + 1) {
            out.drops += (b - a - 1);
        }
    }
    return out;
}

int ndiAvSyncMaxFrames(const std::vector<qint64>& videoFlashIndices,
                       const std::vector<qint64>& audioBeepFrameIndices) {
    if (videoFlashIndices.empty() || audioBeepFrameIndices.empty()) return -1;
    // Pair flash[i] with beep[i] (index-based) and compute the signed A/V offset for each pair.
    // This measures JITTER rather than the absolute NDI buffer delay (which is constant and
    // irrelevant to A/V sync quality). Truncate to the shorter list.
    const size_t n = std::min(videoFlashIndices.size(), audioBeepFrameIndices.size());
    if (n == 0) return -1;
    std::vector<qint64> offsets;
    offsets.reserve(n);
    for (size_t i = 0; i < n; ++i)
        offsets.push_back(audioBeepFrameIndices[i] - videoFlashIndices[i]);
    // Median offset = steady-state NDI audio buffer delay.
    std::vector<qint64> sorted = offsets;
    std::sort(sorted.begin(), sorted.end());
    const qint64 median = sorted[n / 2];
    // Max jitter = max deviation from the median offset.
    int worst = 0;
    for (const qint64 off : offsets)
        worst = std::max(worst, int(std::llabs(off - median)));
    return worst;
}

NdiCadence ndiAnalyzeCadence(const std::vector<double>& arrivalSeconds, int fpsNum, int fpsDen) {
    NdiCadence out;
    if (arrivalSeconds.size() < 2 || fpsNum <= 0) return out;
    const double period = double(fpsDen) / double(fpsNum);
    double maxGap = 0.0;
    for (size_t i = 1; i < arrivalSeconds.size(); ++i)
        maxGap = std::max(maxGap, arrivalSeconds[i] - arrivalSeconds[i - 1]);
    out.maxGapFrames = int(std::lround(maxGap / period));
    const double span = arrivalSeconds.back() - arrivalSeconds.front();
    out.meanRateHz = span > 0.0 ? double(arrivalSeconds.size() - 1) / span : 0.0;
    return out;
}

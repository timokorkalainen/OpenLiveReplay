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
    int worst = 0;
    for (const qint64 v : videoFlashIndices) {
        qint64 best = std::numeric_limits<qint64>::max();
        for (const qint64 b : audioBeepFrameIndices)
            best = std::min(best, std::llabs(v - b));
        worst = std::max(worst, int(best));
    }
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

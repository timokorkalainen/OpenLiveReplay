#ifndef OLR_NDI_RECV_ANALYSIS_H
#define OLR_NDI_RECV_ANALYSIS_H

#include <QtGlobal>

#include <vector>

struct NdiContinuity {
    qint64 framesReceived = 0;
    qint64 drops = 0;
    qint64 dupes = 0;
    qint64 reorders = 0;
};

// Counts, over the decoded frame-index sequence: missing indices (drops), repeated indices
// (dupes), and out-of-order steps (reorders). A step from a to b: b==a -> dupe; b<a -> reorder;
// b>a+1 -> (b-a-1) drops; b==a+1 -> clean.
NdiContinuity ndiAnalyzeContinuity(const std::vector<qint64>& decodedIndices);

// Max over each video flash of the distance to its nearest audio-beep frame index. −1 if
// either list is empty.
int ndiAvSyncMaxFrames(const std::vector<qint64>& videoFlashIndices,
                       const std::vector<qint64>& audioBeepFrameIndices);

struct NdiCadence {
    int maxGapFrames = 0;
    double meanRateHz = 0.0;
};

// From video arrival timestamps (seconds), the largest inter-arrival gap expressed in frame
// periods (rounded) and the mean delivery rate. Fewer than two arrivals -> {0, 0}.
NdiCadence ndiAnalyzeCadence(const std::vector<double>& arrivalSeconds, int fpsNum, int fpsDen);

#endif // OLR_NDI_RECV_ANALYSIS_H

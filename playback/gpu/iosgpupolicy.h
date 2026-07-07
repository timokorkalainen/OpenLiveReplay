#ifndef OLR_IOSGPUPOLICY_H
#define OLR_IOSGPUPOLICY_H

// iOS GPU-pipeline policy. Platform-neutral: no UIKit/Metal/CoreVideo types.
// The iOS per-track cap is derived by PlaybackWorker from the active residency
// window instead of pinned to a fixed small constant.

bool gpuIsIosBuild();
int gpuPerTrackWindowCap(int trackCount, int derivedPerTrackCap = 0);

#endif // OLR_IOSGPUPOLICY_H

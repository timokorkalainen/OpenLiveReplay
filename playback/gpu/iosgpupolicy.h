#ifndef OLR_IOSGPUPOLICY_H
#define OLR_IOSGPUPOLICY_H

// iOS GPU-pipeline policy. Platform-neutral: no UIKit/Metal/CoreVideo types.
// iOS uses a tighter GPU decode-window budget than macOS to bound thermal and
// VRAM pressure under multiview playback.
constexpr int kIosMaxPerTrackGpuFrames = 8;
constexpr int kIosAggregateGpuFrameCeiling = 48;

bool gpuIsIosBuild();
int gpuPerTrackWindowCap(int trackCount);
int gpuIosAggregateWindowCeiling();

#endif // OLR_IOSGPUPOLICY_H

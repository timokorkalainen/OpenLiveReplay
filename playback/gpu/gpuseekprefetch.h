#ifndef OLR_GPUSEEKPREFETCH_H
#define OLR_GPUSEEKPREFETCH_H

#include <QtGlobal>

#include <cstdint>

struct GpuPrefetchPlan {
    int64_t startMs = -1;
    int64_t endMs = -1;
    int surfaceCount = 0;
};

class GpuSeekPrefetch {
public:
    static GpuPrefetchPlan planPrefetch(int64_t targetMs, int dir, int64_t frameDurMs,
                                        int64_t leadMs, qint64 surfaceBytes, qint64 budgetBytes,
                                        qint64 liveBytes);
};

#endif // OLR_GPUSEEKPREFETCH_H

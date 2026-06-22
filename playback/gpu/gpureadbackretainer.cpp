#include "playback/gpu/gpureadbackretainer.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <QMutex>
#include <QMutexLocker>
#include <QVector>

#include <utility>

namespace {

struct ReadbackRetain {
    std::shared_ptr<GpuSurface> surface;
    std::shared_ptr<GpuFence> fence;
    uint64_t fenceValue = 0;
};

QMutex& readbackRetainMutex() {
    static QMutex mutex;
    return mutex;
}

QVector<ReadbackRetain>& readbackRetains() {
    static QVector<ReadbackRetain> retains;
    return retains;
}

void drainCompletedReadbackRetainsLocked() {
    auto& retains = readbackRetains();
    for (qsizetype i = retains.size() - 1; i >= 0; --i) {
        const ReadbackRetain& retain = retains.at(i);
        if (!retain.surface || !retain.fence || retain.fenceValue == 0 ||
            retain.fence->completedValue() >= retain.fenceValue) {
            retains.removeAt(i);
        }
    }
}

} // namespace

void gpuRetainSurfaceUntilFenceRetired(std::shared_ptr<GpuSurface> surface,
                                       std::shared_ptr<GpuFence> fence, uint64_t fenceValue) {
    if (!surface || !fence || fenceValue == 0) return;

    surface->retainUntilFenceRetired(fenceValue);
    QMutexLocker locker(&readbackRetainMutex());
    drainCompletedReadbackRetainsLocked();
    readbackRetains().append(ReadbackRetain{std::move(surface), std::move(fence), fenceValue});
    drainCompletedReadbackRetainsLocked();
}

void gpuDrainCompletedReadbackRetains() {
    QMutexLocker locker(&readbackRetainMutex());
    drainCompletedReadbackRetainsLocked();
}

qsizetype gpuPendingReadbackRetainCount() {
    QMutexLocker locker(&readbackRetainMutex());
    drainCompletedReadbackRetainsLocked();
    return readbackRetains().size();
}

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

qsizetype gpuAbandonAllReadbackRetains(const DeadDeviceToken& deadDevice) {
    (void) deadDevice; // presence is the compile-time proof the device is truly dead
    QMutexLocker locker(&readbackRetainMutex());
    const qsizetype dropped = readbackRetains().size();
    readbackRetains().clear();
    return dropped;
}

int gpuDrainReadbackRetainsWithBoundedWait(int perFenceTimeoutMs) {
    QMutexLocker locker(&readbackRetainMutex());
    auto& retains = readbackRetains();
    QVector<ReadbackRetain> pending;
    int released = 0;
    for (ReadbackRetain& retain : retains) {
        bool retired = !retain.surface || !retain.fence || retain.fenceValue == 0 ||
                       retain.fence->completedValue() >= retain.fenceValue;
        if (!retired) retired = retain.fence->wait(retain.fenceValue, perFenceTimeoutMs);
        if (retired)
            ++released;
        else
            pending.append(std::move(retain));
    }
    retains = std::move(pending);
    return released;
}

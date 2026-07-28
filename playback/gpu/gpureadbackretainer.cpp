#include "playback/gpu/gpureadbackretainer.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <utility>

namespace {

struct ReadbackRetain {
    uint64_t id = 0;
    std::shared_ptr<GpuSurface> surface;
    std::shared_ptr<GpuFence> fence;
    uint64_t fenceValue = 0;
};

struct RetainerMetrics {
    qsizetype highWaterMark = 0;
    uint64_t timeoutCount = 0;
    uint64_t signalFailureCount = 0;
};

QMutex& readbackRetainMutex() {
    static QMutex mutex;
    return mutex;
}

QVector<ReadbackRetain>& readbackRetains() {
    static QVector<ReadbackRetain> retains;
    return retains;
}

RetainerMetrics& retainerMetrics() {
    static RetainerMetrics metrics;
    return metrics;
}

uint64_t& nextReadbackRetainId() {
    static uint64_t id = 1;
    return id;
}

} // namespace

namespace gpuRetireDetail {

void registerRetire(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuFence> fence,
                    uint64_t fenceValue) {
    registerRetireBatch(&surface, 1, fence, fenceValue);
}

void registerRetireBatch(std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
                         const std::shared_ptr<GpuFence>& fence, uint64_t fenceValue) {
    if (!surfaces || count <= 0 || !fence || fenceValue == 0) return;
    for (qsizetype i = 0; i < count; ++i) {
        if (surfaces[i]) surfaces[i]->retainUntilFenceRetired(fenceValue);
    }
    // Preserve immediate release for an already-completed operation, but query
    // the driver once per batch and before taking the process-wide registry lock.
    if (fence->completedValue() >= fenceValue) return;

    QMutexLocker locker(&readbackRetainMutex());
    auto& retains = readbackRetains();
    retains.reserve(retains.size() + count);
    for (qsizetype i = 0; i < count; ++i) {
        if (!surfaces[i]) continue;
        retains.append(
            ReadbackRetain{nextReadbackRetainId()++, std::move(surfaces[i]), fence, fenceValue});
    }
    retainerMetrics().highWaterMark = std::max(retainerMetrics().highWaterMark, retains.size());
}

void drainCompleted() {
    QVector<ReadbackRetain> snapshot;
    {
        QMutexLocker locker(&readbackRetainMutex());
        snapshot = readbackRetains();
    }

    QSet<uint64_t> completedIds;
    completedIds.reserve(snapshot.size());
    for (const ReadbackRetain& retain : snapshot) {
        if (!retain.surface || !retain.fence || retain.fenceValue == 0 ||
            retain.fence->completedValue() >= retain.fenceValue) {
            completedIds.insert(retain.id);
        }
    }
    if (completedIds.isEmpty()) return;

    QMutexLocker locker(&readbackRetainMutex());
    auto& retains = readbackRetains();
    for (qsizetype i = retains.size() - 1; i >= 0; --i) {
        if (completedIds.contains(retains.at(i).id)) retains.removeAt(i);
    }
}

qsizetype pendingCount() {
    drainCompleted();
    QMutexLocker locker(&readbackRetainMutex());
    return readbackRetains().size();
}

qsizetype abandonAllNoWait(const DeadDeviceToken& deadDevice) {
    (void) deadDevice;
    QMutexLocker locker(&readbackRetainMutex());
    const qsizetype dropped = readbackRetains().size();
    readbackRetains().clear();
    return dropped;
}

int drainWithBoundedWait(int perFenceTimeoutMs) {
    QVector<ReadbackRetain> snapshot;
    {
        QMutexLocker locker(&readbackRetainMutex());
        snapshot = readbackRetains();
    }

    QSet<uint64_t> retiredIds;
    retiredIds.reserve(snapshot.size());
    uint64_t timedOut = 0;
    for (const ReadbackRetain& retain : snapshot) {
        bool retired = !retain.surface || !retain.fence || retain.fenceValue == 0 ||
                       retain.fence->completedValue() >= retain.fenceValue;
        if (!retired) retired = retain.fence->wait(retain.fenceValue, perFenceTimeoutMs);
        if (retired)
            retiredIds.insert(retain.id);
        else
            ++timedOut;
    }

    int released = 0;
    QMutexLocker locker(&readbackRetainMutex());
    retainerMetrics().timeoutCount += timedOut;
    auto& retains = readbackRetains();
    for (qsizetype i = retains.size() - 1; i >= 0; --i) {
        if (!retiredIds.contains(retains.at(i).id)) {
            continue;
        }
        retains.removeAt(i);
        ++released;
    }
    return released;
}

qsizetype highWaterMark() {
    QMutexLocker locker(&readbackRetainMutex());
    return retainerMetrics().highWaterMark;
}

uint64_t timeoutCount() {
    QMutexLocker locker(&readbackRetainMutex());
    return retainerMetrics().timeoutCount;
}

uint64_t signalFailureCount() {
    QMutexLocker locker(&readbackRetainMutex());
    return retainerMetrics().signalFailureCount;
}

void noteSignalFailure() {
    QMutexLocker locker(&readbackRetainMutex());
    ++retainerMetrics().signalFailureCount;
}

#ifdef OLR_UNIT_TEST
bool mutexAvailableForTest() {
    if (!readbackRetainMutex().tryLock()) return false;
    readbackRetainMutex().unlock();
    return true;
}
#endif

} // namespace gpuRetireDetail

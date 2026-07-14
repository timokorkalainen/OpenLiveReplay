#include "playback/gpu/gpureadbackretainer.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QVector>
#include <QElapsedTimer>

#include <algorithm>
#include <utility>

namespace {

struct ReadbackRetain {
    uint64_t id = 0;
    std::shared_ptr<GpuSurface> surface;
    std::shared_ptr<GpuFence> fence;
    uint64_t fenceValue = 0;
    uintptr_t deviceDomainId = 0;
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

void GpuReadbackRetainer::registerRetire(std::shared_ptr<GpuSurface> surface,
                                         GpuRetirementTicket ticket) {
    const std::shared_ptr<GpuFence> fence = ticket.fence();
    const uint64_t fenceValue = ticket.value();
    if (!surface || !fence || fenceValue == 0) return;
    try {
        if (!fence->validatesRetirement(ticket, surface->compatibility())) return;
    } catch (...) {
        return;
    }
    surface->retainUntilFenceRetired(fenceValue);
    // Preserve immediate release for an already-completed operation, but query
    // the driver once per batch and before taking the process-wide registry lock.
    if (fence->completedValue() >= fenceValue) return;

    QMutexLocker locker(&readbackRetainMutex());
    auto& retains = readbackRetains();
    retains.reserve(retains.size() + 1);
    retains.append(ReadbackRetain{nextReadbackRetainId()++, std::move(surface), fence, fenceValue,
                                  fence->deviceDomainId()});
    retainerMetrics().highWaterMark = std::max(retainerMetrics().highWaterMark, retains.size());
}

void GpuReadbackRetainer::drainCompleted() {
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

qsizetype GpuReadbackRetainer::pendingCount() {
    drainCompleted();
    QMutexLocker locker(&readbackRetainMutex());
    return readbackRetains().size();
}

qsizetype GpuReadbackRetainer::abandonAllNoWait(const DeadDeviceToken& deadDevice) {
    return GpuReadbackRetainer::abandonAllNoWait(std::vector<DeadDeviceToken>{deadDevice});
}

qsizetype GpuReadbackRetainer::abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices) {
    if (deadDevices.empty()) return 0;
    QSet<quintptr> deadDomains;
    deadDomains.reserve(qsizetype(deadDevices.size()));
    for (const DeadDeviceToken& token : deadDevices)
        deadDomains.insert(quintptr(token.deviceDomainId()));

    QMutexLocker locker(&readbackRetainMutex());
    auto& retains = readbackRetains();
    qsizetype dropped = 0;
    for (qsizetype i = retains.size() - 1; i >= 0; --i) {
        if (!deadDomains.contains(quintptr(retains.at(i).deviceDomainId))) continue;
        retains.removeAt(i);
        ++dropped;
    }
    return dropped;
}

int GpuReadbackRetainer::drainWithBoundedWait(int totalTimeoutMs) {
    QVector<ReadbackRetain> snapshot;
    {
        QMutexLocker locker(&readbackRetainMutex());
        snapshot = readbackRetains();
    }

    QSet<uint64_t> retiredIds;
    retiredIds.reserve(snapshot.size());
    uint64_t timedOut = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    for (qsizetype i = 0; i < snapshot.size(); ++i) {
        const ReadbackRetain& retain = snapshot.at(i);
        bool retired = !retain.surface || !retain.fence || retain.fenceValue == 0;
        int remainingMs = qMax(0, totalTimeoutMs - int(elapsed.elapsed()));
        if (!retired && remainingMs <= 0) {
            timedOut += uint64_t(snapshot.size() - i);
            break;
        }
        if (!retired) retired = retain.fence->completedValue() >= retain.fenceValue;
        remainingMs = qMax(0, totalTimeoutMs - int(elapsed.elapsed()));
        if (!retired && remainingMs > 0)
            retired = retain.fence->wait(retain.fenceValue, remainingMs);
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

qsizetype GpuReadbackRetainer::highWaterMark() {
    QMutexLocker locker(&readbackRetainMutex());
    return retainerMetrics().highWaterMark;
}

uint64_t GpuReadbackRetainer::timeoutCount() {
    QMutexLocker locker(&readbackRetainMutex());
    return retainerMetrics().timeoutCount;
}

uint64_t GpuReadbackRetainer::signalFailureCount() {
    QMutexLocker locker(&readbackRetainMutex());
    return retainerMetrics().signalFailureCount;
}

void GpuReadbackRetainer::noteSignalFailure() {
    QMutexLocker locker(&readbackRetainMutex());
    ++retainerMetrics().signalFailureCount;
}

#ifdef OLR_UNIT_TEST
namespace gpuRetireDetail {
bool mutexAvailableForTest() {
    if (!readbackRetainMutex().tryLock()) return false;
    readbackRetainMutex().unlock();
    return true;
}
} // namespace gpuRetireDetail
#endif

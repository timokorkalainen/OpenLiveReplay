#include "playback/gpu/gpupipelineconfig.h"

#include <QByteArray>
#include <QtGlobal>

#include <atomic>

namespace {
std::atomic<int> g_injectedAllocFailures{0};
} // namespace

bool gpuPipelineEnabled() {
    const QByteArray v = qgetenv("OLR_GPU_PIPELINE").toLower();
    return v == "1" || v == "true" || v == "on";
}

int gpuForcedPerTrackBudget() {
    const QByteArray v = qgetenv("OLR_GPU_FORCE_BUDGET");
    if (v.isEmpty()) return -1;

    bool ok = false;
    const int n = v.toInt(&ok);
    return ok ? n : -1;
}

bool gpuConsumeInjectedAllocFailure() {
    int cur = g_injectedAllocFailures.load(std::memory_order_acquire);
    while (cur > 0) {
        if (g_injectedAllocFailures.compare_exchange_weak(cur, cur - 1,
                                                          std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

void gpuSetInjectedAllocFailures(int count) {
    g_injectedAllocFailures.store(count < 0 ? 0 : count, std::memory_order_release);
}

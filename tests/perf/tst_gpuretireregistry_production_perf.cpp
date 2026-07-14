#include <QtTest>

#include "saved912gpuopscope.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framepixelformat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <vector>

#ifdef OLR_UNIT_TEST
#error "Production retirement benchmark must not compile unit-test probes"
#endif

namespace {

class ProductionPerfSurface final : public GpuSurface {
public:
    ProductionPerfSurface(void* handle, GpuSurfaceCompatibility compatibility)
        : m_handle(handle), m_compatibility(compatibility) {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16, 0}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }

protected:
    void* nativeHandle() const override { return m_handle; }

private:
    void* m_handle = nullptr;
    GpuSurfaceCompatibility m_compatibility;
};

class ProductionPerfFence final : public GpuFence {
public:
    ProductionPerfFence(uintptr_t domain, uint64_t authority) : GpuFence(domain, authority) {}
    uint64_t signal() override { return ++m_signaled; }
    bool wait(uint64_t value, int) override { return m_completed >= value; }
    uint64_t completedValue() const override { return m_completed; }
    void complete() noexcept { m_completed = m_signaled; }

    template <typename SubmitFn>
    std::optional<GpuRetirementTicket>
    submitForBaseline(const GpuFenceIdentity& preparedFence,
                      const GpuSurfaceCompatibility& compatibility, uint64_t generation,
                      SubmitFn&& submitFn) {
        return submitExactForRetirement(preparedFence, compatibility, generation,
                                        std::forward<SubmitFn>(submitFn));
    }

private:
    uint64_t m_signaled = 0;
    uint64_t m_completed = 0;
};

struct ProductionPerfAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::Submitted; }
};

qint64 elapsedNanoseconds(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                started)
        .count();
}

qint64 percentile(std::vector<qint64> samples, size_t numerator, size_t denominator) {
    std::sort(samples.begin(), samples.end());
    return samples[(samples.size() * numerator) / denominator];
}

struct RoundMetrics {
    qint64 registrationMedian = 0;
    qint64 registrationP95 = 0;
    qint64 drainMedian = 0;
    qint64 drainP95 = 0;
};

template <typename Registry, typename OpScope>
RoundMetrics
measureRound(const std::array<std::array<std::shared_ptr<GpuSurface>, 4>, 256>& fixtures) {
    constexpr int sampleCount = 200;
    constexpr uintptr_t domain = 0x740;
    constexpr uint64_t authority = 53;
    std::vector<qint64> registration;
    std::vector<qint64> drain;
    registration.reserve(sampleCount);
    drain.reserve(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
        Registry registry;
        auto fence = std::make_shared<ProductionPerfFence>(domain, authority);
        bool publishedAll = true;
        const auto registrationStarted = std::chrono::steady_clock::now();
        for (const auto& owners : fixtures) {
            ProductionPerfAdapter adapter;
            OpScope operation(fence, registry);
            const auto result = operation.submit(adapter, GpuSurfacePack<4>(owners));
            publishedAll = publishedAll && result.retirement == GpuRetirementDisposition::Published;
        }
        registration.push_back(elapsedNanoseconds(registrationStarted));
        if (!publishedAll) return {};
        fence->complete();
        const auto drainStarted = std::chrono::steady_clock::now();
        registry.drainCompleted();
        drain.push_back(elapsedNanoseconds(drainStarted));
    }
    return RoundMetrics{percentile(registration, 1, 2), percentile(registration, 95, 100),
                        percentile(drain, 1, 2), percentile(drain, 95, 100)};
}

} // namespace

class TestGpuRetireRegistryProductionPerf final : public QObject {
    Q_OBJECT

private slots:
    void preparedSlotsFourOwners();
};

void TestGpuRetireRegistryProductionPerf::preparedSlotsFourOwners() {
    constexpr uintptr_t domain = 0x740;
    constexpr uint64_t authority = 53;
    constexpr int operationCount = 256;
    constexpr int ownerCount = 4;
    constexpr int roundCount = 10;
    std::array<std::array<std::shared_ptr<GpuSurface>, ownerCount>, operationCount> fixtures;
    for (int operation = 0; operation < operationCount; ++operation) {
        for (int owner = 0; owner < ownerCount; ++owner) {
            fixtures[size_t(operation)][size_t(owner)] = std::make_shared<ProductionPerfSurface>(
                reinterpret_cast<void*>(uintptr_t(0x10000 + operation * ownerCount + owner)),
                GpuSurfaceCompatibility{domain, authority});
        }
    }

    std::vector<qint64> currentRegistrationMedians;
    std::vector<qint64> currentRegistrationP95s;
    std::vector<qint64> currentDrainMedians;
    std::vector<qint64> currentDrainP95s;
    std::vector<qint64> baselineRegistrationMedians;
    std::vector<qint64> baselineRegistrationP95s;
    std::vector<qint64> baselineDrainMedians;
    std::vector<qint64> baselineDrainP95s;
    std::vector<qint64> registrationMedianRatios;
    std::vector<qint64> registrationP95Ratios;
    std::vector<qint64> drainMedianRatios;
    std::vector<qint64> drainP95Ratios;
    for (int round = 0; round < roundCount; ++round) {
        RoundMetrics current;
        RoundMetrics baseline;
        if ((round & 1) == 0) {
            baseline =
                measureRound<Saved912PreparedRegistry, Saved912GpuOpScope<ProductionPerfFence>>(
                    fixtures);
            current = measureRound<GpuRetireRegistry, GpuOpScope>(fixtures);
        } else {
            current = measureRound<GpuRetireRegistry, GpuOpScope>(fixtures);
            baseline =
                measureRound<Saved912PreparedRegistry, Saved912GpuOpScope<ProductionPerfFence>>(
                    fixtures);
        }
        QVERIFY(current.registrationMedian > 0);
        QVERIFY(baseline.registrationMedian > 0);
        currentRegistrationMedians.push_back(current.registrationMedian);
        currentRegistrationP95s.push_back(current.registrationP95);
        currentDrainMedians.push_back(current.drainMedian);
        currentDrainP95s.push_back(current.drainP95);
        baselineRegistrationMedians.push_back(baseline.registrationMedian);
        baselineRegistrationP95s.push_back(baseline.registrationP95);
        baselineDrainMedians.push_back(baseline.drainMedian);
        baselineDrainP95s.push_back(baseline.drainP95);
        registrationMedianRatios.push_back(current.registrationMedian * 10000 /
                                           baseline.registrationMedian);
        registrationP95Ratios.push_back(current.registrationP95 * 10000 / baseline.registrationP95);
        drainMedianRatios.push_back(current.drainMedian * 10000 / baseline.drainMedian);
        drainP95Ratios.push_back(current.drainP95 * 10000 / baseline.drainP95);
    }

    const qint64 registrationMedian = percentile(currentRegistrationMedians, 1, 2);
    const qint64 registrationP95 = percentile(currentRegistrationP95s, 1, 2);
    const qint64 drainMedian = percentile(currentDrainMedians, 1, 2);
    const qint64 drainP95 = percentile(currentDrainP95s, 1, 2);
    const qint64 saved912RegistrationMedian = percentile(baselineRegistrationMedians, 1, 2);
    const qint64 saved912RegistrationP95 = percentile(baselineRegistrationP95s, 1, 2);
    const qint64 saved912DrainMedian = percentile(baselineDrainMedians, 1, 2);
    const qint64 saved912DrainP95 = percentile(baselineDrainP95s, 1, 2);
    const qint64 registrationMedianRatio = percentile(registrationMedianRatios, 1, 2);
    const qint64 registrationP95Ratio = percentile(registrationP95Ratios, 1, 2);
    const qint64 drainMedianRatio = percentile(drainMedianRatios, 1, 2);
    const qint64 drainP95Ratio = percentile(drainP95Ratios, 1, 2);
    qInfo("Production GPU retire vs exact 912663be: registration current=%lld/%lld "
          "baseline=%lld/%lld ns; drain current=%lld/%lld baseline=%lld/%lld ns "
          "(median of 10 round median/p95 values); paired ratios=%lld/%lld and %lld/%lld bp",
          static_cast<long long>(registrationMedian), static_cast<long long>(registrationP95),
          static_cast<long long>(saved912RegistrationMedian),
          static_cast<long long>(saved912RegistrationP95), static_cast<long long>(drainMedian),
          static_cast<long long>(drainP95), static_cast<long long>(saved912DrainMedian),
          static_cast<long long>(saved912DrainP95), static_cast<long long>(registrationMedianRatio),
          static_cast<long long>(registrationP95Ratio), static_cast<long long>(drainMedianRatio),
          static_cast<long long>(drainP95Ratio));
#ifdef NDEBUG
    QVERIFY2(registrationMedianRatio <= 10200,
             "production registration median regressed by more than 2% against exact 912663be");
    QVERIFY2(registrationP95Ratio <= 10200,
             "production registration p95 regressed by more than 2% against exact 912663be");
    QVERIFY2(drainMedianRatio <= 10200,
             "production drain median regressed by more than 2% against exact 912663be");
    QVERIFY2(drainP95Ratio <= 10200,
             "production drain p95 regressed by more than 2% against exact 912663be");
#endif
}

QTEST_GUILESS_MAIN(TestGpuRetireRegistryProductionPerf)
#include "tst_gpuretireregistry_production_perf.moc"

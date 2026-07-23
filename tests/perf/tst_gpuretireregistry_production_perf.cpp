#include <QtTest>

#include <QDir>
#include <QFile>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framepixelformat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

template <size_t N>
class Saved912SurfacePack final {
public:
    explicit Saved912SurfacePack(std::array<std::shared_ptr<GpuSurface>, N> owners) noexcept
        : m_owners(std::move(owners)) {}
    const std::array<std::shared_ptr<GpuSurface>, N>& owners() const noexcept { return m_owners; }

private:
    std::array<std::shared_ptr<GpuSurface>, N> m_owners;
};

static_assert(sizeof(GpuSurfaceCompatibility) == 16,
              "surface compatibility must retain the exact saved-912 layout");

#define GpuSurfacePack Saved912SurfacePack
#include "saved912gpuopscope.h"
#undef GpuSurfacePack

#ifdef OLR_UNIT_TEST
#error "Production retirement benchmark must not compile unit-test probes"
#endif

namespace {

class ScopedBenchmarkThreadAffinity final {
public:
    ScopedBenchmarkThreadAffinity() noexcept {
#ifdef _WIN32
        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) ||
            processMask == 0)
            return;
        const DWORD currentProcessor = GetCurrentProcessorNumber();
        const DWORD_PTR currentMask =
            currentProcessor < sizeof(DWORD_PTR) * 8 ? (DWORD_PTR(1) << currentProcessor) : 0;
        const DWORD_PTR selected =
            (currentMask & processMask) != 0 ? currentMask : processMask & (~processMask + 1);
        m_previousMask = SetThreadAffinityMask(GetCurrentThread(), selected);
#endif
    }
    ~ScopedBenchmarkThreadAffinity() {
#ifdef _WIN32
        if (m_previousMask != 0) (void) SetThreadAffinityMask(GetCurrentThread(), m_previousMask);
#endif
    }

    ScopedBenchmarkThreadAffinity(const ScopedBenchmarkThreadAffinity&) = delete;
    ScopedBenchmarkThreadAffinity& operator=(const ScopedBenchmarkThreadAffinity&) = delete;

    bool pinned() const noexcept {
#ifdef _WIN32
        return m_previousMask != 0;
#else
        return true;
#endif
    }

private:
#ifdef _WIN32
    DWORD_PTR m_previousMask = 0;
#endif
};

void emitMachineMetrics(const QString& fileName, const QString& json) {
    qInfo().noquote() << QStringLiteral("OLR_GPU_PERF_JSON=") + json;
    const QString evidenceDir = qEnvironmentVariable("OLR_GPU_PERF_EVIDENCE_DIR");
    if (evidenceDir.isEmpty()) return;
    QVERIFY2(QDir().mkpath(evidenceDir), "could not create GPU performance evidence directory");
    QFile output(QDir(evidenceDir).filePath(fileName));
    QVERIFY2(output.open(QIODevice::WriteOnly | QIODevice::Truncate),
             "could not open GPU performance evidence file");
    const QByteArray encoded = json.toUtf8();
    QCOMPARE(output.write(encoded), qint64(encoded.size()));
}

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

// Commit 912 acquired native handles through the virtual GpuSurface API. Keep a
// public test-only form of that interface so the frozen side cannot be
// devirtualized into a direct field load merely because the concrete fake is final.
class Saved912PublicHandleSurface : public GpuSurface {
public:
    virtual void* nativeHandle() const override = 0;
};

class HandlePerfSurface : public Saved912PublicHandleSurface {
public:
    HandlePerfSurface(void* handle, GpuSurfaceCompatibility compatibility)
        : m_handle(handle), m_compatibility(compatibility) {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16, 0}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }
    void* nativeHandle() const override { return m_handle; }

private:
    void* m_handle = nullptr;
    GpuSurfaceCompatibility m_compatibility;
};

class AlternateHandlePerfSurface final : public HandlePerfSurface {
public:
    using HandlePerfSurface::HandlePerfSurface;
    void* nativeHandle() const override { return HandlePerfSurface::nativeHandle(); }
};

class ProductionPerfFence final : public GpuFence {
public:
    ProductionPerfFence(uintptr_t domain, uint64_t authority, uint64_t startingValue = 0)
        : GpuFence(domain, authority), m_startingValue(startingValue), m_signaled(startingValue),
          m_completed(startingValue) {}
    uint64_t signal() override { return ++m_signaled; }
    bool wait(uint64_t value, int) override { return m_completed >= value; }
    uint64_t completedValue() const override { return m_completed; }
    void complete() noexcept { m_completed = m_signaled; }
    uint64_t signalCount() const noexcept { return m_signaled - m_startingValue; }

    template <typename SubmitFn>
    std::optional<GpuRetirementTicket>
    submitForBaseline(const GpuFenceIdentity& preparedFence,
                      const GpuSurfaceCompatibility& compatibility, uint64_t generation,
                      SubmitFn&& submitFn) {
        return submitExactForRetirement(preparedFence, compatibility, generation,
                                        std::forward<SubmitFn>(submitFn));
    }

    bool validatesRetirement(const GpuRetirementTicket& ticket,
                             const GpuSurfaceCompatibility& compatibility) const noexcept {
        return GpuFence::validatesRetirement(ticket, compatibility);
    }

private:
    uint64_t m_startingValue = 0;
    uint64_t m_signaled = 0;
    uint64_t m_completed = 0;
};

struct ProductionPerfAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::Submitted; }
};

struct Saved912PerfAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::Submitted; }
};

struct CurrentHandlePerfAdapter {
    uintptr_t* checksum = nullptr;
    GpuSubmitOutcome operator()(const GpuScopedNativeView<4>& view) noexcept {
        for (size_t i = 0; i < view.size(); ++i)
            *checksum += reinterpret_cast<uintptr_t>(view[i].nativeHandle());
        return GpuSubmitOutcome::Submitted;
    }
};

struct Saved912HandlePerfAdapter {
    const std::array<std::shared_ptr<GpuSurface>, 4>* owners = nullptr;
    uintptr_t* checksum = nullptr;
    GpuSubmitOutcome operator()() noexcept {
        for (const auto& owner : *owners) {
            auto* surface = static_cast<Saved912PublicHandleSurface*>(owner.get());
#if defined(__GNUC__) || defined(__clang__)
            // The saved API dispatched through GpuSurface. This zero-instruction compiler
            // barrier prevents the final test fake from being devirtualized into a field
            // load; it does not add work inside the measured baseline.
            __asm__ __volatile__("" : "+r"(surface));
#endif
            *checksum += reinterpret_cast<uintptr_t>(surface->nativeHandle());
        }
        return GpuSubmitOutcome::Submitted;
    }
};

qint64 elapsedNanoseconds(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                started)
        .count();
}

#ifdef _WIN32
constexpr const char* kHandleMeasurementUnit = "thread_cycles";
bool readHandleWorkCounter(qint64& value) noexcept {
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) return false;
    value = qint64(cycles);
    return true;
}
#else
constexpr const char* kHandleMeasurementUnit = "wall_ns";
bool readHandleWorkCounter(qint64& value) noexcept {
    value = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
    return true;
}
#endif

qint64 percentile(std::vector<qint64> samples, size_t numerator, size_t denominator) {
    std::sort(samples.begin(), samples.end());
    return samples[(samples.size() * numerator) / denominator];
}

uint64_t currentDeviceAuthority() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    for (uint64_t authority = 1; authority <= 1024; ++authority) {
        if (monitor.isCurrentDeviceAuthority(authority)) return authority;
    }
    return 0;
}

struct RoundMetrics {
    qint64 registrationMedian = 0;
    qint64 registrationP95 = 0;
    qint64 drainMedian = 0;
    qint64 drainP95 = 0;
    uint64_t signalCalls = 0;
    qsizetype pendingHighWater = 0;
};

template <typename Registry, typename OpScope>
RoundMetrics
measureRound(const std::array<std::array<std::shared_ptr<GpuSurface>, 4>, 256>& fixtures,
             uint64_t authority, uint64_t fenceStart, int sampleCount = 200) {
    constexpr uintptr_t domain = 0x740;
    std::vector<qint64> registration;
    std::vector<qint64> drain;
    registration.reserve(sampleCount);
    drain.reserve(sampleCount);
    uint64_t signalCalls = 0;
    qsizetype pendingHighWater = 0;
    for (int sample = 0; sample < sampleCount; ++sample) {
        Registry registry;
        auto fence = std::make_shared<ProductionPerfFence>(
            domain, authority, fenceStart + uint64_t(sample) * fixtures.size());
        bool publishedAll = true;
        const auto registrationStarted = std::chrono::steady_clock::now();
        for (const auto& owners : fixtures) {
            OpScope operation(fence, registry);
            GpuSubmissionResult result;
            if constexpr (std::is_same_v<Registry, GpuRetireRegistry>) {
                ProductionPerfAdapter adapter;
                result = operation.submitRetained(adapter, GpuSurfacePack<4>(owners));
            } else {
                Saved912PerfAdapter adapter;
                result = operation.submit(adapter, Saved912SurfacePack<4>(owners));
            }
            publishedAll = publishedAll && result.retirement == GpuRetirementDisposition::Published;
        }
        registration.push_back(elapsedNanoseconds(registrationStarted));
        if (!publishedAll) return {};
        signalCalls += fence->signalCount();
        if constexpr (std::is_same_v<Registry, GpuRetireRegistry>)
            pendingHighWater = std::max(pendingHighWater, registry.diagnostics().highWaterMark);
        fence->complete();
        const auto drainStarted = std::chrono::steady_clock::now();
        registry.drainCompleted();
        drain.push_back(elapsedNanoseconds(drainStarted));
    }
    return RoundMetrics{percentile(registration, 1, 2),
                        percentile(registration, 95, 100),
                        percentile(drain, 1, 2),
                        percentile(drain, 95, 100),
                        signalCalls,
                        pendingHighWater};
}

struct PairedRoundMetrics {
    RoundMetrics current;
    RoundMetrics baseline;
};

PairedRoundMetrics
measurePairedRound(const std::array<std::array<std::shared_ptr<GpuSurface>, 4>, 256>& fixtures,
                   uint64_t authority, bool baselineFirst, uint64_t& currentFenceBase) {
    constexpr int sampleCount = 200;
    std::vector<qint64> currentRegistration;
    std::vector<qint64> currentDrain;
    std::vector<qint64> baselineRegistration;
    std::vector<qint64> baselineDrain;
    currentRegistration.reserve(sampleCount);
    currentDrain.reserve(sampleCount);
    baselineRegistration.reserve(sampleCount);
    baselineDrain.reserve(sampleCount);
    PairedRoundMetrics result;
    auto runCurrent = [&] {
        RoundMetrics metrics =
            measureRound<GpuRetireRegistry, GpuOpScope>(fixtures, authority, currentFenceBase, 1);
        currentFenceBase += fixtures.size();
        return metrics;
    };
    auto runBaseline = [&] {
        return measureRound<Saved912PreparedRegistry, Saved912GpuOpScope<ProductionPerfFence>>(
            fixtures, authority, 0, 1);
    };
    for (int group = 0; group < sampleCount; ++group) {
        RoundMetrics currentFirst;
        RoundMetrics currentSecond;
        RoundMetrics baselineFirstMetrics;
        RoundMetrics baselineSecond;
        const bool currentOutside = baselineFirst != ((group & 1) != 0);
        if (currentOutside) { // C-B-B-C
            currentFirst = runCurrent();
            baselineFirstMetrics = runBaseline();
            baselineSecond = runBaseline();
            currentSecond = runCurrent();
        } else { // B-C-C-B
            baselineFirstMetrics = runBaseline();
            currentFirst = runCurrent();
            currentSecond = runCurrent();
            baselineSecond = runBaseline();
        }
        currentRegistration.push_back(
            (currentFirst.registrationMedian + currentSecond.registrationMedian) / 2);
        currentDrain.push_back((currentFirst.drainMedian + currentSecond.drainMedian) / 2);
        baselineRegistration.push_back(
            (baselineFirstMetrics.registrationMedian + baselineSecond.registrationMedian) / 2);
        baselineDrain.push_back((baselineFirstMetrics.drainMedian + baselineSecond.drainMedian) /
                                2);
        result.current.signalCalls += currentFirst.signalCalls + currentSecond.signalCalls;
        result.baseline.signalCalls +=
            baselineFirstMetrics.signalCalls + baselineSecond.signalCalls;
        result.current.pendingHighWater =
            std::max({result.current.pendingHighWater, currentFirst.pendingHighWater,
                      currentSecond.pendingHighWater});
    }
    result.current.registrationMedian = percentile(currentRegistration, 1, 2);
    result.current.registrationP95 = percentile(currentRegistration, 95, 100);
    result.current.drainMedian = percentile(currentDrain, 1, 2);
    result.current.drainP95 = percentile(currentDrain, 95, 100);
    result.baseline.registrationMedian = percentile(baselineRegistration, 1, 2);
    result.baseline.registrationP95 = percentile(baselineRegistration, 95, 100);
    result.baseline.drainMedian = percentile(baselineDrain, 1, 2);
    result.baseline.drainP95 = percentile(baselineDrain, 95, 100);
    return result;
}

} // namespace

class TestGpuRetireRegistryProductionPerf final : public QObject {
    Q_OBJECT

private slots:
    void preparedSlotsFourOwners();
    void scopedViewConsumesExactHandles();
    void scopedViewHandleBindingOverhead();
};

void TestGpuRetireRegistryProductionPerf::preparedSlotsFourOwners() {
    const ScopedBenchmarkThreadAffinity affinity;
    constexpr uintptr_t domain = 0x740;
    const uint64_t authority = currentDeviceAuthority();
    QVERIFY(authority != 0);
    constexpr int operationCount = 256;
    constexpr int ownerCount = 4;
    constexpr int warmupRoundCount = 3;
    constexpr int roundCount = 20;
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
    uint64_t currentFenceBase = 0;
    for (int warmup = 0; warmup < warmupRoundCount; ++warmup) {
        (void) measurePairedRound(fixtures, authority, (warmup & 1) == 0, currentFenceBase);
    }
    uint64_t currentSignals = 0;
    uint64_t baselineSignals = 0;
    qsizetype currentPendingHighWater = 0;
    for (int round = 0; round < roundCount; ++round) {
        const PairedRoundMetrics paired =
            measurePairedRound(fixtures, authority, (round & 1) == 0, currentFenceBase);
        const RoundMetrics& current = paired.current;
        const RoundMetrics& baseline = paired.baseline;
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
        currentSignals += current.signalCalls;
        baselineSignals += baseline.signalCalls;
        currentPendingHighWater = std::max(currentPendingHighWater, current.pendingHighWater);
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
          "(median of %d round median/p95 values); paired ratios=%lld/%lld and %lld/%lld bp",
          static_cast<long long>(registrationMedian), static_cast<long long>(registrationP95),
          static_cast<long long>(saved912RegistrationMedian),
          static_cast<long long>(saved912RegistrationP95), static_cast<long long>(drainMedian),
          static_cast<long long>(drainP95), static_cast<long long>(saved912DrainMedian),
          static_cast<long long>(saved912DrainP95), roundCount,
          static_cast<long long>(registrationMedianRatio),
          static_cast<long long>(registrationP95Ratio), static_cast<long long>(drainMedianRatio),
          static_cast<long long>(drainP95Ratio));
    const qint64 currentRegistrationThroughput =
        qint64(operationCount) * 1000000000LL / registrationMedian;
    const qint64 baselineRegistrationThroughput =
        qint64(operationCount) * 1000000000LL / saved912RegistrationMedian;
    const QString machineMetrics =
        QStringLiteral("{\"schema\":1,\"case\":\"production_four_owner\","
                       "\"warmup_rounds\":%1,\"paired_rounds\":%2,\"paired_groups_per_round\":200,"
                       "\"measurements_per_side_per_group\":2,"
                       "\"operations_per_sample\":%3,\"owners_per_operation\":4,"
                       "\"registration_current_median_ns\":%4,"
                       "\"registration_current_p95_ns\":%5,"
                       "\"registration_baseline_median_ns\":%6,"
                       "\"registration_baseline_p95_ns\":%7,"
                       "\"drain_current_median_ns\":%8,\"drain_current_p95_ns\":%9,"
                       "\"drain_baseline_median_ns\":%10,\"drain_baseline_p95_ns\":%11,"
                       "\"registration_median_ratio_bp\":%12,"
                       "\"registration_p95_ratio_bp\":%13,\"drain_median_ratio_bp\":%14,"
                       "\"drain_p95_ratio_bp\":%15,\"max_ratio_bp\":10200,"
                       "\"current_throughput_ops_per_s\":%16,"
                       "\"baseline_throughput_ops_per_s\":%17,\"current_signals\":%18,"
                       "\"baseline_signals\":%19,\"current_pending_high_water\":%20}")
            .arg(warmupRoundCount)
            .arg(roundCount)
            .arg(operationCount)
            .arg(registrationMedian)
            .arg(registrationP95)
            .arg(saved912RegistrationMedian)
            .arg(saved912RegistrationP95)
            .arg(drainMedian)
            .arg(drainP95)
            .arg(saved912DrainMedian)
            .arg(saved912DrainP95)
            .arg(registrationMedianRatio)
            .arg(registrationP95Ratio)
            .arg(drainMedianRatio)
            .arg(drainP95Ratio)
            .arg(currentRegistrationThroughput)
            .arg(baselineRegistrationThroughput)
            .arg(currentSignals)
            .arg(baselineSignals)
            .arg(currentPendingHighWater);
    emitMachineMetrics(QStringLiteral("gpu-retirement-production.json"), machineMetrics);
    const uint64_t expectedMeasuredSignals =
        uint64_t(roundCount) * 200 * 2 * uint64_t(operationCount);
    QCOMPARE(currentSignals, expectedMeasuredSignals);
    QCOMPARE(baselineSignals, expectedMeasuredSignals);
    for (int operation = 0; operation < operationCount; ++operation) {
        const uint64_t expectedFenceValue =
            currentFenceBase - uint64_t(operationCount) + uint64_t(operation) + 1;
        for (const auto& owner : fixtures[size_t(operation)])
            QCOMPARE(owner->pendingFenceValue(), expectedFenceValue);
    }
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

void TestGpuRetireRegistryProductionPerf::scopedViewConsumesExactHandles() {
    constexpr uintptr_t domain = 0x741;
    const uint64_t authority = currentDeviceAuthority();
    QVERIFY(authority != 0);
    std::array<std::shared_ptr<GpuSurface>, 4> owners;
    for (size_t i = 0; i < owners.size(); ++i) {
        owners[i] =
            std::make_shared<ProductionPerfSurface>(reinterpret_cast<void*>(uintptr_t(0x7410 + i)),
                                                    GpuSurfaceCompatibility{domain, authority});
    }
    auto fence = std::make_shared<ProductionPerfFence>(domain, authority);
    GpuRetireRegistry registry;
    bool exact = false;
    auto adapter = [&](const GpuScopedNativeView<4>& view) noexcept {
        exact = true;
        for (size_t i = 0; i < view.size(); ++i) {
            exact =
                exact && view[i].nativeHandle() == reinterpret_cast<void*>(uintptr_t(0x7410 + i));
        }
        return exact ? GpuSubmitOutcome::Submitted : GpuSubmitOutcome::NotSubmitted;
    };
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(adapter, GpuSurfacePack<4>(owners));
    QVERIFY(exact);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    fence->complete();
    registry.drainCompleted();
}

void TestGpuRetireRegistryProductionPerf::scopedViewHandleBindingOverhead() {
    const ScopedBenchmarkThreadAffinity affinity;
    QVERIFY2(affinity.pinned(), "scoped handle binding benchmark could not pin its thread");
    constexpr uintptr_t domain = 0x742;
    const uint64_t authority = currentDeviceAuthority();
    QVERIFY(authority != 0);
    constexpr int operationCount = 256;
    constexpr int warmupRoundCount = 3;
    constexpr int warmupPairedGroupsPerRound = 100;
    constexpr int pairedGroupCount = 2000;
    constexpr int measurementsPerSidePerGroup = 2;
    constexpr int sampleCount = pairedGroupCount * measurementsPerSidePerGroup;
    static_assert(sampleCount == 4000, "handle gate must retain 4000 measured samples per side");
    qint64 counterProbe = 0;
    QVERIFY2(readHandleWorkCounter(counterProbe),
             "scoped handle binding work counter is unavailable");
    std::array<std::array<std::shared_ptr<GpuSurface>, 4>, operationCount> currentFixtures;
    std::array<std::array<std::shared_ptr<GpuSurface>, 4>, operationCount> baselineFixtures;
    for (int operation = 0; operation < operationCount; ++operation) {
        for (size_t owner = 0; owner < 4; ++owner) {
            const auto handle =
                reinterpret_cast<void*>(uintptr_t(0x742000 + operation * 4 + owner));
            const GpuSurfaceCompatibility compatibility{domain, authority};
            if (((operation + int(owner)) & 1) == 0) {
                currentFixtures[size_t(operation)][owner] =
                    std::make_shared<HandlePerfSurface>(handle, compatibility);
                baselineFixtures[size_t(operation)][owner] =
                    std::make_shared<HandlePerfSurface>(handle, compatibility);
            } else {
                currentFixtures[size_t(operation)][owner] =
                    std::make_shared<AlternateHandlePerfSurface>(handle, compatibility);
                baselineFixtures[size_t(operation)][owner] =
                    std::make_shared<AlternateHandlePerfSurface>(handle, compatibility);
            }
        }
    }
    std::vector<qint64> current;
    std::vector<qint64> baseline;
    current.reserve(pairedGroupCount);
    baseline.reserve(pairedGroupCount);
    uintptr_t currentChecksum = 0;
    uintptr_t baselineChecksum = 0;
    uint64_t currentFenceBase = 0;
    uint64_t baselineFenceBase = 0;
    bool counterValid = true;
    struct HandleSampleMetrics {
        qint64 workUnits = 0;
        bool publishedAll = false;
        uint64_t signalCalls = 0;
    };
    auto currentSample = [&] {
        GpuRetireRegistry registry;
        auto fence = std::make_shared<ProductionPerfFence>(domain, authority, currentFenceBase);
        qint64 started = 0;
        const bool startValid = readHandleWorkCounter(started);
        bool publishedAll = true;
        for (const auto& owners : currentFixtures) {
            CurrentHandlePerfAdapter adapter{&currentChecksum};
            GpuOpScope operation(fence, registry);
            const auto result = operation.submit(adapter, GpuSurfacePack<4>(owners));
            publishedAll = publishedAll && result.retirement == GpuRetirementDisposition::Published;
        }
        qint64 finished = 0;
        const bool finishValid = readHandleWorkCounter(finished);
        counterValid = counterValid && startValid && finishValid && finished >= started;
        const uint64_t signalCalls = fence->signalCount();
        fence->complete();
        registry.drainCompleted();
        currentFenceBase += operationCount;
        return HandleSampleMetrics{finished - started, publishedAll, signalCalls};
    };
    auto baselineSample = [&] {
        Saved912PreparedRegistry registry;
        auto fence = std::make_shared<ProductionPerfFence>(domain, authority, baselineFenceBase);
        qint64 started = 0;
        const bool startValid = readHandleWorkCounter(started);
        bool publishedAll = true;
        for (const auto& owners : baselineFixtures) {
            Saved912HandlePerfAdapter adapter{&owners, &baselineChecksum};
            Saved912GpuOpScope<ProductionPerfFence> operation(fence, registry);
            const auto result = operation.submit(adapter, Saved912SurfacePack<4>(owners));
            publishedAll = publishedAll && result.retirement == GpuRetirementDisposition::Published;
            for (const auto& owner : owners)
                owner->retainUntilFenceRetired(result.fenceValue);
        }
        qint64 finished = 0;
        const bool finishValid = readHandleWorkCounter(finished);
        counterValid = counterValid && startValid && finishValid && finished >= started;
        const uint64_t signalCalls = fence->signalCount();
        fence->complete();
        registry.drainCompleted();
        baselineFenceBase += operationCount;
        return HandleSampleMetrics{finished - started, publishedAll, signalCalls};
    };
    bool publicationAndSignalOracleValid = true;
    int measuredCurrentSamples = 0;
    int measuredBaselineSamples = 0;
    int warmupCurrentSamples = 0;
    int warmupBaselineSamples = 0;
    uint64_t measuredCurrentSignalCalls = 0;
    uint64_t measuredBaselineSignalCalls = 0;
    uint64_t warmupCurrentSignalCalls = 0;
    uint64_t warmupBaselineSignalCalls = 0;
    auto measurePairedGroup = [&](int group, bool measured) {
        HandleSampleMetrics currentFirst;
        HandleSampleMetrics currentSecond;
        HandleSampleMetrics baselineFirst;
        HandleSampleMetrics baselineSecond;
        if ((group & 1) == 0) { // C-B-B-C
            currentFirst = currentSample();
            baselineFirst = baselineSample();
            baselineSecond = baselineSample();
            currentSecond = currentSample();
        } else { // B-C-C-B
            baselineFirst = baselineSample();
            currentFirst = currentSample();
            currentSecond = currentSample();
            baselineSecond = baselineSample();
        }
        const auto sampleValid = [&](const HandleSampleMetrics& sample) {
            return sample.publishedAll && sample.signalCalls == uint64_t(operationCount);
        };
        publicationAndSignalOracleValid = publicationAndSignalOracleValid &&
                                          sampleValid(currentFirst) && sampleValid(currentSecond) &&
                                          sampleValid(baselineFirst) && sampleValid(baselineSecond);
        if (measured) {
            current.push_back((currentFirst.workUnits + currentSecond.workUnits) / 2);
            baseline.push_back((baselineFirst.workUnits + baselineSecond.workUnits) / 2);
            measuredCurrentSamples += measurementsPerSidePerGroup;
            measuredBaselineSamples += measurementsPerSidePerGroup;
            measuredCurrentSignalCalls += currentFirst.signalCalls + currentSecond.signalCalls;
            measuredBaselineSignalCalls += baselineFirst.signalCalls + baselineSecond.signalCalls;
        } else {
            warmupCurrentSamples += measurementsPerSidePerGroup;
            warmupBaselineSamples += measurementsPerSidePerGroup;
            warmupCurrentSignalCalls += currentFirst.signalCalls + currentSecond.signalCalls;
            warmupBaselineSignalCalls += baselineFirst.signalCalls + baselineSecond.signalCalls;
        }
    };
    constexpr int warmupPairedGroupCount = warmupRoundCount * warmupPairedGroupsPerRound;
    for (int group = 0; group < warmupPairedGroupCount; ++group)
        measurePairedGroup(group, false);
    for (int group = 0; group < pairedGroupCount; ++group)
        measurePairedGroup(warmupPairedGroupCount + group, true);
    QVERIFY2(counterValid, "scoped handle binding work counter failed during measurement");
    QVERIFY2(publicationAndSignalOracleValid,
             "scoped handle binding sample did not publish or signal exactly once per operation");
    constexpr int warmupSampleCount = warmupPairedGroupCount * measurementsPerSidePerGroup;
    constexpr uint64_t expectedMeasuredSignalCalls =
        uint64_t(sampleCount) * uint64_t(operationCount);
    constexpr uint64_t expectedWarmupSignalCalls =
        uint64_t(warmupSampleCount) * uint64_t(operationCount);
    QCOMPARE(measuredCurrentSamples, sampleCount);
    QCOMPARE(measuredBaselineSamples, sampleCount);
    QCOMPARE(warmupCurrentSamples, warmupSampleCount);
    QCOMPARE(warmupBaselineSamples, warmupSampleCount);
    QCOMPARE(measuredCurrentSignalCalls, expectedMeasuredSignalCalls);
    QCOMPARE(measuredBaselineSignalCalls, expectedMeasuredSignalCalls);
    QCOMPARE(warmupCurrentSignalCalls, expectedWarmupSignalCalls);
    QCOMPARE(warmupBaselineSignalCalls, expectedWarmupSignalCalls);
    QCOMPARE(currentFenceBase, baselineFenceBase);
    QCOMPARE(current.size(), size_t(pairedGroupCount));
    QCOMPARE(baseline.size(), size_t(pairedGroupCount));
    QCOMPARE(currentChecksum, baselineChecksum);
    for (int operation = 0; operation < operationCount; ++operation) {
        const uint64_t expectedFenceValue =
            currentFenceBase - uint64_t(operationCount) + uint64_t(operation) + 1;
        for (const auto& owner : currentFixtures[size_t(operation)])
            QCOMPARE(owner->pendingFenceValue(), expectedFenceValue);
        for (const auto& owner : baselineFixtures[size_t(operation)])
            QCOMPARE(owner->pendingFenceValue(), expectedFenceValue);
    }
    const qint64 currentMedian = percentile(current, 1, 2);
    const qint64 currentP95 = percentile(current, 95, 100);
    const qint64 baselineMedian = percentile(baseline, 1, 2);
    const qint64 baselineP95 = percentile(baseline, 95, 100);
    const qint64 medianRatio = currentMedian * 10000 / baselineMedian;
    const qint64 p95Ratio = currentP95 * 10000 / baselineP95;
    qInfo("Scoped handle binding: current=%lld/%lld baseline=%lld/%lld %s per %d ops, "
          "ratios=%lld/%lld bp from %d pair-averaged observations / %d raw measurements per side",
          static_cast<long long>(currentMedian), static_cast<long long>(currentP95),
          static_cast<long long>(baselineMedian), static_cast<long long>(baselineP95),
          kHandleMeasurementUnit, operationCount, static_cast<long long>(medianRatio),
          static_cast<long long>(p95Ratio), pairedGroupCount, sampleCount);
    emitMachineMetrics(QStringLiteral("gpu-scoped-handle-binding.json"),
#ifdef _WIN32
                       QStringLiteral("{\"schema\":2,\"case\":\"scoped_handle_binding\","
                                      "\"measurement_unit\":\"thread_cycles\","
                                      "\"warmup_rounds\":%1,\"warmup_paired_groups_per_round\":%2,"
                                      "\"paired_groups\":%3,"
                                      "\"measurements_per_side_per_group\":%4,"
                                      "\"warmup_raw_measurements_per_side\":%5,"
                                      "\"measured_raw_measurements_per_side\":%6,"
                                      "\"pair_averaged_observations_per_side\":%7,"
                                      "\"operations_per_raw_measurement\":%8,"
                                      "\"current_median_thread_cycles\":%9,"
                                      "\"current_p95_thread_cycles\":%10,"
                                      "\"baseline_median_thread_cycles\":%11,"
                                      "\"baseline_p95_thread_cycles\":%12,"
                                      "\"median_ratio_bp\":%13,\"p95_ratio_bp\":%14,"
                                      "\"current_measured_signal_calls\":%15,"
                                      "\"baseline_measured_signal_calls\":%16,"
                                      "\"current_warmup_signal_calls\":%17,"
                                      "\"baseline_warmup_signal_calls\":%18,"
                                      "\"max_ratio_bp\":10200}")
#else
                       QStringLiteral(
                           "{\"schema\":2,\"case\":\"scoped_handle_binding\","
                           "\"measurement_unit\":\"wall_ns\","
                           "\"warmup_rounds\":%1,\"warmup_paired_groups_per_round\":%2,"
                           "\"paired_groups\":%3,"
                           "\"measurements_per_side_per_group\":%4,"
                           "\"warmup_raw_measurements_per_side\":%5,"
                           "\"measured_raw_measurements_per_side\":%6,"
                           "\"pair_averaged_observations_per_side\":%7,"
                           "\"operations_per_raw_measurement\":%8,"
                           "\"current_median_wall_ns\":%9,\"current_p95_wall_ns\":%10,"
                           "\"baseline_median_wall_ns\":%11,\"baseline_p95_wall_ns\":%12,"
                           "\"median_ratio_bp\":%13,\"p95_ratio_bp\":%14,"
                           "\"current_measured_signal_calls\":%15,"
                           "\"baseline_measured_signal_calls\":%16,"
                           "\"current_warmup_signal_calls\":%17,"
                           "\"baseline_warmup_signal_calls\":%18,"
                           "\"max_ratio_bp\":10200}")
#endif
                           .arg(warmupRoundCount)
                           .arg(warmupPairedGroupsPerRound)
                           .arg(pairedGroupCount)
                           .arg(measurementsPerSidePerGroup)
                           .arg(warmupSampleCount)
                           .arg(sampleCount)
                           .arg(pairedGroupCount)
                           .arg(operationCount)
                           .arg(currentMedian)
                           .arg(currentP95)
                           .arg(baselineMedian)
                           .arg(baselineP95)
                           .arg(medianRatio)
                           .arg(p95Ratio)
                           .arg(measuredCurrentSignalCalls)
                           .arg(measuredBaselineSignalCalls)
                           .arg(warmupCurrentSignalCalls)
                           .arg(warmupBaselineSignalCalls));
#ifdef NDEBUG
    QVERIFY2(medianRatio <= 10200, "scoped handle binding median regressed by more than 2%");
    QVERIFY2(p95Ratio <= 10200, "scoped handle binding p95 regressed by more than 2%");
#endif
}

QTEST_GUILESS_MAIN(TestGpuRetireRegistryProductionPerf)
#include "tst_gpuretireregistry_production_perf.moc"

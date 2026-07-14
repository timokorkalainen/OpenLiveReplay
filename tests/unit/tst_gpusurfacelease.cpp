// Type-state gate for the GPU surface native handle (Challenge 3, part 1).
//
// The load-bearing guarantee is a NEGATIVE one, enforced at COMPILE time by the
// static_asserts below: GpuSurface::nativeHandle() is unreachable outside a
// GpuReadLease, and a DeadDeviceToken is unconstructible outside the two driver
// mints. If either gate regresses, this translation unit stops compiling — which is
// exactly the falsifiability the challenge asks for. The runtime slots then exercise
// the positive path (lease hands out the real handle; the bounded-wait drain releases
// only retired surfaces).
#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusubmission.h"
#include "playback/output/framepixelformat.h"

#include <memory>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>

#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority {
    static uint64_t capture() {
        return GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    }
    static uint64_t publish(uint64_t deviceAuthorityEpoch = capture(),
                            uintptr_t deviceDomainId = 0) {
        return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch,
            deviceDomainId);
    }
};
#endif

namespace {

// SFINAE probe: is `s.nativeHandle()` well-formed from a non-friend context? Access
// checking is part of substitution (CWG1170), so a protected member yields a soft
// failure -> false_type. This is the compile-time negative-compile test: it lives in
// a normally-compiled TU, so a regression to a public nativeHandle() breaks the build.
template <typename T, typename = void>
struct CanCallNativeHandle : std::false_type {};
template <typename T>
struct CanCallNativeHandle<T, std::void_t<decltype(std::declval<const T&>().nativeHandle())>>
    : std::true_type {};

static_assert(!CanCallNativeHandle<GpuSurface>::value,
              "GpuSurface::nativeHandle() must be inaccessible without a GpuReadLease — "
              "the type-state gate is broken if this fires.");

// The device-loss no-wait free is gated on a DeadDeviceToken that only the driver
// mints can construct. Prove the injection path cannot fabricate one.
static_assert(!std::is_default_constructible<DeadDeviceToken>::value,
              "DeadDeviceToken must not be default-constructible (mint-only).");
static_assert(!std::is_constructible<DeadDeviceToken, DeadDeviceToken::Provenance, uint64_t>::value,
              "DeadDeviceToken's provenance constructor must be private (driver-mint-only).");
static_assert(!std::is_constructible<DeadDeviceToken, DeadDeviceToken::Provenance, uint64_t,
                                     uintptr_t>::value,
              "DeadDeviceToken's device-scoped constructor must be private (driver-mint-only).");
static_assert(!std::is_copy_constructible<GpuReadLease>::value,
              "GpuReadLease must not escape a synchronous callback by copy.");
static_assert(!std::is_move_constructible<GpuReadLease>::value,
              "GpuReadLease must not escape a synchronous callback by move.");
static_assert(!std::is_copy_constructible<GpuOwnedNativeHandle>::value,
              "The raw-surface native owner must remain unique.");
static_assert(std::is_nothrow_move_constructible<GpuOwnedNativeHandle>::value,
              "The allocation-free native owner must transfer without throwing.");

// A surface whose native handle is a known sentinel. nativeHandle() is protected,
// mirroring the production surfaces, so the ONLY way the test reads it is via a lease.
class FakeLeaseSurface : public GpuSurface {
public:
    FakeLeaseSurface(void* handle, bool valid, GpuSurfaceCompatibility compatibility = {})
        : m_handle(handle), m_valid(valid), m_compatibility(compatibility) {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16, 0}; }
    bool isValid() const override { return m_valid; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }

protected:
    void* nativeHandle() const override { return m_valid ? m_handle : nullptr; }

private:
    void* m_handle = nullptr;
    bool m_valid = false;
    GpuSurfaceCompatibility m_compatibility;
};

// Fence with a test-controllable completed watermark.
class FakeFence : public GpuFence {
public:
    explicit FakeFence(uintptr_t deviceDomainId = 0, uint64_t authorityEpoch = 1)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override {
        ++m_signalCalls;
        return ++m_signalled;
    }
    bool wait(uint64_t value, int /*timeoutMs*/) override {
        m_waitSawUnlockedRetainer = gpuRetireDetail::mutexAvailableForTest();
        if (m_retireOnWait) m_completed = value;
        return m_completed >= value;
    }
    uint64_t completedValue() const override {
        m_completedSawUnlockedRetainer = gpuRetireDetail::mutexAvailableForTest();
        ++m_completedCalls;
        return m_completed;
    }
    void setCompleted(uint64_t v) { m_completed = v; }
    void setRetireOnWait(bool retire) { m_retireOnWait = retire; }
    bool waitSawUnlockedRetainer() const { return m_waitSawUnlockedRetainer; }
    int signalCalls() const { return m_signalCalls; }
    int completedCalls() const { return m_completedCalls; }
    bool completedSawUnlockedRetainer() const { return m_completedSawUnlockedRetainer; }

private:
    uint64_t m_signalled = 0;
    uint64_t m_completed = 0;
    bool m_retireOnWait = false;
    bool m_waitSawUnlockedRetainer = false;
    int m_signalCalls = 0;
    mutable int m_completedCalls = 0;
    mutable bool m_completedSawUnlockedRetainer = false;
};

class ZeroSignalFence final : public GpuFence {
public:
    uint64_t signal() override {
        ++m_signalCalls;
        return 0;
    }
    bool wait(uint64_t value, int) override { return m_completed >= value; }
    uint64_t completedValue() const override { return m_completed; }
    void retireQuarantine() { m_completed = std::numeric_limits<uint64_t>::max(); }
    int signalCalls() const { return m_signalCalls; }

private:
    uint64_t m_completed = 0;
    int m_signalCalls = 0;
};

bool isExpectedCheckedContractTermination(QProcess::ExitStatus status, int exitCode) {
#ifdef Q_OS_WIN
    constexpr quint32 windowsFailFastAssertionStatus = 0xC0000602u;
    Q_UNUSED(status);
    return static_cast<quint32>(exitCode) == windowsFailFastAssertionStatus;
#else
    Q_UNUSED(exitCode);
    return status == QProcess::CrashExit;
#endif
}

struct ChildProcessResult {
    bool started = false;
    bool finishedBeforeTimeout = false;
    bool timedOut = false;
    bool reaped = false;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    int exitCode = -1;
    QProcess::ProcessError processError = QProcess::UnknownError;
    QString errorString;
    QByteArray output;
};

ChildProcessResult runChildProcess(const QString& testFunction, const QByteArray& environmentName,
                                   int timeoutMs, const QByteArray& startupMarker = {}) {
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(environmentName), QStringLiteral("1"));
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {testFunction});

    ChildProcessResult result;
    result.started = child.waitForStarted(5000);
    if (result.started && !startupMarker.isEmpty()) {
        QElapsedTimer markerWait;
        markerWait.start();
        while (!result.output.contains(startupMarker) && child.state() != QProcess::NotRunning &&
               markerWait.elapsed() < 5000) {
            (void) child.waitForReadyRead(5000 - int(markerWait.elapsed()));
            result.output += child.readAll();
        }
    }
    if (result.started) {
        result.finishedBeforeTimeout = child.waitForFinished(timeoutMs);
        if (!result.finishedBeforeTimeout) {
            result.timedOut = true;
            child.terminate();
            if (!child.waitForFinished(250)) {
                child.kill();
                (void) child.waitForFinished(5000);
            }
        }
    }
    if (child.state() != QProcess::NotRunning) {
        child.kill();
        (void) child.waitForFinished(5000);
    }

    result.reaped = child.state() == QProcess::NotRunning;
    result.exitStatus = child.exitStatus();
    result.exitCode = child.exitCode();
    result.processError = child.error();
    result.errorString = child.errorString();
    result.output += child.readAll();
    return result;
}

bool isAcceptedCheckedContractDeath(const ChildProcessResult& result) {
    return result.started && result.finishedBeforeTimeout && !result.timedOut && result.reaped &&
           isExpectedCheckedContractTermination(result.exitStatus, result.exitCode);
}

QString childProcessDiagnostic(const ChildProcessResult& result) {
    return QStringLiteral("started=%1 finishedBeforeTimeout=%2 timedOut=%3 reaped=%4 "
                          "exitStatus=%5 exitCodeSigned=%6 exitCodeHex=0x%7 processError=%8 "
                          "errorString=%9 output=%10")
        .arg(result.started ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(result.finishedBeforeTimeout ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(result.timedOut ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(result.reaped ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(int(result.exitStatus))
        .arg(result.exitCode)
        .arg(qulonglong(static_cast<quint32>(result.exitCode)), 8, 16, QLatin1Char('0'))
        .arg(int(result.processError))
        .arg(result.errorString)
        .arg(QString::fromLocal8Bit(result.output));
}

} // namespace

class TestGpuSurfaceLease : public QObject {
    Q_OBJECT
private slots:
    void callbackLeaseExposesMetadataOnly();
    void callbackLeaseReportsInvalidSurface();
    void withReadCompletesScope();
    void withReadCompletesDuringExceptionUnwinding();
    void handleQueryAfterCompletionFailsCheckedContract();
    void checkedContractDeathOracleRejectsUnrelatedExit();
    void deathControlTimeoutCapturesDiagnosticsAndReaps();
    void surfaceOwnerSurvivesUntilLeaseDestruction();
    void boundedWaitDrainReleasesOnlyRetired();
    void boundedWaitDoesNotHoldRetainerMutex();
    void registryDiagnosticsTrackHighWaterAndTimeouts();
    void registryRegistrationDoesNotPollDriver();
    void opScopeSignalsOnceAndRegistersUniqueSurfaces();
    void opScopeCancelsBeforeSubmissionWithoutRetaining();
    void opScopeRetainsAfterSubmittedError();
    void opScopeQuarantinesOnZeroSignal();
    void zeroSignalQuarantineReleasesAfterAuthoritativeUpgrade();
    void deadTokenAbandonsOnlyMatchingDeviceDomain();
    void multipleDeadTokensAbandonInOnePass();
    void retirementEvidenceRejectsWrongFenceInstanceAndDomain();
    void submissionEvidenceRejectsStaleGenerationAndAuthority();
};

void TestGpuSurfaceLease::callbackLeaseExposesMetadataOnly() {
    auto sentinel = reinterpret_cast<void*>(0xBEEF);
    auto surface = std::make_shared<FakeLeaseSurface>(sentinel, /*valid=*/true);
    {
        GpuSyncReadScope scope;
        const GpuReadLease lease = scope.read(surface);
        const auto state = std::make_pair(lease.valid(), lease.desc().width);
        QVERIFY(state.first);
        QCOMPARE(state.second, 16);
        QCOMPARE(lease.nativeHandle(), sentinel);
        scope.complete();
    }
}

void TestGpuSurfaceLease::callbackLeaseReportsInvalidSurface() {
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x1), /*valid=*/false);
    GpuSyncReadScope scope;
    const bool valid = scope.read(surface).valid();
    scope.complete();
    QVERIFY(!valid);
}

void TestGpuSurfaceLease::retirementEvidenceRejectsWrongFenceInstanceAndDomain() {
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t preparedDomain = 0xD011;
    constexpr uintptr_t otherDomain = 0xD022;
    constexpr uint64_t authorityEpoch = 9;
    const uint64_t generation = GpuGenerationCounter::instance().current();
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xBEEF), true,
                                           GpuSurfaceCompatibility{preparedDomain, authorityEpoch});
    auto preparedFence = std::make_shared<FakeFence>(preparedDomain, authorityEpoch);
    auto sameDomainOtherFence = std::make_shared<FakeFence>(preparedDomain, authorityEpoch);
    auto otherDomainFence = std::make_shared<FakeFence>(otherDomain, authorityEpoch);

    GpuRetirementTicket ticket{preparedFence, preparedFence->identity(), generation, 0};
    QVERIFY(preparedFence->validatesPreparedSubmission(ticket, surface->compatibility()));
    QVERIFY(!sameDomainOtherFence->validatesPreparedSubmission(ticket, surface->compatibility()));
    QVERIFY(!otherDomainFence->validatesPreparedSubmission(ticket, surface->compatibility()));

    const uint64_t preparedValue = preparedFence->signal();
    QCOMPARE(sameDomainOtherFence->signal(), preparedValue);
    QCOMPARE(otherDomainFence->signal(), preparedValue);
    ticket.value = preparedValue;

    QVERIFY(preparedFence->validatesRetirement(ticket, surface->compatibility()));
    QVERIFY(!sameDomainOtherFence->validatesRetirement(ticket, surface->compatibility()));
    QVERIFY(!otherDomainFence->validatesRetirement(ticket, surface->compatibility()));
}

void TestGpuSurfaceLease::submissionEvidenceRejectsStaleGenerationAndAuthority() {
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t reusedDomain = 0xDEC0DE;
    constexpr uint64_t oldAuthorityEpoch = 41;
    constexpr uint64_t newAuthorityEpoch = 42;
    const uint64_t oldGeneration = GpuGenerationCounter::instance().current();
    const GpuSurfaceCompatibility oldSurface{reusedDomain, oldAuthorityEpoch};
    auto oldFence = std::make_shared<FakeFence>(reusedDomain, oldAuthorityEpoch);

    QVERIFY(oldFence->acceptsSubmission(oldSurface, oldGeneration));
    GpuGenerationCounter::instance().bump();
    QVERIFY(!oldFence->acceptsSubmission(oldSurface, oldGeneration));

    auto replacementFence = std::make_shared<FakeFence>(reusedDomain, newAuthorityEpoch);
    const uint64_t newGeneration = GpuGenerationCounter::instance().current();
    QVERIFY(!replacementFence->acceptsSubmission(oldSurface, newGeneration));
    QVERIFY(replacementFence->acceptsSubmission(
        GpuSurfaceCompatibility{reusedDomain, newAuthorityEpoch}, newGeneration));

    const GpuRetirementTicket staleTicket{oldFence, oldFence->identity(), oldGeneration, 1};
    QVERIFY(!oldFence->validatesRetirement(staleTicket, oldSurface));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::withReadCompletesScope() {
    auto sentinel = reinterpret_cast<void*>(0xCAFE);
    auto surface = std::make_shared<FakeLeaseSurface>(sentinel, /*valid=*/true);

    void* observed = nullptr;
    {
        GpuSyncReadScope scope;
        observed =
            scope.withRead(surface, [](const GpuReadLease& lease) { return lease.nativeHandle(); });
    }

    QCOMPARE(observed, sentinel);
}

void TestGpuSurfaceLease::withReadCompletesDuringExceptionUnwinding() {
#ifndef QT_NO_DEBUG
    constexpr auto deathChildEnvironment = "OLR_GPU_SYNC_READ_THROW_DEATH_CHILD";
    constexpr auto callbackReachedThrowMarker = "withRead callback reached throw";
    if (qEnvironmentVariableIsSet(deathChildEnvironment)) {
        auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xFACE), true);
        GpuSyncReadScope scope;
        const GpuReadLease retainedLease = scope.read(surface);
        try {
            scope.withRead(surface, [](const GpuReadLease& lease) {
                (void) lease.nativeHandle();
                std::fputs("withRead callback reached throw\n", stderr);
                std::fflush(stderr);
                throw std::runtime_error("expected callback failure");
            });
        } catch (const std::runtime_error&) {
        }
        (void) retainedLease.nativeHandle();
        scope.complete();
        return;
    }

    const ChildProcessResult result =
        runChildProcess(QStringLiteral("withReadCompletesDuringExceptionUnwinding"),
                        QByteArray(deathChildEnvironment), 10000);
    const QString diagnostic = childProcessDiagnostic(result);
    QVERIFY2(result.output.contains(callbackReachedThrowMarker), qPrintable(diagnostic));
    QVERIFY2(isAcceptedCheckedContractDeath(result), qPrintable(diagnostic));
#else
    QSKIP("Checked-contract assertions are disabled in this build");
#endif
}

void TestGpuSurfaceLease::handleQueryAfterCompletionFailsCheckedContract() {
#ifndef QT_NO_DEBUG
    constexpr auto deathChildEnvironment = "OLR_GPU_SYNC_READ_LEASE_DEATH_CHILD";
    if (qEnvironmentVariableIsSet(deathChildEnvironment)) {
        auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD00D), true);
        GpuSyncReadScope scope;
        const GpuReadLease lease = scope.read(surface);
        scope.complete();
        (void) lease.nativeHandle();
        return;
    }

    const ChildProcessResult result =
        runChildProcess(QStringLiteral("handleQueryAfterCompletionFailsCheckedContract"),
                        QByteArray(deathChildEnvironment), 10000);
    const QString diagnostic = childProcessDiagnostic(result);
    QVERIFY2(isAcceptedCheckedContractDeath(result), qPrintable(diagnostic));
#else
    QSKIP("Checked-contract assertions are disabled in this build");
#endif
}

void TestGpuSurfaceLease::checkedContractDeathOracleRejectsUnrelatedExit() {
#ifndef QT_NO_DEBUG
    constexpr auto unrelatedChildEnvironment = "OLR_GPU_SYNC_READ_UNRELATED_FAILURE_CHILD";
    if (qEnvironmentVariableIsSet(unrelatedChildEnvironment)) {
        std::fputs("unrelated child failure mutation\n", stderr);
        std::fflush(stderr);
        std::exit(7);
    }

    const ChildProcessResult result =
        runChildProcess(QStringLiteral("checkedContractDeathOracleRejectsUnrelatedExit"),
                        QByteArray(unrelatedChildEnvironment), 10000);
    const QString diagnostic = childProcessDiagnostic(result);
    QVERIFY2(result.output.contains("unrelated child failure mutation"), qPrintable(diagnostic));
    QVERIFY2(result.started && result.finishedBeforeTimeout && !result.timedOut && result.reaped,
             qPrintable(diagnostic));
    QVERIFY2(result.exitStatus == QProcess::NormalExit, qPrintable(diagnostic));
    QVERIFY2(result.exitCode == 7, qPrintable(diagnostic));
    QVERIFY2(!isAcceptedCheckedContractDeath(result), qPrintable(diagnostic));
#else
    QSKIP("Checked-contract assertions are disabled in this build");
#endif
}

void TestGpuSurfaceLease::deathControlTimeoutCapturesDiagnosticsAndReaps() {
#ifndef QT_NO_DEBUG
    constexpr auto timeoutChildEnvironment = "OLR_GPU_SYNC_READ_TIMEOUT_CHILD";
    constexpr auto timeoutOutputMarker = "timeout child output marker";
    if (qEnvironmentVariableIsSet(timeoutChildEnvironment)) {
        std::fputs("timeout child output marker\n", stderr);
        std::fflush(stderr);
        QThread::msleep(5000);
        std::exit(0);
    }

    const ChildProcessResult result =
        runChildProcess(QStringLiteral("deathControlTimeoutCapturesDiagnosticsAndReaps"),
                        QByteArray(timeoutChildEnvironment), 10, QByteArray(timeoutOutputMarker));
    QVERIFY(result.started);
    QVERIFY(result.timedOut);
    QVERIFY(result.reaped);
    QVERIFY(result.output.contains(timeoutOutputMarker));
    QVERIFY(!isAcceptedCheckedContractDeath(result));
    const QString diagnostic = childProcessDiagnostic(result);
    QVERIFY(diagnostic.contains(QStringLiteral("timedOut=true")));
    QVERIFY(diagnostic.contains(QStringLiteral("exitCodeSigned=")));
    QVERIFY(diagnostic.contains(QStringLiteral("exitCodeHex=0x")));
    QVERIFY(diagnostic.contains(QStringLiteral("processError=")));
    QVERIFY(diagnostic.contains(QStringLiteral("errorString=")));
    QVERIFY(diagnostic.contains(QString::fromLatin1(timeoutOutputMarker)));
#else
    QSKIP("Checked-contract assertions are disabled in this build");
#endif
}

void TestGpuSurfaceLease::surfaceOwnerSurvivesUntilLeaseDestruction() {
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xABCD), true);
    std::weak_ptr<FakeLeaseSurface> observer = surface;
    bool ownerSurvivedBeforeCompletion = false;
    bool ownerSurvivedAfterCompletion = false;

    GpuSyncReadScope scope;
    {
        const GpuReadLease lease = scope.read(surface);
        surface.reset();
        ownerSurvivedBeforeCompletion = !observer.expired();
        scope.complete();
        ownerSurvivedAfterCompletion = !observer.expired();
    }

    QVERIFY(ownerSurvivedBeforeCompletion);
    QVERIFY(ownerSurvivedAfterCompletion);
    QVERIFY(observer.expired());
}

void TestGpuSurfaceLease::boundedWaitDrainReleasesOnlyRetired() {
    // Track our own surface's shared ownership rather than the global count, so other
    // tests' retains cannot perturb the assertions.
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x2), /*valid=*/true);
    auto fence = std::make_shared<FakeFence>();
    const long baseline = surface.use_count();

    GpuRetireRegistry registry;
    registry.registerRetire(surface, fence, 5);
    QVERIFY(surface.use_count() > baseline); // the retainer holds a reference

    // Fence has not reached 5 -> bounded wait cannot retire it -> surface stays held.
    registry.drainWithBoundedWait(1);
    QVERIFY(surface.use_count() > baseline);

    // Fence retires -> the bounded-wait drain releases the surface.
    fence->setCompleted(5);
    registry.drainWithBoundedWait(1);
    QCOMPARE(surface.use_count(), baseline);
}

void TestGpuSurfaceLease::boundedWaitDoesNotHoldRetainerMutex() {
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x3), true);
    auto fence = std::make_shared<FakeFence>();
    fence->setRetireOnWait(true);
    GpuRetireRegistry registry;
    registry.registerRetire(surface, fence, 9);

    QCOMPARE(registry.drainWithBoundedWait(1), 1);
    QVERIFY(fence->waitSawUnlockedRetainer());
}

void TestGpuSurfaceLease::registryDiagnosticsTrackHighWaterAndTimeouts() {
    GpuRetireRegistry registry;
    const GpuRetireDiagnostics before = registry.diagnostics();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x4), true);
    auto fence = std::make_shared<FakeFence>();
    registry.registerRetire(surface, fence, 11);

    const GpuRetireDiagnostics registered = registry.diagnostics();
    QVERIFY(registered.pendingRetains >= 1);
    QVERIFY(registered.highWaterMark >= registered.pendingRetains);
    QCOMPARE(registry.drainWithBoundedWait(1), 0);
    QVERIFY(registry.diagnostics().timeoutCount >= before.timeoutCount + 1);

    fence->setCompleted(11);
    registry.drainCompleted();
}

void TestGpuSurfaceLease::opScopeSignalsOnceAndRegistersUniqueSurfaces() {
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t spillsBefore = GpuOpScope::spillAllocationCount();
    auto first = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5), true);
    auto second = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x6), true);
    auto fence = std::make_shared<FakeFence>();

    GpuOpScope operation(fence, registry);
    QVERIFY(operation.track(first));
    QVERIFY(!operation.track(first));
    QVERIFY(operation.track(second));
    QVERIFY(operation.submit([] { return GpuSubmitOutcome::Submitted; }));

    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 2);
    QCOMPARE(GpuOpScope::spillAllocationCount(), spillsBefore);
    fence->setCompleted(1);
    registry.drainCompleted();
}

void TestGpuSurfaceLease::opScopeCancelsBeforeSubmissionWithoutRetaining() {
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x7), true);
    const long ownersBefore = surface.use_count();
    auto fence = std::make_shared<FakeFence>();

    {
        GpuOpScope operation(fence, registry);
        QVERIFY(operation.track(surface));
        QVERIFY(!operation.submit([] { return GpuSubmitOutcome::NotSubmitted; }));
    }

    QCOMPARE(fence->signalCalls(), 0);
    QCOMPARE(surface.use_count(), ownersBefore);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuSurfaceLease::opScopeQuarantinesOnZeroSignal() {
    GpuDeviceLossMonitor::instance().reset();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t failuresBefore = registry.diagnostics().signalFailureCount;
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x8), true);
    auto fence = std::make_shared<ZeroSignalFence>();

    GpuOpScope operation(fence, registry);
    QVERIFY(operation.track(surface));
    QVERIFY(!operation.submit([] { return GpuSubmitOutcome::Submitted; }));
    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QCOMPARE(registry.diagnostics().signalFailureCount, failuresBefore + 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(!GpuDeviceLossMonitor::instance().realLossToken().has_value());

    fence->retireQuarantine();
    registry.drainCompleted();
    GpuDeviceLossMonitor::instance().reset();
}

void TestGpuSurfaceLease::zeroSignalQuarantineReleasesAfterAuthoritativeUpgrade() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t deviceAuthorityEpoch = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xA), true);
    const long ownersBefore = surface.use_count();
    auto fence = std::make_shared<ZeroSignalFence>();

    GpuOpScope operation(fence, registry);
    QVERIFY(operation.track(surface));
    QVERIFY(!operation.submit([] { return GpuSubmitOutcome::Submitted; }));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(surface.use_count() > ownersBefore);

    const uint64_t lossGeneration =
        GpuDeviceLossMonitorTestAuthority::publish(deviceAuthorityEpoch);
    QVERIFY(lossGeneration != 0);
    const auto token = monitor.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(registry.abandonAllNoWait(*token), qsizetype(1));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QCOMPARE(surface.use_count(), ownersBefore);
    monitor.reset();
}

void TestGpuSurfaceLease::deadTokenAbandonsOnlyMatchingDeviceDomain() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    auto deadSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xB), true);
    auto liveSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xC), true);
    const long deadOwners = deadSurface.use_count();
    const long liveOwners = liveSurface.use_count();
    auto deadFence = std::make_shared<FakeFence>(11);
    auto liveFence = std::make_shared<FakeFence>(22);
    GpuRetireRegistry registry;
    registry.registerRetire(deadSurface, deadFence, 1);
    registry.registerRetire(liveSurface, liveFence, 1);

    GpuDeviceLossMonitorTestAuthority::publish(authority, 11);
    const auto token = monitor.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(token->deviceDomainId(), uintptr_t(11));
    QCOMPARE(registry.abandonAllNoWait(*token), qsizetype(1));
    QCOMPARE(deadSurface.use_count(), deadOwners);
    QVERIFY(liveSurface.use_count() > liveOwners);

    liveFence->setCompleted(1);
    registry.drainCompleted();
    QCOMPARE(liveSurface.use_count(), liveOwners);
    monitor.reset();
}

void TestGpuSurfaceLease::multipleDeadTokensAbandonInOnePass() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    auto firstSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD), true);
    auto secondSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xE), true);
    const long firstOwners = firstSurface.use_count();
    const long secondOwners = secondSurface.use_count();
    GpuRetireRegistry registry;
    registry.registerRetire(firstSurface, std::make_shared<FakeFence>(31), 1);
    registry.registerRetire(secondSurface, std::make_shared<FakeFence>(32), 1);

    GpuDeviceLossMonitorTestAuthority::publish(authority, 31);
    GpuDeviceLossMonitorTestAuthority::publish(authority, 32);
    QCOMPARE(registry.abandonAllNoWait(monitor.realLossTokens()), qsizetype(2));
    QCOMPARE(firstSurface.use_count(), firstOwners);
    QCOMPARE(secondSurface.use_count(), secondOwners);
    monitor.reset();
}

void TestGpuSurfaceLease::registryRegistrationDoesNotPollDriver() {
    GpuRetireRegistry registry;
    auto fence = std::make_shared<FakeFence>();
    GpuOpScope operation(fence, registry);
    for (quintptr value = 1; value <= 4; ++value)
        QVERIFY(operation.track(
            std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(value), true)));
    QVERIFY(operation.submit([] { return GpuSubmitOutcome::Submitted; }));
    QCOMPARE(fence->completedCalls(), 1);
    QVERIFY(fence->completedSawUnlockedRetainer());
    fence->setCompleted(1);
    registry.drainCompleted();
}

void TestGpuSurfaceLease::opScopeRetainsAfterSubmittedError() {
    GpuDeviceLossMonitor::instance().reset();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x9), true);
    auto fence = std::make_shared<FakeFence>();

    GpuOpScope operation(fence, registry);
    QVERIFY(operation.track(surface));
    QVERIFY(!operation.submit([] { return GpuSubmitOutcome::SubmittedWithError; }));
    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(!GpuDeviceLossMonitor::instance().realLossToken().has_value());

    fence->setCompleted(1);
    registry.drainCompleted();
    GpuDeviceLossMonitor::instance().reset();
}

QTEST_GUILESS_MAIN(TestGpuSurfaceLease)
#include "tst_gpusurfacelease.moc"

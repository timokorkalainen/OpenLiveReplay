#include "playback/output/win/wingpuimportedge.h"

#ifdef _WIN32

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpuframereadbacktelemetry.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/win/d3d11gpusurface.h"

#include "recorder_engine/ingest/h26xaccessunit.h"
#include "recorder_engine/ingest/nativeframecopy.h"
#include "recorder_engine/ingest/nativevideodecoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#ifdef OLR_UNIT_TEST
#include <QSemaphore>
#endif
#include <QStringList>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <d3d10.h>
#include <d3d11.h>
#include <functional>
#include <future>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <objbase.h>
#include <thread>
#include <utility>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
}

using Microsoft::WRL::ComPtr;

namespace {

#ifdef OLR_UNIT_TEST
std::atomic<int> winDeviceLossPollCallsForTest{0};
#endif

constexpr const char* kMfHardwareDecoderEnv = "OLR_MF_VIDEO_ENABLE_HARDWARE";
constexpr int kD3DReadbackFenceTimeoutMs = 2000;

class ScopedComApartment {
public:
    ScopedComApartment() noexcept : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedComApartment() {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }

    bool usable() const noexcept { return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE; }
    HRESULT result() const noexcept { return m_result; }

private:
    HRESULT m_result = E_FAIL;
};

uintptr_t deviceDomainId(ID3D11Device* device) {
    ComPtr<IUnknown> identity;
    return device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&identity)))
               ? reinterpret_cast<uintptr_t>(identity.Get())
               : 0;
}

class ScopedHardwareDecoderProbeFlag {
public:
    ScopedHardwareDecoderProbeFlag()
        : m_wasSet(qEnvironmentVariableIsSet(kMfHardwareDecoderEnv)),
          m_previous(qgetenv(kMfHardwareDecoderEnv)) {
        qputenv(kMfHardwareDecoderEnv, QByteArrayLiteral("1"));
    }

    ~ScopedHardwareDecoderProbeFlag() {
        if (m_wasSet) {
            qputenv(kMfHardwareDecoderEnv, m_previous);
        } else {
            qunsetenv(kMfHardwareDecoderEnv);
        }
    }

private:
    bool m_wasSet = false;
    QByteArray m_previous;
};

QString hresultString(const char* what, HRESULT hr) {
    return QStringLiteral("%1 failed (0x%2)")
        .arg(QString::fromLatin1(what))
        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

bool createD3D11(ComPtr<ID3D11Device>* device, ComPtr<IMFDXGIDeviceManager>* manager,
                 UINT* resetToken, QString* detail) {
    ComPtr<ID3D11DeviceContext> context;
    const std::array<D3D_FEATURE_LEVEL, 4> levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels.data(), UINT(levels.size()), D3D11_SDK_VERSION, &*device, &created, &context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels.data() + 1,
                               UINT(levels.size() - 1), D3D11_SDK_VERSION, &*device, &created,
                               &context);
    }
    if (FAILED(hr)) {
        if (detail) *detail = hresultString("D3D11CreateDevice", hr);
        return false;
    }

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device->As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    hr = MFCreateDXGIDeviceManager(resetToken, &*manager);
    if (FAILED(hr)) {
        if (detail) *detail = hresultString("MFCreateDXGIDeviceManager", hr);
        return false;
    }

    hr = (*manager)->ResetDevice(device->Get(), *resetToken);
    if (FAILED(hr)) {
        if (detail) *detail = hresultString("IMFDXGIDeviceManager::ResetDevice", hr);
        return false;
    }
    return true;
}

QByteArray byteArrayFromBytes(const unsigned char* bytes, int size) {
    return QByteArray(reinterpret_cast<const char*>(bytes), size);
}

CompressedAccessUnit makeProbeH264AccessUnit() {
    static constexpr unsigned char kAnnexB[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1e, 0xdd, 0xec, 0x04, 0x40,
        0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x03, 0x00, 0xa3, 0xc5, 0x8b,
        0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0xc8, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x09, 0x02, 0xe0,
    };
    static constexpr unsigned char kSps[] = {
        0x67, 0x42, 0xc0, 0x1e, 0xdd, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03,
        0x00, 0x40, 0x00, 0x00, 0x03, 0x00, 0xa3, 0xc5, 0x8b, 0xe0,
    };
    static constexpr unsigned char kPps[] = {0x68, 0xce, 0x0f, 0xc8};

    const QByteArray annexB = byteArrayFromBytes(kAnnexB, int(sizeof(kAnnexB)));
    H26xAccessUnitSplitter splitter(NativeVideoCodec::H264);
    const QList<CompressedAccessUnit> units = splitter.pushPesPayload(annexB, 0, 0);
    if (!units.isEmpty()) return units.first();

    CompressedAccessUnit unit;
    unit.codec = NativeVideoCodec::H264;
    unit.pts90k = 0;
    unit.dts90k = 0;
    unit.annexB = annexB;
    unit.parameterSets.h264Sps.append(byteArrayFromBytes(kSps, int(sizeof(kSps))));
    unit.parameterSets.h264Pps.append(byteArrayFromBytes(kPps, int(sizeof(kPps))));
    return unit;
}

bool decodedSampleHasD3DTexture(void* nativeDecodedImage) {
    if (!nativeDecodedImage) return false;

    auto* sample = static_cast<IMFSample*>(nativeDecodedImage);
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) || !buffer) return false;

    ComPtr<IMFDXGIBuffer> dxgi;
    if (FAILED(buffer.As(&dxgi)) || !dxgi) return false;

    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&texture))) && texture;
}

class D3D11IGpuFrameData final : public IFrameData {
public:
#ifdef OLR_GPU_PIPELINE_BUILD
    D3D11IGpuFrameData(std::shared_ptr<D3D11GpuSurface> surface,
                       std::shared_ptr<GpuFence> renderFence, uint64_t renderFenceValue,
                       GpuBudgetCharge budgetCharge)
        : m_surface(std::move(surface)), m_renderFence(std::move(renderFence)),
          m_renderFenceValue(renderFenceValue), m_budgetCharge(std::move(budgetCharge)) {}
#else
    D3D11IGpuFrameData(std::shared_ptr<D3D11GpuSurface> surface,
                       std::shared_ptr<GpuFence> renderFence, uint64_t renderFenceValue)
        : m_surface(std::move(surface)), m_renderFence(std::move(renderFence)),
          m_renderFenceValue(renderFenceValue) {}
#endif

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override;
    CpuPlanes cachedCpuPlanes(FramePixelFormat target) const override;
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    std::shared_ptr<GpuFence> gpuFence() const override { return m_renderFence; }
    GpuFrameSynchronization gpuSynchronization() const override {
        if (m_renderFenceValue == 0) return {nullptr, 0, true};
        return {m_renderFence, m_renderFenceValue, true};
    }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }
    void seedCpuCacheForTest(CpuPlanes planes) const {
        if (!planes.isValid()) return;
        QMutexLocker locker(&m_cacheMutex);
        m_cpuCache.insert(int(planes.format), std::move(planes));
    }

private:
    std::shared_ptr<D3D11GpuSurface> m_surface;
    std::shared_ptr<GpuFence> m_renderFence;
    uint64_t m_renderFenceValue = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
    GpuBudgetCharge m_budgetCharge;
#endif
    mutable QMutex m_cacheMutex;
    mutable QHash<int, CpuPlanes> m_cpuCache;
};

} // namespace

WinGpuImportCapabilities probeWinGpuImport() {
    WinGpuImportCapabilities caps;
    caps.backend = QString::fromLatin1(kWinRhiBackend);

    QString decodeError;
    bool sawDecodedSample = false;
    bool importsDecodedSample = false;
    ScopedHardwareDecoderProbeFlag forceHardwareDecoder;
    NativeVideoDecoder decoder(/*outputWidth=*/16, /*outputHeight=*/16);
    const bool decoded = decoder.decodeKeepSurface(
        makeProbeH264AccessUnit(),
        [&](void* nativeDecodedImage, qint64) {
            sawDecodedSample = nativeDecodedImage != nullptr;
            importsDecodedSample = decodedSampleHasD3DTexture(nativeDecodedImage);
            return true;
        },
        &decodeError);
    if (!decoded || !sawDecodedSample) {
        caps.detail =
            decodeError.isEmpty()
                ? QStringLiteral("MF H.264 keep-surface probe produced no decoded sample; CPU "
                                 "fallback")
                : QStringLiteral("MF H.264 keep-surface probe failed: %1; CPU fallback")
                      .arg(decodeError);
        return caps;
    }

    caps.d3d11KeepTexture = importsDecodedSample;
    caps.rhiImportable = importsDecodedSample;
    caps.detail = importsDecodedSample
                      ? QStringLiteral("decoded MF H.264 sample exposes a D3D11 texture; "
                                       "keep-texture path available")
                      : QStringLiteral("decoded MF H.264 sample is not DXGI-backed on this host; "
                                       "CPU fallback");
    return caps;
}

#ifdef OLR_UNIT_TEST
bool winGpuImportProbeForcesHardwareDecoderForTest() {
    const bool wasSet = qEnvironmentVariableIsSet(kMfHardwareDecoderEnv);
    const QByteArray previous = qgetenv(kMfHardwareDecoderEnv);

    qunsetenv(kMfHardwareDecoderEnv);
    bool forced = false;
    bool restored = false;
    {
        ScopedHardwareDecoderProbeFlag forceHardwareDecoder;
        forced = qgetenv(kMfHardwareDecoderEnv) == QByteArrayLiteral("1");
    }
    restored = !qEnvironmentVariableIsSet(kMfHardwareDecoderEnv);

    if (wasSet) {
        qputenv(kMfHardwareDecoderEnv, previous);
    } else {
        qunsetenv(kMfHardwareDecoderEnv);
    }

    return forced && restored;
}
#endif

struct WinGpuImportEdge::Impl {
    struct DeviceLossPollState {
        std::atomic<bool> deviceLost{false};
        std::atomic<bool> pollPending{false};
#ifdef OLR_UNIT_TEST
        std::atomic<QSemaphore*> blockEntered{nullptr};
        std::atomic<QSemaphore*> blockRelease{nullptr};
#endif
    };

    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    bool mfStarted = false;
    uint64_t deviceAuthorityEpoch = 0;
    std::shared_ptr<DeviceLossPollState> deviceLossState = std::make_shared<DeviceLossPollState>();
    std::function<void(const FrameHandle&)> importTap;

    ~Impl() {
        ScopedComApartment apartment;
        if (mfStarted) MFShutdown();
        manager.Reset();
        device.Reset();
    }

    bool ownsDevice(ID3D11Device* candidate) const {
        if (!candidate || !device) return false;
        ComPtr<IUnknown> candidateIdentity;
        ComPtr<IUnknown> edgeIdentity;
        return SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&candidateIdentity))) &&
               SUCCEEDED(device.As(&edgeIdentity)) && candidateIdentity.Get() == edgeIdentity.Get();
    }

    bool noteDeviceLostReason(HRESULT reason, uintptr_t domainId) const {
        if (FAILED(reason)) {
            const uint64_t generation = WinGpuImportEdge::publishDeviceRemovedForMonitor(
                reason, deviceAuthorityEpoch, domainId);
            if (generation != 0) deviceLossState->deviceLost.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    bool noteDeviceLostIfRemoved() const {
        return device &&
               noteDeviceLostReason(device->GetDeviceRemovedReason(), deviceDomainId(device.Get()));
    }
};

WinGpuImportEdge::WinGpuImportEdge() : m_impl(std::make_unique<Impl>()) {}

uint64_t WinGpuImportEdge::publishDeviceRemovedForMonitor(HRESULT reason,
                                                          uint64_t deviceAuthorityEpoch,
                                                          uintptr_t domainId) {
    if (SUCCEEDED(reason)) return 0;
    return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
        DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch, domainId);
}

WinGpuImportEdge::~WinGpuImportEdge() {
    m_impl.reset();
}

std::unique_ptr<WinGpuImportEdge> WinGpuImportEdge::create(QString* error) {
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    if (!caps.d3d11KeepTexture) {
        if (error) *error = caps.detail;
        return nullptr;
    }

    auto edge = std::unique_ptr<WinGpuImportEdge>(new WinGpuImportEdge());
    ScopedComApartment apartment;
    if (!apartment.usable()) {
        if (error) *error = hresultString("CoInitializeEx", apartment.result());
        return nullptr;
    }

    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mfHr)) {
        if (error) *error = hresultString("MFStartup", mfHr);
        return nullptr;
    }
    edge->m_impl->mfStarted = true;

    QString detail;
    edge->m_impl->deviceAuthorityEpoch =
        GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    if (!createD3D11(&edge->m_impl->device, &edge->m_impl->manager, &edge->m_impl->resetToken,
                     &detail)) {
        if (error) *error = detail;
        return nullptr;
    }
    return edge;
}

bool WinGpuImportEdge::isAvailable() const {
    return m_impl && m_impl->device &&
           !m_impl->deviceLossState->deviceLost.load(std::memory_order_acquire);
}

bool WinGpuImportEdge::deviceLost() const {
#ifdef OLR_UNIT_TEST
    winDeviceLossPollCallsForTest.fetch_add(1, std::memory_order_acq_rel);
#endif
    if (!m_impl) return false;
    if (m_impl->deviceLossState->deviceLost.load(std::memory_order_acquire)) return true;
    return m_impl->noteDeviceLostIfRemoved();
}

bool WinGpuImportEdge::pollDeviceLossFor(int timeoutMs) const {
#ifdef OLR_UNIT_TEST
    winDeviceLossPollCallsForTest.fetch_add(1, std::memory_order_acq_rel);
#endif
    if (!m_impl) return false;
    const auto state = m_impl->deviceLossState;
    if (state->deviceLost.load(std::memory_order_acquire)) return true;

#ifdef OLR_UNIT_TEST
    QSemaphore* const blockEntered = state->blockEntered.exchange(nullptr);
    QSemaphore* const blockRelease = state->blockRelease.exchange(nullptr);
#else
    constexpr void* blockEntered = nullptr;
    constexpr void* blockRelease = nullptr;
#endif
    const ComPtr<ID3D11Device> device = m_impl->device;
    if (!device && !blockEntered) return false;
    bool expected = false;
    if (!state->pollPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                    std::memory_order_acquire))
        return state->deviceLost.load(std::memory_order_acquire);

    const uint64_t authority = m_impl->deviceAuthorityEpoch;
    auto completion = std::make_shared<std::promise<void>>();
    std::future<void> completed = completion->get_future();
    try {
        std::thread([state, device, authority, completion, blockEntered, blockRelease] {
            ScopedComApartment apartment;
#ifdef OLR_UNIT_TEST
            if (blockEntered) blockEntered->release();
            if (blockRelease) blockRelease->acquire();
#endif
            if (device) {
                const HRESULT reason = device->GetDeviceRemovedReason();
                if (FAILED(reason)) {
                    const uint64_t generation = WinGpuImportEdge::publishDeviceRemovedForMonitor(
                        reason, authority, deviceDomainId(device.Get()));
                    if (generation != 0) state->deviceLost.store(true, std::memory_order_release);
                }
            }
            state->pollPending.store(false, std::memory_order_release);
            try {
                completion->set_value();
            } catch (...) {
                static_cast<void>(0);
            }
        }).detach();
    } catch (...) {
        state->pollPending.store(false, std::memory_order_release);
        return state->deviceLost.load(std::memory_order_acquire);
    }
    const int boundedTimeoutMs = timeoutMs < 0 ? 0 : timeoutMs;
    (void) completed.wait_for(std::chrono::milliseconds(boundedTimeoutMs));
    return state->deviceLost.load(std::memory_order_acquire);
}

#ifdef OLR_UNIT_TEST
std::unique_ptr<WinGpuImportEdge> WinGpuImportEdge::createUnavailableForTest() {
    return std::unique_ptr<WinGpuImportEdge>(new WinGpuImportEdge);
}

void WinGpuImportEdge::resetDeviceLossPollCountForTest() noexcept {
    winDeviceLossPollCallsForTest.store(0, std::memory_order_release);
}

int WinGpuImportEdge::deviceLossPollCountForTest() noexcept {
    return winDeviceLossPollCallsForTest.load(std::memory_order_acquire);
}

bool WinGpuImportEdge::observeDeviceRemovedForTest(HRESULT reason, uint64_t deviceAuthorityEpoch,
                                                   uintptr_t deviceDomainId) {
    if (!m_impl) return false;
    if (m_impl->deviceLossState->deviceLost.load(std::memory_order_acquire)) return true;
    m_impl->deviceAuthorityEpoch = deviceAuthorityEpoch;
    return m_impl->noteDeviceLostReason(reason, deviceDomainId);
}

bool WinGpuImportEdge::deviceLostStickyForTest() const noexcept {
    return m_impl && m_impl->deviceLossState->deviceLost.load(std::memory_order_acquire);
}

void WinGpuImportEdge::blockNextDeviceLossPollForTest(QSemaphore* entered,
                                                      QSemaphore* release) noexcept {
    if (!m_impl) return;
    m_impl->deviceLossState->blockRelease.store(release, std::memory_order_release);
    m_impl->deviceLossState->blockEntered.store(entered, std::memory_order_release);
}
#endif

std::optional<FrameHandle> WinGpuImportEdge::tryImport(void* mfSampleOpaque, int feedIndex,
                                                       qint64 ptsMs, int width, int height,
                                                       std::shared_ptr<GpuFence> renderFence) {
    if (!renderFence) return std::nullopt;
    auto surface = tryImportSurface(mfSampleOpaque, width, height);
    if (!surface) return std::nullopt;

    FrameMetadata meta;
    meta.key.feedIndex = feedIndex;
    meta.key.ptsMs = ptsMs;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = width;
    meta.key.height = height;
    return makeGpuFrameHandleForTest(std::move(surface), meta, std::move(renderFence));
}

std::shared_ptr<D3D11GpuSurface> WinGpuImportEdge::tryImportSurface(void* mfSampleOpaque, int width,
                                                                    int height) {
    if (!isAvailable() || !mfSampleOpaque || width <= 0 || height <= 0) return nullptr;

    auto* sample = static_cast<IMFSample*>(mfSampleOpaque);
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) || !buffer) {
        if (m_impl) m_impl->noteDeviceLostIfRemoved();
        return nullptr;
    }

    ComPtr<IMFDXGIBuffer> dxgi;
    if (FAILED(buffer.As(&dxgi)) || !dxgi) {
        if (m_impl) m_impl->noteDeviceLostIfRemoved();
        return nullptr;
    }

    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture))) || !texture) {
        if (m_impl) m_impl->noteDeviceLostIfRemoved();
        return nullptr;
    }
    dxgi->GetSubresourceIndex(&subresource);

    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (!m_impl->ownsDevice(textureDevice.Get())) return nullptr;
    auto surface = D3D11GpuSurface::createKept(textureDevice, texture, subresource, width, height,
                                               m_impl->deviceAuthorityEpoch);
    if (!surface && m_impl) m_impl->noteDeviceLostIfRemoved();
    return surface;
}

std::shared_ptr<GpuFence>
WinGpuImportEdge::createFenceForSurface(const std::shared_ptr<D3D11GpuSurface>& surface) {
    if (!surface) return nullptr;
    const uint64_t authorityEpoch = surface->compatibility().authorityEpoch;
    std::shared_ptr<GpuFence> result;
    GpuSyncReadScope scope;
    scope.withRead(surface, [authorityEpoch, &result](const GpuReadLease& lease) {
        auto* texture = static_cast<ID3D11Texture2D*>(lease.nativeHandle());
        ComPtr<ID3D11Device> device;
        if (texture) texture->GetDevice(&device);
        result = makeD3D11GpuFence(device.Get(), authorityEpoch);
    });
    return result;
}

std::shared_ptr<GpuFence> WinGpuImportEdge::createFence() const {
    return (m_impl && m_impl->device)
               ? makeD3D11GpuFence(m_impl->device.Get(), m_impl->deviceAuthorityEpoch)
               : nullptr;
}

FrameHandle WinGpuImportEdge::makeGpuFrameHandleForTest(std::shared_ptr<D3D11GpuSurface> surface,
                                                        FrameMetadata meta,
                                                        std::shared_ptr<GpuFence> renderFence,
#ifdef OLR_GPU_PIPELINE_BUILD
                                                        GpuBudgetCharge charge,
#else
                                                        std::nullptr_t charge,
#endif
                                                        uint64_t* submittedFenceValue) {
    if (!surface) return FrameHandle();
    if (renderFence && !renderFence->sharesDeviceAuthorityWith(surface)) return FrameHandle();
    if (meta.key.width <= 0) meta.key.width = surface->desc().width;
    if (meta.key.height <= 0) meta.key.height = surface->desc().height;
    meta.key.format = FramePixelFormat::Nv12;
    uint64_t exactFenceValue = 0;
    std::shared_ptr<GpuFence> exactFence;
    if (renderFence) {
        GpuRetireRegistry registry;
        GpuOpScope operation(renderFence, registry);
        auto adapter = []() noexcept { return GpuSubmitOutcome::Submitted; };
        const auto result = operation.submitRetained(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        if (!result.succeeded()) return FrameHandle{};
        exactFence = result.producerFence;
        exactFenceValue = result.fenceValue;
        if (submittedFenceValue) *submittedFenceValue = exactFenceValue;
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    auto data = std::make_shared<D3D11IGpuFrameData>(std::move(surface), std::move(exactFence),
                                                     exactFenceValue, std::move(charge));
#else
    (void) charge;
    auto data = std::make_shared<D3D11IGpuFrameData>(std::move(surface), std::move(exactFence),
                                                     exactFenceValue);
#endif
    return FrameHandle(std::move(data), meta);
}

#if defined(OLR_GPU_PIPELINE_BUILD) && defined(OLR_UNIT_TEST)
FrameHandle WinGpuImportEdge::makeGpuFrameHandleWithCachedCpuForTest(
    std::shared_ptr<D3D11GpuSurface> surface, FrameMetadata meta,
    std::shared_ptr<GpuFence> renderFence, GpuBudgetCharge charge, uint64_t* submittedFenceValue,
    CpuPlanes cachedCpu) {
    FrameHandle handle =
        makeGpuFrameHandleForTest(std::move(surface), std::move(meta), std::move(renderFence),
                                  std::move(charge), submittedFenceValue);
    const auto data = std::dynamic_pointer_cast<const D3D11IGpuFrameData>(handle.dataPtr());
    if (data) data->seedCpuCacheForTest(std::move(cachedCpu));
    return handle;
}
#endif

void WinGpuImportEdge::setImportTapForTest(std::function<void(const FrameHandle&)> tap) {
    if (!m_impl) return;
    m_impl->importTap = std::move(tap);
}

bool WinGpuImportEdge::acceptsD3D11DeviceForTest(void* device) const {
    return m_impl && m_impl->ownsDevice(static_cast<ID3D11Device*>(device));
}

bool WinGpuImportEdge::decodeOneForTest(ComPtr<ID3D11Device> device, ComPtr<ID3D11Texture2D> nv12,
                                        int width, int height) {
    const uint64_t authorityEpoch =
        m_impl ? m_impl->deviceAuthorityEpoch
               : GpuDeviceLossMonitor::instance().currentDeviceAuthorityEpoch();
    auto surface = D3D11GpuSurface::createKept(std::move(device), std::move(nv12), 0, width, height,
                                               authorityEpoch);
    if (!surface) return false;

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = width;
    meta.key.height = height;
    const FrameHandle handle = makeGpuFrameHandleForTest(std::move(surface), meta);
    if (m_impl && m_impl->importTap) m_impl->importTap(handle);
    return !handle.isNull();
}

CpuPlanes D3D11IGpuFrameData::readToCpu(FramePixelFormat target) const {
    CpuPlanes out;
    if (!m_surface) return out;

    QMutexLocker locker(&m_cacheMutex);
    const auto cached = m_cpuCache.constFind(int(target));
    if (cached != m_cpuCache.cend()) return cached.value();

    bool nativeReadComplete = false;
    GpuSyncReadScope readScope;
    readScope.withRead(m_surface, [&](const GpuReadLease& lease) {
        auto* src = static_cast<ID3D11Texture2D*>(lease.nativeHandle());
        ComPtr<ID3D11Device> retainedDevice;
        if (src) src->GetDevice(&retainedDevice);
        ID3D11Device* device = retainedDevice.Get();
        if (!device || !src) return;

        if (m_renderFenceValue != 0) {
            if (!m_renderFence ||
                !m_renderFence->wait(m_renderFenceValue, kD3DReadbackFenceTimeoutMs)) {
                return;
            }
        }

        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        if (!ctx) return;

        D3D11_TEXTURE2D_DESC desc{};
        src->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.MiscFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.ArraySize = 1;
        staging.MipLevels = 1;

        ComPtr<ID3D11Texture2D> readable;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, &readable))) return;
        ctx->CopySubresourceRegion(readable.Get(), 0, 0, 0, 0, src, lease.nativeSubresource(),
                                   nullptr);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;

        const int w = m_surface->desc().width;
        const int h = m_surface->desc().height;
        const int chromaH = (h + 1) / 2;
        const auto* base = static_cast<const uint8_t*>(mapped.pData);
        const int pitch = int(mapped.RowPitch);
        const uint8_t* yPlane = base;
        const uint8_t* uvPlane = base + size_t(pitch) * h;

        if (target == FramePixelFormat::Yuv420p) {
            AVFrame* frame = nativeCopyNv12ToYuv420p(yPlane, pitch, uvPlane, pitch, w, h);
            if (frame) {
                out.format = FramePixelFormat::Yuv420p;
                out.width = w;
                out.height = h;
                out.stride[0] = frame->linesize[0];
                out.stride[1] = frame->linesize[1];
                out.stride[2] = frame->linesize[2];
                out.plane[0] = QByteArray(reinterpret_cast<const char*>(frame->data[0]),
                                          frame->linesize[0] * h);
                out.plane[1] = QByteArray(reinterpret_cast<const char*>(frame->data[1]),
                                          frame->linesize[1] * chromaH);
                out.plane[2] = QByteArray(reinterpret_cast<const char*>(frame->data[2]),
                                          frame->linesize[2] * chromaH);
                av_frame_free(&frame);
            }
        } else if (target == FramePixelFormat::Nv12) {
            out.format = FramePixelFormat::Nv12;
            out.width = w;
            out.height = h;
            out.stride[0] = pitch;
            out.stride[1] = pitch;
            out.plane[0] = QByteArray(reinterpret_cast<const char*>(yPlane), pitch * h);
            out.plane[1] = QByteArray(reinterpret_cast<const char*>(uvPlane), pitch * chromaH);
        }

        ctx->Unmap(readable.Get(), 0);
        nativeReadComplete = true;
    });
    if (!nativeReadComplete) return out;

    if (out.isValid()) {
        gpuRecordFrameReadToCpuReadback();
        if (m_renderFence && m_surface) {
            GpuRetireRegistry registry;
            GpuOpScope operation(m_renderFence, registry);
            auto adapter = []() noexcept { return GpuSubmitOutcome::Submitted; };
            (void) operation.submitRetained(
                adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{m_surface}));
        }
        m_cpuCache.insert(int(target), out);
    }
    return out;
}

CpuPlanes D3D11IGpuFrameData::cachedCpuPlanes(FramePixelFormat target) const {
    QMutexLocker locker(&m_cacheMutex);
    const auto cached = m_cpuCache.constFind(int(target));
    return cached == m_cpuCache.cend() ? CpuPlanes{} : cached.value();
}

#endif // _WIN32

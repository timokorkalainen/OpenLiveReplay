#include <QtTest>

#include "playback/output/win/wingpuimportedge.h"
#ifdef _WIN32
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/win/d3d11gpusurface.h"
#include "playback/output/win/d3dfence.h"
#include "recorder_engine/ingest/nativeframecopy.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <d3d11.h>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
}

using Microsoft::WRL::ComPtr;

namespace {

template <typename Device, typename = void>
struct CanMakeD3D11FenceWithoutAuthority : std::false_type {};

template <typename Device>
struct CanMakeD3D11FenceWithoutAuthority<
    Device, std::void_t<decltype(makeD3D11GpuFence(std::declval<Device>()))>> : std::true_type {};

template <typename Device, typename Texture, typename = void>
struct CanKeepD3D11SurfaceWithoutAuthority : std::false_type {};

template <typename Device, typename Texture>
struct CanKeepD3D11SurfaceWithoutAuthority<
    Device, Texture,
    std::void_t<decltype(D3D11GpuSurface::createKept(
        std::declval<Device>(), std::declval<Texture>(), UINT(0), 1, 1))>> : std::true_type {};

static_assert(!CanMakeD3D11FenceWithoutAuthority<void*>::value,
              "native-device fence construction must require persistent device authority");
static_assert(
    !CanKeepD3D11SurfaceWithoutAuthority<ComPtr<ID3D11Device>, ComPtr<ID3D11Texture2D>>::value,
    "native-surface construction must require persistent device authority");

class DeferredFence final : public GpuFence {
public:
    explicit DeferredFence(GpuSurfaceCompatibility compatibility)
        : GpuFence(compatibility.deviceDomainId, compatibility.authorityEpoch) {}

    uint64_t signal() override { return m_next.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int) override {
        waits.fetch_add(1, std::memory_order_acq_rel);
        lastValue.store(value, std::memory_order_release);
        return completedValue() >= value;
    }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }
    void complete(uint64_t value) { m_completed.store(value, std::memory_order_release); }

    std::atomic<int> waits{0};
    std::atomic<uint64_t> lastValue{0};

protected:
    bool isCompatibleWithNativeHandle(void*) const override { return true; }

private:
    std::atomic<uint64_t> m_next{0};
    std::atomic<uint64_t> m_completed{0};
};

bool createTestD3D11Device(ComPtr<ID3D11Device>* device, ComPtr<ID3D11DeviceContext>* ctx) {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1, D3D11_SDK_VERSION,
                                   device->GetAddressOf(), &level, ctx->GetAddressOf());
    if (SUCCEEDED(hr)) return true;

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1,
                           D3D11_SDK_VERSION, device->GetAddressOf(), &level, ctx->GetAddressOf());
    if (SUCCEEDED(hr)) return true;

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &want, 1, D3D11_SDK_VERSION,
                           device->GetAddressOf(), &level, ctx->GetAddressOf());
    return SUCCEEDED(hr);
}

uint64_t currentDeviceAuthority() {
    return GpuDeviceLossMonitor::instance().currentDeviceAuthorityForTest();
}

} // namespace
#endif

class TestWinGpuImportEdge : public QObject {
    Q_OBJECT
private slots:
    void probeIsConsistentAndNeverThrows();
    void probeForcesHardwareDecoderEnumeration();
    void backendConstantIsValid();
    void createConsistentWithProbe();
    void nullSampleYieldsFallbackNullopt();
    void surfaceKeepsTextureAndTracksFence();
    void surfaceRejectsSuppliedDeviceMismatch();
    void surfaceRejectsNonNv12Texture();
    void edgeRejectsForeignD3D11Device();
    void frameRejectsForeignFenceTimeline();
    void frameGenerationBumpKeepsLiveDeviceAuthority();
    void importedFrameExposesRenderFence();
    void readToCpuDeinterleavesNv12ToI420();
    void readToCpuWaitsForPendingFenceBeforeCopy();
    void fenceSignalsAndWaits();
    void fenceWaitForeverWaitsUntilSignal();
    void importedReadbackMatchesCpuDecodeWithinOneLsb();
    void allocFailureDegradesToCpuFallback();
    void surfaceSurvivesInFlightReadback();
};

void TestWinGpuImportEdge::probeIsConsistentAndNeverThrows() {
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    if (caps.rhiImportable) {
        QVERIFY2(caps.d3d11KeepTexture, "rhiImportable but keep-texture failed");
    }
    QVERIFY(!caps.detail.isEmpty());
#ifdef _WIN32
    if (caps.d3d11KeepTexture) {
        QVERIFY2(caps.detail.contains(QStringLiteral("decoded MF H.264 sample")),
                 "positive Windows GPU import probe must be based on a decoded sample import");
    }
#endif
#ifndef _WIN32
    QVERIFY2(!caps.d3d11KeepTexture, "non-Windows must report no keep-texture");
    QVERIFY2(!caps.rhiImportable, "non-Windows must report no RHI import");
#endif
}

void TestWinGpuImportEdge::probeForcesHardwareDecoderEnumeration() {
#ifndef _WIN32
    QSKIP("Windows-only hardware decoder probe flag");
#else
    QVERIFY(winGpuImportProbeForcesHardwareDecoderForTest());
#endif
}

void TestWinGpuImportEdge::backendConstantIsValid() {
    const QString backend = QString::fromLatin1(kWinRhiBackend);
    QVERIFY2(backend == "d3d11" || backend == "d3d12", "kWinRhiBackend must be d3d11 or d3d12");
}

void TestWinGpuImportEdge::createConsistentWithProbe() {
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    QString error;
    auto edge = WinGpuImportEdge::create(&error);
    if (caps.d3d11KeepTexture) {
        QVERIFY2(edge != nullptr,
                 qPrintable(QStringLiteral("probe says keep-texture but create failed: ") + error));
        QVERIFY(edge->isAvailable());
    } else {
        QVERIFY2(edge == nullptr, "probe says no keep-texture but create returned an edge");
    }
}

void TestWinGpuImportEdge::nullSampleYieldsFallbackNullopt() {
#ifndef _WIN32
    QSKIP("WinGpuImportEdge null sample import is Windows-only");
#else
    QString error;
    auto edge = WinGpuImportEdge::create(&error);
    if (!edge) QSKIP("no GPU import edge on this host (CPU fallback path)");

    auto renderFence = edge->createFence();
    if (!renderFence) QSKIP("no D3D11 fence on this host");

    const std::optional<FrameHandle> handle =
        edge->tryImport(nullptr, 0, 1000, 1280, 720, renderFence);
    QVERIFY2(!handle.has_value(), "null sample must return nullopt (CPU fallback)");
#endif
}

void TestWinGpuImportEdge::surfaceKeepsTextureAndTracksFence() {
#ifndef _WIN32
    QSKIP("D3D11GpuSurface is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1280;
    desc.Height = 720;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

    const uint64_t authorityEpoch = currentDeviceAuthority();
    QVERIFY(D3D11GpuSurface::createKept(device, texture, 0, 1279, 720, authorityEpoch) == nullptr);
    QVERIFY(D3D11GpuSurface::createKept(device, texture, 1, 1280, 720, authorityEpoch) == nullptr);
    auto surface = D3D11GpuSurface::createKept(device, texture, 0, 1280, 720, authorityEpoch);
    QVERIFY(surface != nullptr);
    QVERIFY(surface->isValid());
    QCOMPARE(int(surface->desc().format), int(FramePixelFormat::Nv12));
    QCOMPARE(surface->desc().width, 1280);
    QCOMPARE(surface->desc().height, 720);
    QVERIFY(surface->aliasesTextureForTest(texture.Get()));

    GpuSyncReadScope readScope;
    const GpuReadLease lease = readScope.read(surface);
    QVERIFY(lease.nativeHandle() != nullptr);
    QCOMPARE(static_cast<ID3D11Texture2D*>(lease.nativeHandle()), texture.Get());

    surface->retainUntilFenceRetired(5);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(5));
    surface->retainUntilFenceRetired(3);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(5));
    surface.reset();
    QCOMPARE(static_cast<ID3D11Texture2D*>(lease.nativeHandle()), texture.Get());
    readScope.complete();
#endif
}

void TestWinGpuImportEdge::surfaceRejectsSuppliedDeviceMismatch() {
#ifndef _WIN32
    QSKIP("D3D11GpuSurface is Windows-only");
#else
    ComPtr<ID3D11Device> textureDevice;
    ComPtr<ID3D11DeviceContext> textureContext;
    ComPtr<ID3D11Device> suppliedDevice;
    ComPtr<ID3D11DeviceContext> suppliedContext;
    if (!createTestD3D11Device(&textureDevice, &textureContext) ||
        !createTestD3D11Device(&suppliedDevice, &suppliedContext)) {
        QSKIP("two D3D11 devices are unavailable");
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(textureDevice->CreateTexture2D(&desc, nullptr, &texture)));

    QVERIFY(D3D11GpuSurface::createKept(suppliedDevice, texture, 0, 64, 64,
                                        currentDeviceAuthority()) == nullptr);
#endif
}

void TestWinGpuImportEdge::surfaceRejectsNonNv12Texture() {
#ifndef _WIN32
    QSKIP("D3D11GpuSurface is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!createTestD3D11Device(&device, &context)) QSKIP("no D3D11 test device available");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

    QVERIFY(D3D11GpuSurface::createKept(device, texture, 0, 64, 64, currentDeviceAuthority()) ==
            nullptr);
#endif
}

void TestWinGpuImportEdge::edgeRejectsForeignD3D11Device() {
#ifndef _WIN32
    QSKIP("D3D11 device identity is Windows-only");
#else
    QString error;
    auto edge = WinGpuImportEdge::create(&error);
    if (!edge) QSKIP("no GPU import edge on this host");

    ComPtr<ID3D11Device> foreignDevice;
    ComPtr<ID3D11DeviceContext> foreignContext;
    if (!createTestD3D11Device(&foreignDevice, &foreignContext))
        QSKIP("no second D3D11 test device available");
    QVERIFY(!edge->acceptsD3D11DeviceForTest(foreignDevice.Get()));
#endif
}

void TestWinGpuImportEdge::frameRejectsForeignFenceTimeline() {
#ifndef _WIN32
    QSKIP("D3D11 fence identity is Windows-only");
#else
    ComPtr<ID3D11Device> surfaceDevice;
    ComPtr<ID3D11DeviceContext> surfaceContext;
    ComPtr<ID3D11Device> foreignDevice;
    ComPtr<ID3D11DeviceContext> foreignContext;
    if (!createTestD3D11Device(&surfaceDevice, &surfaceContext) ||
        !createTestD3D11Device(&foreignDevice, &foreignContext)) {
        QSKIP("two D3D11 devices are unavailable");
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(surfaceDevice->CreateTexture2D(&desc, nullptr, &texture)));

    const uint64_t authorityEpoch = currentDeviceAuthority();
    auto surface = D3D11GpuSurface::createKept(surfaceDevice, texture, 0, 64, 64, authorityEpoch);
    auto foreignFence = makeD3D11GpuFence(foreignDevice.Get(), authorityEpoch);
    if (!foreignFence) QSKIP("D3D11 fence unavailable");

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 64;
    QVERIFY(WinGpuImportEdge::makeGpuFrameHandleForTest(surface, meta, foreignFence).isNull());
    QCOMPARE(surface->pendingFenceValue(), uint64_t(0));
#endif
}

void TestWinGpuImportEdge::frameGenerationBumpKeepsLiveDeviceAuthority() {
#ifndef _WIN32
    QSKIP("D3D11 device authority is Windows-only");
#else
    auto& monitor = GpuDeviceLossMonitor::instance();
    auto& generations = GpuGenerationCounter::instance();
    monitor.reset();
    generations.resetForTest();
    const auto resetState = qScopeGuard([&] {
        monitor.reset();
        generations.resetForTest();
        const uint64_t resetAuthority = monitor.currentDeviceAuthorityForTest();
        while (generations.current() < resetAuthority)
            generations.bump();
    });

    const uint64_t deviceAuthority = monitor.currentDeviceAuthorityForTest();
    while (generations.current() < deviceAuthority)
        generations.bump();
    QCOMPARE(generations.current(), deviceAuthority);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!createTestD3D11Device(&device, &context)) QSKIP("no D3D11 test device available");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

    auto preSeekSurface = D3D11GpuSurface::createKept(device, texture, 0, 64, 64, deviceAuthority);
    auto persistentFence = makeD3D11GpuFence(device.Get(), deviceAuthority);
    QVERIFY(preSeekSurface != nullptr);
    if (!persistentFence) QSKIP("D3D11 fence unavailable");
    QCOMPARE(preSeekSurface->compatibility().authorityEpoch, deviceAuthority);
    QCOMPARE(persistentFence->identity().authorityEpoch, deviceAuthority);

    const uint64_t preSeekGeneration = generations.current();
    const uint64_t postSeekGeneration = generations.bump();
    QCOMPARE(postSeekGeneration, preSeekGeneration + 1);
    QCOMPARE(monitor.currentDeviceAuthorityForTest(), deviceAuthority);

    auto postSeekSurface = D3D11GpuSurface::createKept(device, texture, 0, 64, 64, deviceAuthority);
    QVERIFY(postSeekSurface != nullptr);
    QCOMPARE(postSeekSurface->compatibility().deviceDomainId,
             preSeekSurface->compatibility().deviceDomainId);
    QCOMPARE(postSeekSurface->compatibility().authorityEpoch, deviceAuthority);
    QCOMPARE(persistentFence->identity().authorityEpoch, deviceAuthority);

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 64;
    meta.gpuGeneration = postSeekGeneration;
    uint64_t submittedFenceValue = 0;
    const FrameHandle handle = WinGpuImportEdge::makeGpuFrameHandleForTest(
        postSeekSurface, meta, persistentFence, {}, &submittedFenceValue);

    QVERIFY(handle.isGpuBacked());
    QCOMPARE(handle.metadata().gpuGeneration, postSeekGeneration);
    QVERIFY(!handle.isStaleForGeneration(postSeekGeneration));
    QVERIFY(handle.isStaleForGeneration(preSeekGeneration));
    QVERIFY(submittedFenceValue != 0);
    QVERIFY(persistentFence->wait(submittedFenceValue, 2000));
    GpuRetireRegistry{}.drainCompleted();

    monitor.recordLoss();
    monitor.beginRebuild();
    QVERIFY(monitor.currentDeviceAuthorityForTest() != deviceAuthority);
    auto staleSurface = D3D11GpuSurface::createKept(device, texture, 0, 64, 64, deviceAuthority);
    auto staleFence = makeD3D11GpuFence(device.Get(), deviceAuthority);
    QVERIFY(staleSurface != nullptr);
    QVERIFY(staleFence != nullptr);
    QVERIFY(WinGpuImportEdge::makeGpuFrameHandleForTest(staleSurface, meta, staleFence).isNull());
#endif
}

void TestWinGpuImportEdge::importedFrameExposesRenderFence() {
#ifndef _WIN32
    QSKIP("D3D11GpuSurface is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

    auto surface =
        D3D11GpuSurface::createKept(device, texture, 0, 64, 64, currentDeviceAuthority());
    QVERIFY(surface != nullptr);
    auto renderFence = std::make_shared<DeferredFence>(surface->compatibility());
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 64;

    uint64_t submittedFenceValue = 0;
    const FrameHandle handle = WinGpuImportEdge::makeGpuFrameHandleForTest(
        surface, meta, renderFence, {}, &submittedFenceValue);

    QVERIFY(handle.isGpuBacked());
    QVERIFY(handle.data() != nullptr);
    QVERIFY(handle.data()->gpuFence() == renderFence);
    QCOMPARE(submittedFenceValue, uint64_t(1));
    renderFence->complete(submittedFenceValue);
    GpuRetireRegistry{}.drainCompleted();
#endif
}

void TestWinGpuImportEdge::readToCpuDeinterleavesNv12ToI420() {
#ifndef _WIN32
    QSKIP("D3D11 readback is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");

    constexpr int kW = 8;
    constexpr int kH = 8;
    std::vector<uint8_t> nv12(size_t(kW) * kH + size_t(kW) * (kH / 2));
    uint8_t* y = nv12.data();
    uint8_t* uv = nv12.data() + size_t(kW) * kH;
    for (int r = 0; r < kH; ++r)
        for (int c = 0; c < kW; ++c)
            y[size_t(r) * kW + c] = uint8_t((r * kW + c) & 0xff);
    for (int r = 0; r < kH / 2; ++r) {
        for (int c = 0; c < kW / 2; ++c) {
            uv[size_t(r) * kW + 2 * c] = uint8_t((r * (kW / 2) + c) & 0xff);
            uv[size_t(r) * kW + 2 * c + 1] = uint8_t((128 + r * (kW / 2) + c) & 0xff);
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kW;
    desc.Height = kH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    ctx->UpdateSubresource(texture.Get(), 0, nullptr, nv12.data(), UINT(kW), 0);

    auto surface =
        D3D11GpuSurface::createKept(device, texture, 0, kW, kH, currentDeviceAuthority());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = kW;
    meta.key.height = kH;
    const FrameHandle handle = WinGpuImportEdge::makeGpuFrameHandleForTest(surface, meta);
    QVERIFY(handle.isGpuBacked());

    const CpuPlanes got = handle.readToCpu(FramePixelFormat::Yuv420p);
    QCOMPARE(int(got.format), int(FramePixelFormat::Yuv420p));
    QCOMPARE(got.width, kW);
    QCOMPARE(got.height, kH);

    AVFrame* expected = nativeCopyNv12ToYuv420p(y, kW, uv, kW, kW, kH);
    QVERIFY(expected != nullptr);
    for (int r = 0; r < kH; ++r) {
        for (int c = 0; c < kW; ++c) {
            QCOMPARE(uint8_t(got.plane[0].at(size_t(r) * got.stride[0] + c)),
                     expected->data[0][size_t(r) * expected->linesize[0] + c]);
        }
    }
    for (int r = 0; r < kH / 2; ++r) {
        for (int c = 0; c < kW / 2; ++c) {
            QCOMPARE(uint8_t(got.plane[1].at(size_t(r) * got.stride[1] + c)),
                     expected->data[1][size_t(r) * expected->linesize[1] + c]);
            QCOMPARE(uint8_t(got.plane[2].at(size_t(r) * got.stride[2] + c)),
                     expected->data[2][size_t(r) * expected->linesize[2] + c]);
        }
    }
    av_frame_free(&expected);
#endif
}

void TestWinGpuImportEdge::readToCpuWaitsForPendingFenceBeforeCopy() {
#ifndef _WIN32
    QSKIP("D3D11 readback is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");

    constexpr int kW = 8;
    constexpr int kH = 8;
    std::vector<uint8_t> nv12(size_t(kW) * kH + size_t(kW) * (kH / 2), uint8_t(128));
    std::fill(nv12.begin(), nv12.begin() + size_t(kW) * kH, uint8_t(32));

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kW;
    desc.Height = kH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    ctx->UpdateSubresource(texture.Get(), 0, nullptr, nv12.data(), UINT(kW), 0);

    auto surface =
        D3D11GpuSurface::createKept(device, texture, 0, kW, kH, currentDeviceAuthority());
    QVERIFY(surface != nullptr);
    auto fence = std::make_shared<DeferredFence>(surface->compatibility());

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = kW;
    meta.key.height = kH;
    uint64_t submittedFenceValue = 0;
    const FrameHandle handle =
        WinGpuImportEdge::makeGpuFrameHandleForTest(surface, meta, fence, {}, &submittedFenceValue);
    QCOMPARE(submittedFenceValue, uint64_t(1));

    QVERIFY(!handle.readToCpu(FramePixelFormat::Yuv420p).isValid());
    QCOMPARE(fence->waits.load(std::memory_order_acquire), 1);
    QCOMPARE(fence->lastValue.load(std::memory_order_acquire), submittedFenceValue);

    fence->complete(submittedFenceValue);
    const CpuPlanes got = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(got.isValid());
    QCOMPARE(fence->waits.load(std::memory_order_acquire), 2);
    fence->complete(2);
    GpuRetireRegistry{}.drainCompleted();
#endif
}

void TestWinGpuImportEdge::fenceSignalsAndWaits() {
#ifndef _WIN32
    QSKIP("D3D fence is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");
    QString error;
    auto fence = D3DFence::create(device.Get(), &error);
    if (!fence) QSKIP(qPrintable(error));
    const uint64_t value = fence->signal(ctx.Get());
    QVERIFY(value > 0);
    QVERIFY(fence->wait(value, 2000));
    QVERIFY(fence->completedValue() >= value);
#endif
}

void TestWinGpuImportEdge::fenceWaitForeverWaitsUntilSignal() {
#ifndef _WIN32
    QSKIP("D3D fence is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");
    QString error;
    auto fence = D3DFence::create(device.Get(), &error);
    if (!fence) QSKIP(qPrintable(error));

    bool waitResult = false;
    std::thread waiter([&] { waitResult = fence->wait(1, -1); });
    QTest::qWait(20);
    QCOMPARE(fence->signal(ctx.Get()), uint64_t(1));
    waiter.join();
    QVERIFY(waitResult);
#endif
}

void TestWinGpuImportEdge::importedReadbackMatchesCpuDecodeWithinOneLsb() {
#ifndef _WIN32
    QSKIP("Windows import slice");
#else
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    if (!caps.d3d11KeepTexture) QSKIP("host has no keep-texture path; CPU fallback only");
    QString error;
    auto edge = WinGpuImportEdge::create(&error);
    if (!edge) QSKIP("no GPU import edge on this host");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");

    constexpr int kW = 8;
    constexpr int kH = 8;
    std::vector<uint8_t> nv12(size_t(kW) * kH + size_t(kW) * (kH / 2));
    uint8_t* y = nv12.data();
    uint8_t* uv = nv12.data() + size_t(kW) * kH;
    for (int r = 0; r < kH; ++r)
        for (int c = 0; c < kW; ++c)
            y[size_t(r) * kW + c] = uint8_t((r * kW + c) & 0xff);
    for (int r = 0; r < kH / 2; ++r) {
        for (int c = 0; c < kW / 2; ++c) {
            uv[size_t(r) * kW + 2 * c] = uint8_t((r * (kW / 2) + c) & 0xff);
            uv[size_t(r) * kW + 2 * c + 1] = uint8_t((128 + r * (kW / 2) + c) & 0xff);
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kW;
    desc.Height = kH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    ctx->UpdateSubresource(texture.Get(), 0, nullptr, nv12.data(), UINT(kW), 0);

    FrameHandle imported;
    bool tapped = false;
    edge->setImportTapForTest([&](const FrameHandle& handle) {
        imported = handle;
        tapped = true;
    });
    QVERIFY(edge->decodeOneForTest(device, texture, kW, kH));
    QVERIFY(tapped);
    QVERIFY(imported.isGpuBacked());

    const CpuPlanes got = imported.readToCpu(FramePixelFormat::Yuv420p);
    AVFrame* expected = nativeCopyNv12ToYuv420p(y, kW, uv, kW, kW, kH);
    QVERIFY(expected != nullptr);

    auto withinOne = [](uint8_t a, uint8_t b) { return std::abs(int(a) - int(b)) <= 1; };
    for (int r = 0; r < kH; ++r) {
        for (int c = 0; c < kW; ++c) {
            const uint8_t act = uint8_t(got.plane[0].at(size_t(r) * got.stride[0] + c));
            const uint8_t exp = expected->data[0][size_t(r) * expected->linesize[0] + c];
            QVERIFY2(withinOne(act, exp), "Y plane exceeds +/-1 LSB");
        }
    }
    for (int r = 0; r < kH / 2; ++r) {
        for (int c = 0; c < kW / 2; ++c) {
            const uint8_t actU = uint8_t(got.plane[1].at(size_t(r) * got.stride[1] + c));
            const uint8_t actV = uint8_t(got.plane[2].at(size_t(r) * got.stride[2] + c));
            const uint8_t expU = expected->data[1][size_t(r) * expected->linesize[1] + c];
            const uint8_t expV = expected->data[2][size_t(r) * expected->linesize[2] + c];
            QVERIFY2(withinOne(actU, expU), "U plane exceeds +/-1 LSB");
            QVERIFY2(withinOne(actV, expV), "V plane exceeds +/-1 LSB");
        }
    }
    av_frame_free(&expected);
    edge->setImportTapForTest(nullptr);
#endif
}

void TestWinGpuImportEdge::allocFailureDegradesToCpuFallback() {
#ifndef _WIN32
    QSKIP("Windows micro-stress");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

    D3D11GpuSurface::setForceAllocFailureForTest(true);
    auto imported =
        D3D11GpuSurface::createKept(device, texture, 0, 64, 64, currentDeviceAuthority());
    D3D11GpuSurface::setForceAllocFailureForTest(false);
    QVERIFY(imported == nullptr);

    FrameHandle fallback = solidYuv420pHandle(64, 64, 16, 128, 128);
    QVERIFY2(!fallback.isGpuBacked(), "CPU fallback must remain available when import fails");
    QVERIFY(fallback.isValid());
#endif
}

void TestWinGpuImportEdge::surfaceSurvivesInFlightReadback() {
#ifndef _WIN32
    QSKIP("Windows micro-stress");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createTestD3D11Device(&device, &ctx)) QSKIP("no D3D11 test device available");
    constexpr int kW = 8;
    constexpr int kH = 8;
    std::vector<uint8_t> nv12(size_t(kW) * kH + size_t(kW) * (kH / 2), 0x80);
    for (int r = 0; r < kH; ++r)
        for (int c = 0; c < kW; ++c)
            nv12[size_t(r) * kW + c] = uint8_t((r * kW + c) & 0xff);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kW;
    desc.Height = kH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    ctx->UpdateSubresource(texture.Get(), 0, nullptr, nv12.data(), UINT(kW), 0);

    auto surface =
        D3D11GpuSurface::createKept(device, texture, 0, kW, kH, currentDeviceAuthority());
    QVERIFY(surface != nullptr);
    std::weak_ptr<D3D11GpuSurface> weak = surface;
    auto renderFence = std::make_shared<DeferredFence>(surface->compatibility());
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = kW;
    meta.key.height = kH;
    uint64_t submittedFenceValue = 0;
    FrameHandle handle = WinGpuImportEdge::makeGpuFrameHandleForTest(surface, meta, renderFence, {},
                                                                     &submittedFenceValue);
    QCOMPARE(submittedFenceValue, uint64_t(1));
    surface.reset();
    QVERIFY(!weak.expired());
    renderFence->complete(submittedFenceValue);
    QVERIFY(handle.readToCpu(FramePixelFormat::Yuv420p).isValid());
    handle = FrameHandle();
    QVERIFY(!weak.expired());
    QVERIFY(GpuRetireRegistry{}.pendingRetainCount() >= 1);

    renderFence->complete(2);
    GpuRetireRegistry{}.drainCompleted();
    QVERIFY(weak.expired());
#endif
}

QTEST_GUILESS_MAIN(TestWinGpuImportEdge)
#include "tst_wingpuimportedge.moc"

#include <QtTest>

#include "playback/output/win/wingpuimportedge.h"
#ifdef _WIN32
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/output/win/d3d11gpusurface.h"
#include "playback/output/win/d3dfence.h"
#include "recorder_engine/ingest/nativeframecopy.h"

#include <atomic>
#include <cstdlib>
#include <d3d11.h>
#include <thread>
#include <vector>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
}

using Microsoft::WRL::ComPtr;

namespace {

class DeferredFence final : public GpuFence {
public:
    uint64_t signal() override { return m_next.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int) override { return completedValue() >= value; }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }
    void complete(uint64_t value) { m_completed.store(value, std::memory_order_release); }

private:
    std::atomic<uint64_t> m_next{0};
    std::atomic<uint64_t> m_completed{0};
};

} // namespace
#endif

class TestWinGpuImportEdge : public QObject {
    Q_OBJECT
private slots:
    void probeIsConsistentAndNeverThrows();
    void backendConstantIsValid();
    void createConsistentWithProbe();
    void nullSampleYieldsFallbackNullopt();
    void surfaceKeepsTextureAndTracksFence();
    void readToCpuDeinterleavesNv12ToI420();
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
#ifndef _WIN32
    QVERIFY2(!caps.d3d11KeepTexture, "non-Windows must report no keep-texture");
    QVERIFY2(!caps.rhiImportable, "non-Windows must report no RHI import");
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

    auto renderFence = makeD3D11GpuFence(edge->d3d11Device());
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
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1,
                                        D3D11_SDK_VERSION, &device, &level, &ctx)));

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

    auto surface = D3D11GpuSurface::createKept(device, texture, 0, 1280, 720);
    QVERIFY(surface != nullptr);
    QVERIFY(surface->isValid());
    QCOMPARE(int(surface->desc().format), int(FramePixelFormat::Nv12));
    QCOMPARE(surface->desc().width, 1280);
    QCOMPARE(surface->desc().height, 720);
    QCOMPARE(surface->nativeHandle(), static_cast<void*>(texture.Get()));

    surface->retainUntilFenceRetired(5);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(5));
    surface->retainUntilFenceRetired(3);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(5));
#endif
}

void TestWinGpuImportEdge::readToCpuDeinterleavesNv12ToI420() {
#ifndef _WIN32
    QSKIP("D3D11 readback is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1,
                                        D3D11_SDK_VERSION, &device, &level, &ctx)));

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

    auto surface = D3D11GpuSurface::createKept(device, texture, 0, kW, kH);
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

void TestWinGpuImportEdge::fenceSignalsAndWaits() {
#ifndef _WIN32
    QSKIP("D3D fence is Windows-only");
#else
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1,
                                        D3D11_SDK_VERSION, &device, &level, &ctx)));
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
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1,
                                        D3D11_SDK_VERSION, &device, &level, &ctx)));
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
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1,
                                        D3D11_SDK_VERSION, &device, &level, &ctx)));

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
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1,
                                        D3D11_SDK_VERSION, &device, &level, &ctx)));
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
    auto imported = D3D11GpuSurface::createKept(device, texture, 0, 64, 64);
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
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &want, 1,
                                        D3D11_SDK_VERSION, &device, &level, &ctx)));
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

    auto surface = D3D11GpuSurface::createKept(device, texture, 0, kW, kH);
    QVERIFY(surface != nullptr);
    std::weak_ptr<D3D11GpuSurface> weak = surface;
    auto renderFence = std::make_shared<DeferredFence>();
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = kW;
    meta.key.height = kH;
    FrameHandle handle = WinGpuImportEdge::makeGpuFrameHandleForTest(surface, meta, renderFence);
    surface.reset();
    QVERIFY(!weak.expired());
    QVERIFY(handle.readToCpu(FramePixelFormat::Yuv420p).isValid());
    handle = FrameHandle();
    QVERIFY(!weak.expired());
    QVERIFY(gpuPendingReadbackRetainCount() >= 1);

    renderFence->complete(1);
    gpuDrainCompletedReadbackRetains();
    QVERIFY(weak.expired());
#endif
}

QTEST_GUILESS_MAIN(TestWinGpuImportEdge)
#include "tst_wingpuimportedge.moc"

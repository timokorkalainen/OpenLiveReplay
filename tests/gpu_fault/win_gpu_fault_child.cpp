#include "tests/gpu_fault/win_gpu_fault_protocol.h"

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/win/d3d11gpusurface.h"
#include "playback/output/win/wingpuimportedge.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QThread>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace {

struct HardwareDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    DXGI_ADAPTER_DESC1 adapter{};
};

bool createHardwareDevice(HardwareDevice* out, QString* error) {
    if (GetSystemMetrics(SM_REMOTESESSION)) {
        if (error) *error = QStringLiteral("remote-session");
        return false;
    }
    D3D_FEATURE_LEVEL created{};
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                          levels, 2, D3D11_SDK_VERSION, &out->device, &created, &out->context);
    if (FAILED(hr) || !out->device || !out->context) {
        if (error)
            *error = QStringLiteral("hardware D3D11 device unavailable (0x%1)")
                         .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter1> adapter1;
    if (FAILED(out->device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter.As(&adapter1)) || FAILED(adapter1->GetDesc1(&out->adapter)) ||
        (out->adapter.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
        if (error) *error = QStringLiteral("selected adapter is software or has no DXGI identity");
        return false;
    }
    const UINT vendor = out->adapter.VendorId;
    if (vendor == 0x1414 || vendor == 0x15ad || vendor == 0x80ee || vendor == 0x1ab8) {
        if (error) *error = QStringLiteral("virtual display adapter is not eligible");
        return false;
    }
    if (qEnvironmentVariableIsSet("OLR_GPU_FAULT_EXPECTED_LUID_HIGH") &&
        (qEnvironmentVariableIntValue("OLR_GPU_FAULT_EXPECTED_LUID_HIGH") !=
             out->adapter.AdapterLuid.HighPart ||
         qEnvironmentVariable("OLR_GPU_FAULT_EXPECTED_LUID_LOW").toULongLong() !=
             out->adapter.AdapterLuid.LowPart)) {
        if (error) *error = QStringLiteral("adapter LUID changed after capability admission");
        return false;
    }
    return true;
}

bool supportsFence(ID3D11Device* device) {
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11Fence> fence;
    return device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
           SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
}

class MeasuredFence final : public GpuFence {
public:
    explicit MeasuredFence(std::shared_ptr<GpuFence> inner) : m_inner(std::move(inner)) {}

    uint64_t signal() override {
        ++m_signalCount;
        return m_inner ? m_inner->signal() : 0;
    }
    bool wait(uint64_t value, int timeoutMs) override {
        ++m_waitCount;
        return m_inner && m_inner->wait(value, timeoutMs);
    }
    uint64_t completedValue() const override { return m_inner ? m_inner->completedValue() : 0; }
    int signalCount() const { return m_signalCount; }
    int waitCount() const { return m_waitCount; }

private:
    std::shared_ptr<GpuFence> m_inner;
    int m_signalCount = 0;
    int m_waitCount = 0;
};

ComPtr<ID3D11ComputeShader> compileShader(ID3D11Device* device, const QByteArray& source,
                                          QString* error) {
    if (!device || source.isEmpty()) return {};
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT hr =
        D3DCompile(source.constData(), size_t(source.size()), nullptr, nullptr, nullptr, "main",
                   "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &diagnostics);
    if (FAILED(hr)) {
        if (error) {
            *error =
                diagnostics
                    ? QString::fromUtf8(static_cast<const char*>(diagnostics->GetBufferPointer()),
                                        int(diagnostics->GetBufferSize()))
                    : QStringLiteral("D3DCompile failed (0x%1)")
                          .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        }
        return {};
    }
    ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                           nullptr, &shader))) {
        if (error) *error = QStringLiteral("CreateComputeShader failed");
        return {};
    }
    return shader;
}

QByteArray loadShader(const QString& name, QString* error) {
    QFile file(QStringLiteral(OLR_GPU_FAULT_SOURCE_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read fault shader %1").arg(file.fileName());
        return {};
    }
    return file.readAll();
}

bool createDispatchResources(HardwareDevice& gpu, ComPtr<ID3D11UnorderedAccessView>* uav,
                             std::shared_ptr<D3D11GpuSurface>* surface, QString* error) {
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(uint32_t);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = sizeof(uint32_t);
    const uint32_t initial = 0;
    D3D11_SUBRESOURCE_DATA initialData{&initial, 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = 1;
    if (FAILED(gpu.device->CreateBuffer(&bufferDesc, &initialData, &buffer)) ||
        FAILED(
            gpu.device->CreateUnorderedAccessView(buffer.Get(), &uavDesc, uav->GetAddressOf()))) {
        if (error) *error = QStringLiteral("compute UAV allocation failed");
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = 16;
    textureDesc.Height = 16;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_NV12;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(gpu.device->CreateTexture2D(&textureDesc, nullptr, &texture))) {
        if (error) *error = QStringLiteral("retirement surface allocation failed");
        return false;
    }
    *surface = D3D11GpuSurface::createKept(gpu.device, texture, 0, 16, 16);
    return *surface != nullptr;
}

void emitObject(const QJsonObject& object) {
    const QByteArray line = winGpuFault::encode(object);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

int capability() {
    QJsonObject evidence{{QStringLiteral("mode"), QStringLiteral("capability")}};
    if (GetSystemMetrics(SM_REMOTESESSION)) {
        evidence.insert(QStringLiteral("supported"), false);
        evidence.insert(QStringLiteral("reason"), QStringLiteral("remote-session"));
        emitObject(evidence);
        return 0;
    }

    HardwareDevice gpu;
    QString error;
    if (!createHardwareDevice(&gpu, &error) || !supportsFence(gpu.device.Get())) {
        evidence.insert(QStringLiteral("supported"), false);
        evidence.insert(QStringLiteral("reason"),
                        error.isEmpty() ? QStringLiteral("no-d3d11-fence") : error);
        emitObject(evidence);
        return 0;
    }

    evidence.insert(QStringLiteral("supported"), true);
    evidence.insert(QStringLiteral("destructiveAllowed"), true);
    evidence.insert(QStringLiteral("adapter"), QString::fromWCharArray(gpu.adapter.Description));
    evidence.insert(QStringLiteral("vendorId"), int(gpu.adapter.VendorId));
    evidence.insert(QStringLiteral("deviceId"), int(gpu.adapter.DeviceId));
    evidence.insert(QStringLiteral("luidHigh"), int(gpu.adapter.AdapterLuid.HighPart));
    evidence.insert(QStringLiteral("luidLow"), double(gpu.adapter.AdapterLuid.LowPart));
    emitObject(evidence);
    return 0;
}

int probeFence() {
    HardwareDevice gpu;
    QString error;
    if (!createHardwareDevice(&gpu, &error)) return 2;
    const auto shader = compileShader(
        gpu.device.Get(), loadShader(QStringLiteral("win_gpu_long_dispatch.hlsl"), &error), &error);
    ComPtr<ID3D11UnorderedAccessView> uav;
    std::shared_ptr<D3D11GpuSurface> surface;
    if (!shader || !createDispatchResources(gpu, &uav, &surface, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 3;
    }
    const auto nativeFence = makeD3D11GpuFence(gpu.device.Get());
    if (!nativeFence) return 4;
    auto fence = std::make_shared<MeasuredFence>(nativeFence);

    gpu.context->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* rawUav = uav.Get();
    gpu.context->CSSetUnorderedAccessViews(0, 1, &rawUav, nullptr);
    gpu.context->Dispatch(1, 1, 1);

    GpuRetireRegistry registry;
    GpuOpScope operation(fence, registry);
    operation.track(surface);
    if (!operation.submit([] { return GpuSubmitOutcome::Submitted; })) return 5;
    const uint64_t signalValue = operation.fenceValue();
    const uint64_t initialCompleted = fence->completedValue();
    const qsizetype pendingInitially = registry.pendingRetainCount();
    const bool waited = fence->wait(signalValue, 15000);
    const uint64_t finalCompleted = fence->completedValue();
    registry.drainCompleted();

    emitObject({{QStringLiteral("mode"), QStringLiteral("probe-fence")},
                {QStringLiteral("signalCount"), fence->signalCount()},
                {QStringLiteral("signalValue"), double(signalValue)},
                {QStringLiteral("initialCompleted"), double(initialCompleted)},
                {QStringLiteral("pendingInitially"), int(pendingInitially)},
                {QStringLiteral("waited"), waited},
                {QStringLiteral("finalCompleted"), double(finalCompleted)},
                {QStringLiteral("pendingFinally"), int(registry.pendingRetainCount())},
                {QStringLiteral("waitCount"), fence->waitCount()}});
    return 0;
}

int triggerTdr() {
    HardwareDevice gpu;
    QString error;
    if (!createHardwareDevice(&gpu, &error) || !supportsFence(gpu.device.Get())) return 2;
    const auto shader = compileShader(
        gpu.device.Get(), loadShader(QStringLiteral("win_gpu_tdr_dispatch.hlsl"), &error), &error);
    ComPtr<ID3D11UnorderedAccessView> uav;
    std::shared_ptr<D3D11GpuSurface> surface;
    if (!shader || !createDispatchResources(gpu, &uav, &surface, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 3;
    }
    const auto nativeFence = makeD3D11GpuFence(gpu.device.Get());
    if (!nativeFence) return 4;
    auto fence = std::make_shared<MeasuredFence>(nativeFence);

    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t generationBefore = GpuGenerationCounter::instance().current();
    FrameMetadata preLossMetadata;
    preLossMetadata.key.format = FramePixelFormat::Nv12;
    preLossMetadata.key.width = 16;
    preLossMetadata.key.height = 16;
    preLossMetadata.gpuGeneration = generationBefore;
    FrameHandle preLossFrame =
        WinGpuImportEdge::makeGpuFrameHandleForTest(surface, preLossMetadata, nullptr);
    gpu.context->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* rawUav = uav.Get();
    gpu.context->CSSetUnorderedAccessViews(0, 1, &rawUav, nullptr);
    gpu.context->Dispatch(1, 1, 1);

    GpuRetireRegistry registry;
    GpuOpScope operation(fence, registry);
    operation.track(surface);
    (void) operation.submit([] { return GpuSubmitOutcome::Submitted; });
    const qsizetype pendingInitially = registry.pendingRetainCount();
    gpu.context->Flush();

    QElapsedTimer timer;
    timer.start();
    D3D11RemovalObservationForTest removal;
    while (timer.elapsed() < 30000 && removal.hresult >= 0) {
        QThread::msleep(50);
        removal = GpuRhiContext::observeD3D11RemovalForTest(gpu.device.Get(), generationBefore);
    }
    if (removal.hresult >= 0 || removal.generation == 0) return 6;

    const uint64_t generationAfter = removal.generation;
    const auto token = monitor.realLossToken();
    const bool staleFrameRejected = preLossFrame.isStaleForGeneration(generationAfter);
    preLossFrame = FrameHandle{};
    qsizetype released = 0;
    if (token) released = registry.abandonAllNoWait(*token);
    emitObject(
        {{QStringLiteral("mode"), QStringLiteral("trigger-tdr")},
         {QStringLiteral("destructiveStarted"), true},
         {QStringLiteral("removedHresult"), double(removal.hresult)},
         {QStringLiteral("generationBefore"), double(generationBefore)},
         {QStringLiteral("generationAfter"), double(generationAfter)},
         {QStringLiteral("realLossToken"), token.has_value()},
         {QStringLiteral("tokenGeneration"), token ? double(token->observedGeneration()) : 0.0},
         {QStringLiteral("staleFrameRejected"), staleFrameRejected},
         {QStringLiteral("signalCount"), fence->signalCount()},
         {QStringLiteral("pendingInitially"), int(pendingInitially)},
         {QStringLiteral("releasedRetains"), int(released)},
         {QStringLiteral("deadFenceWaits"), fence->waitCount()},
         {QStringLiteral("pendingFinally"), int(registry.pendingRetainCount())},
         {QStringLiteral("elapsedMs"), double(timer.elapsed())}});
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--capability"))) return capability();
    if (args.contains(QStringLiteral("--probe-fence"))) return probeFence();
    if (args.contains(QStringLiteral("--trigger-tdr"))) {
        const QString token = qEnvironmentVariable("OLR_GPU_FAULT_CHILD_TOKEN");
        if (qEnvironmentVariableIntValue("OLR_GPU_FAULT_LANE") != 1 || token.isEmpty() ||
            !args.contains(QStringLiteral("--parent-token=") + token)) {
            std::fprintf(stderr, "destructive child requires parent authorization\n");
            return 65;
        }
        return triggerTdr();
    }
    return 64;
}

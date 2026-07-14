#include "tests/gpu_fault/win_gpu_fault_worker_oracle.h"

#include "playback/frameprovider.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/outputruntime.h"
#include "playback/output/outputsink.h"
#include "playback/output/win/d3d11gpusurface.h"
#include "playback/output/win/wingpuimportedge.h"
#include "playback/playbacktransport.h"
#include "playback/playbackworker.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

class OracleSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }
    bool start(const OutputTargetAssignment&, FrameRate) override {
        QMutexLocker locker(&m_mutex);
        m_active = true;
        return true;
    }
    void stop() override {
        QMutexLocker locker(&m_mutex);
        m_active = false;
    }
    bool isActive() const override {
        QMutexLocker locker(&m_mutex);
        return m_active;
    }
    bool submit(const OutputBusFrame& frame) override {
        QMutexLocker locker(&m_mutex);
        ++m_submitCount;
        m_lastFrame = frame.video;
        return !frame.video.isNull();
    }
    int submitCount() const {
        QMutexLocker locker(&m_mutex);
        return m_submitCount;
    }
    FrameHandle lastFrame() const {
        QMutexLocker locker(&m_mutex);
        return m_lastFrame;
    }
    void clearLastFrame() {
        QMutexLocker locker(&m_mutex);
        m_lastFrame = FrameHandle{};
    }

private:
    mutable QMutex m_mutex;
    bool m_active = false;
    int m_submitCount = 0;
    FrameHandle m_lastFrame;
};

OutputTargetAssignment oracleAssignment() {
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("gpu-fault-worker-oracle");
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    return assignment;
}

bool deviceAndLuid(const std::shared_ptr<GpuRhiContext>& rhi, ComPtr<ID3D11Device>* device,
                   LUID* luid) {
    if (!rhi || !device || !luid) return false;
    return rhi->invokeOnRenderThread([&](QRhi* qrhi) {
        const auto* handles =
            qrhi ? static_cast<const QRhiD3D11NativeHandles*>(qrhi->nativeHandles()) : nullptr;
        auto* rawDevice = handles ? static_cast<ID3D11Device*>(handles->dev) : nullptr;
        if (!rawDevice) return;
        *device = rawDevice;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(rawDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter &&
            SUCCEEDED(adapter->GetDesc(&desc)))
            *luid = desc.AdapterLuid;
    }) && *device;
}

bool sameCpuPlanes(const CpuPlanes& left, const CpuPlanes& right) {
    if (!left.isValid() || !right.isValid() || left.format != right.format ||
        left.width != right.width || left.height != right.height)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (left.stride[i] != right.stride[i] || left.plane[i] != right.plane[i]) return false;
    }
    return true;
}

} // namespace

struct WinGpuFaultWorkerOracle::Impl {
    OracleSink sink; // Outlives worker and its non-owning endpoint pointer.
    FrameProvider feed;
    PlaybackTransport transport;
    std::unique_ptr<PlaybackWorker> worker;
    std::shared_ptr<GpuRhiContext> preLossRhi;
    std::shared_ptr<D3D11GpuSurface> preLossSurface;
    std::weak_ptr<GpuSurface> workerRetainedSurface;
    FrameMetadata expectedMetadata;
    CpuPlanes expectedCpu;
    uint64_t generationBefore = 0;
    uint64_t deviceAuthorityEpoch = 0;
    ComPtr<ID3D11Device> device;
};

WinGpuFaultWorkerOracle::WinGpuFaultWorkerOracle() : m_impl(std::make_unique<Impl>()) {}
WinGpuFaultWorkerOracle::~WinGpuFaultWorkerOracle() = default;

bool WinGpuFaultWorkerOracle::prepare(const QJsonObject& capability, QString* error) {
    qputenv("OLR_GPU_PIPELINE", QByteArrayLiteral("1"));
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();
    m_impl->transport.setFrameRate(25, 1);
    m_impl->transport.seek(0);
    m_impl->transport.setPlaying(false);
    m_impl->worker =
        std::make_unique<PlaybackWorker>(QList<FrameProvider*>{&m_impl->feed}, &m_impl->transport);
    m_impl->worker->initializeOutputGraph(1, 16, 16);
    const auto gpuRhi =
        std::atomic_load_explicit(&m_impl->worker->m_gpuRhi, std::memory_order_acquire);
    if (m_impl->worker->gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu || !gpuRhi) {
        if (error) *error = QStringLiteral("PlaybackWorker failed to create a hardware GPU spine");
        return false;
    }
    m_impl->preLossRhi = gpuRhi;
    m_impl->deviceAuthorityEpoch = GpuRhiContext::captureD3D11RemovalAuthorityForTest();

    LUID workerLuid{};
    if (!deviceAndLuid(m_impl->preLossRhi, &m_impl->device, &workerLuid)) {
        if (error) *error = QStringLiteral("cannot inspect PlaybackWorker D3D11 device");
        return false;
    }
    if (workerLuid.HighPart != capability.value(QStringLiteral("luidHigh")).toInt() ||
        workerLuid.LowPart != DWORD(capability.value(QStringLiteral("luidLow")).toDouble())) {
        if (error)
            *error = QStringLiteral("PlaybackWorker and fault child selected different adapters");
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
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    QByteArray textureBytes(16 * 16 * 3 / 2, char(0x2a));
    for (int offset = 16 * 16; offset < textureBytes.size(); offset += 2) {
        textureBytes[offset] = char(0x35);
        textureBytes[offset + 1] = char(0xc7);
    }
    D3D11_SUBRESOURCE_DATA textureData{};
    textureData.pSysMem = textureBytes.constData();
    textureData.SysMemPitch = 16;
    textureData.SysMemSlicePitch = UINT(textureBytes.size());
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(m_impl->device->CreateTexture2D(&textureDesc, &textureData, &texture))) {
        if (error) *error = QStringLiteral("cannot create worker-oracle NV12 surface");
        return false;
    }
    m_impl->preLossSurface = D3D11GpuSurface::createKept(m_impl->device, texture, 0, 16, 16,
                                                         m_impl->deviceAuthorityEpoch);
    FrameMetadata metadata;
    metadata.key.feedIndex = 0;
    metadata.key.ptsMs = 0;
    metadata.key.videoHash = 0xa17e51d3u;
    metadata.key.format = FramePixelFormat::Nv12;
    metadata.key.width = 16;
    metadata.key.height = 16;
    metadata.outputFrameIndex = 13;
    metadata.sampledPlayheadMs = 37;
    metadata.decodedSequence = 991;
    m_impl->generationBefore = GpuGenerationCounter::instance().current();
    metadata.gpuGeneration = m_impl->generationBefore;
    m_impl->expectedMetadata = metadata;
    FrameHandle preLossFrame =
        WinGpuImportEdge::makeGpuFrameHandleForTest(m_impl->preLossSurface, metadata, nullptr);
    const bool readOnRenderThread = m_impl->preLossRhi->invokeOnRenderThread(
        [&](QRhi*) { m_impl->expectedCpu = preLossFrame.readToCpu(FramePixelFormat::Yuv420p); });
    if (!readOnRenderThread || preLossFrame.isNull() || !m_impl->expectedCpu.isValid()) {
        if (error)
            *error = QStringLiteral("worker-oracle GPU frame could not cache a CPU fallback");
        return false;
    }

    {
        QMutexLocker locker(&m_impl->worker->m_bufferMutex);
        m_impl->worker->m_outputCache->insertVideoFrame(preLossFrame);
        m_impl->worker->publishOutputCacheLocked();
    }
    OutputRuntime* runtime = nullptr;
    {
        QMutexLocker locker(&m_impl->worker->m_outputRuntimeMutex);
        runtime = m_impl->worker->m_outputRuntime.get();
    }
    if (!runtime) {
        if (error) *error = QStringLiteral("worker-oracle output runtime is unavailable");
        return false;
    }
    // Never hold m_outputRuntimeMutex while dispatching: the snapshot provider
    // re-enters PlaybackWorker and takes that mutex to read the frame index.
    runtime->setIdentitySkip(false);
    runtime->setEndpoints({{oracleAssignment(), &m_impl->sink}});
    runtime->dispatchImmediate();
    if (m_impl->sink.submitCount() < 1 || m_impl->sink.lastFrame().isNull()) {
        if (error) *error = QStringLiteral("worker-oracle produced no pre-loss output");
        return false;
    }
    m_impl->sink.clearLastFrame();
    return true;
}

void* WinGpuFaultWorkerOracle::d3dDevice() const {
    return m_impl ? m_impl->device.Get() : nullptr;
}

bool WinGpuFaultWorkerOracle::invokeOnRenderThread(
    const std::function<void(void*)>& operation) const {
    if (!m_impl || !m_impl->preLossRhi || !operation) return false;
    return m_impl->preLossRhi->invokeOnRenderThread([&](QRhi* rhi) {
        const auto* handles =
            rhi ? static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles()) : nullptr;
        operation(handles ? handles->context : nullptr);
    });
}

bool WinGpuFaultWorkerOracle::submitWorkerFaultOperation(
    const std::shared_ptr<GpuFence>& fence, const std::function<bool(void*, void*)>& dispatch,
    qsizetype* pendingRetains, QString* error) {
    if (!m_impl || !m_impl->worker || !m_impl->preLossSurface || !fence || !dispatch) return false;

    FrameHandle faultFrame;
    uint64_t submittedFenceValue = 0;
    bool operationSubmitted = false;
    const bool invoked = invokeOnRenderThread([&](void* rawContext) {
        GpuSyncReadScope readScope;
        const GpuReadLease lease = readScope.read(m_impl->preLossSurface);
        operationSubmitted =
            lease.valid() && lease.nativeHandle() && dispatch(rawContext, lease.nativeHandle());
        if (!operationSubmitted) return;
        faultFrame = WinGpuImportEdge::makeGpuFrameHandleWithCachedCpuForTest(
            m_impl->preLossSurface, m_impl->expectedMetadata, fence, {}, &submittedFenceValue,
            m_impl->expectedCpu);
        auto* context = static_cast<ID3D11DeviceContext*>(rawContext);
        if (context) context->Flush();
    });
    const bool cachedCpuAttached =
        faultFrame.data() &&
        faultFrame.data()->cachedCpuPlanes(FramePixelFormat::Yuv420p).isValid();
    if (!invoked || !operationSubmitted || faultFrame.isNull() || submittedFenceValue == 0 ||
        !cachedCpuAttached) {
        if (error) *error = QStringLiteral("worker failed to register the fault operation retain");
        return false;
    }

    m_impl->workerRetainedSurface = m_impl->preLossSurface;
    {
        QMutexLocker locker(&m_impl->worker->m_bufferMutex);
        m_impl->worker->m_outputCache->insertVideoFrame(faultFrame);
        m_impl->worker->publishOutputCacheLocked();
    }
    faultFrame = FrameHandle{};
    m_impl->preLossSurface.reset();
    if (pendingRetains) *pendingRetains = GpuRetireRegistry{}.pendingRetainCount();
    return true;
}

bool WinGpuFaultWorkerOracle::recover(QJsonObject* evidence, QString* error) {
    if (!m_impl->worker || !m_impl->preLossRhi || !evidence) return false;
    (void) m_impl->preLossRhi->importAndReadback(nullptr, FramePixelFormat::Yuv420p);
    const auto token = GpuDeviceLossMonitor::instance().realLossToken();
    if (!token || !m_impl->preLossRhi->deviceLost()) {
        if (error) *error = QStringLiteral("PlaybackWorker RHI did not publish real DXGI loss");
        return false;
    }

    const uint64_t generationAfter = GpuGenerationCounter::instance().current();
    bool staleRejected = false;
    {
        const OutputRuntimeSnapshot staleSnapshot = m_impl->worker->makeOutputSnapshot();
        const auto staleFrame = staleSnapshot.cache.videoFrameAt(0, 0);
        staleRejected = staleFrame.has_value() && staleFrame->isGpuBacked() &&
                        staleFrame->isStaleForGeneration(generationAfter);
    }

    QElapsedTimer recoveryTimer;
    recoveryTimer.start();
    m_impl->worker->handleGpuDeviceLoss();
    const qint64 recoveryMs = recoveryTimer.elapsed();

    const OutputRuntimeSnapshot snapshot = m_impl->worker->makeOutputSnapshot();
    const auto recovered = snapshot.cache.videoFrameAt(0, 0);
    const auto recoveredMetadataMatches = [&](const FrameMetadata& metadata) {
        FramePayloadKey expectedKey = m_impl->expectedMetadata.key;
        expectedKey.format = m_impl->expectedCpu.format;
        return metadata.key.samePayloadAs(expectedKey) &&
               metadata.outputFrameIndex == m_impl->expectedMetadata.outputFrameIndex &&
               metadata.sampledPlayheadMs == m_impl->expectedMetadata.sampledPlayheadMs &&
               metadata.decodedSequence == m_impl->expectedMetadata.decodedSequence &&
               metadata.color == m_impl->expectedMetadata.color &&
               metadata.stride[0] == m_impl->expectedCpu.stride[0] &&
               metadata.stride[1] == m_impl->expectedCpu.stride[1] &&
               metadata.stride[2] == m_impl->expectedCpu.stride[2];
    };
    const bool cacheRecovered = recovered.has_value() && !recovered->isGpuBacked() &&
                                recovered->metadata().gpuGeneration == 0 &&
                                recoveredMetadataMatches(recovered->metadata());
    const int submitsBefore = m_impl->sink.submitCount();
    OutputRuntime* runtime = nullptr;
    {
        QMutexLocker locker(&m_impl->worker->m_outputRuntimeMutex);
        runtime = m_impl->worker->m_outputRuntime.get();
    }
    if (!runtime) {
        if (error) *error = QStringLiteral("worker-oracle output runtime vanished after recovery");
        return false;
    }
    runtime->setIdentitySkip(false);
    runtime->setEndpoints({{oracleAssignment(), &m_impl->sink}});
    runtime->dispatchImmediate();
    runtime->setEndpoints({});
    const FrameHandle lastFrame = m_impl->sink.lastFrame();
    const CpuPlanes lastCpu = lastFrame.readToCpu(FramePixelFormat::Yuv420p);
    const bool outputResumed =
        m_impl->sink.submitCount() > submitsBefore && !lastFrame.isNull() &&
        !lastFrame.isGpuBacked() && !lastFrame.metadata().key.isPlaceholder && recovered &&
        lastFrame.metadata().key.samePayloadAs(recovered->metadata().key) &&
        lastFrame.metadata().sampledPlayheadMs == recovered->metadata().sampledPlayheadMs &&
        lastFrame.metadata().decodedSequence == recovered->metadata().decodedSequence &&
        lastFrame.metadata().color == recovered->metadata().color &&
        !lastFrame.isStaleForGeneration(generationAfter) &&
        sameCpuPlanes(lastCpu, m_impl->expectedCpu);
    const bool coherentState =
        m_impl->worker->gpuPipelineState() == PlaybackWorker::GpuPipelineState::Gpu ||
        m_impl->worker->gpuPipelineState() == PlaybackWorker::GpuPipelineState::CpuFallback;

    evidence->insert(QStringLiteral("workerRecoveryExercised"), true);
    evidence->insert(QStringLiteral("workerRealLossToken"), true);
    evidence->insert(QStringLiteral("workerGenerationBefore"), double(m_impl->generationBefore));
    evidence->insert(QStringLiteral("workerGenerationAfter"), double(generationAfter));
    evidence->insert(QStringLiteral("workerStaleFrameRejected"), staleRejected);
    evidence->insert(QStringLiteral("workerCacheRecoveredToCpu"), cacheRecovered);
    evidence->insert(QStringLiteral("workerOutputResumed"), outputResumed);
    evidence->insert(QStringLiteral("workerRecoveryMs"), double(recoveryMs));
    evidence->insert(QStringLiteral("workerCoherentState"), coherentState);
    evidence->insert(
        QStringLiteral("workerAbandonedRetains"),
        double(m_impl->worker->m_gpuLastAbandonedRetainsForTest.load(std::memory_order_acquire)));
    const bool workerRetainedSurfaceReleased = m_impl->workerRetainedSurface.expired();
    evidence->insert(QStringLiteral("workerRetainedSurfaceReleased"),
                     workerRetainedSurfaceReleased);

    if (generationAfter <= m_impl->generationBefore || !staleRejected || !cacheRecovered ||
        !outputResumed || !coherentState || !workerRetainedSurfaceReleased || recoveryMs > 10000) {
        if (error) {
            *error =
                QStringLiteral(
                    "PlaybackWorker recovery oracle failed: generation=%1 stale=%2 cache=%3 "
                    "recovered=%4 recoveredGpu=%5 recoveredGeneration=%6 metadata=%7 output=%8 "
                    "coherent=%9 released=%10 ms=%11")
                    .arg(generationAfter > m_impl->generationBefore)
                    .arg(staleRejected)
                    .arg(cacheRecovered)
                    .arg(recovered.has_value())
                    .arg(recovered && recovered->isGpuBacked())
                    .arg(recovered ? recovered->metadata().gpuGeneration : UINT64_MAX)
                    .arg(recovered && recoveredMetadataMatches(recovered->metadata()))
                    .arg(outputResumed)
                    .arg(coherentState)
                    .arg(workerRetainedSurfaceReleased)
                    .arg(recoveryMs);
        }
        return false;
    }
    return true;
}

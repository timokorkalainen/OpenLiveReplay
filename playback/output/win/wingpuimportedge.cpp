#include "playback/output/win/wingpuimportedge.h"

#ifdef _WIN32

#include "playback/output/win/d3d11gpusurface.h"

#include "recorder_engine/ingest/nativeframecopy.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d10.h>
#include <d3d11.h>
#include <functional>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <objbase.h>
#include <utility>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
}

using Microsoft::WRL::ComPtr;

namespace {

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

class D3D11IGpuFrameData final : public IFrameData {
public:
    explicit D3D11IGpuFrameData(std::shared_ptr<D3D11GpuSurface> surface)
        : m_surface(std::move(surface)) {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override;
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }

private:
    std::shared_ptr<D3D11GpuSurface> m_surface;
    mutable QMutex m_cacheMutex;
    mutable QHash<int, CpuPlanes> m_cpuCache;
};

} // namespace

WinGpuImportCapabilities probeWinGpuImport() {
    WinGpuImportCapabilities caps;
    caps.backend = QString::fromLatin1(kWinRhiBackend);

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coOwned = SUCCEEDED(coHr);
    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mfHr)) {
        caps.detail = hresultString("MFStartup", mfHr);
        if (coOwned) CoUninitialize();
        return caps;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    QString detail;
    if (!createD3D11(&device, &manager, &resetToken, &detail)) {
        caps.detail = detail;
        MFShutdown();
        if (coOwned) CoUninitialize();
        return caps;
    }

    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT enumHr =
        MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                  MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_LOCALMFT, &input,
                  nullptr, &activates, &count);
    bool d3dAware = false;
    if (SUCCEEDED(enumHr) && count > 0 && activates && activates[0]) {
        ComPtr<IMFTransform> transform;
        if (SUCCEEDED(activates[0]->ActivateObject(IID_PPV_ARGS(&transform)))) {
            ComPtr<IMFAttributes> attrs;
            UINT32 aware = 0;
            if (SUCCEEDED(transform->GetAttributes(&attrs)) && attrs &&
                SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware) {
                const HRESULT setHr = transform->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get()));
                d3dAware = SUCCEEDED(setHr);
            }
        }
    }

    if (activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }

    caps.d3d11KeepTexture = d3dAware;
    caps.rhiImportable = d3dAware;
    caps.detail =
        d3dAware ? QStringLiteral("MF H.264 decoder is D3D11-aware; keep-texture path available")
                 : QStringLiteral("MF H.264 decoder is not D3D11-aware on this host; CPU fallback");

    MFShutdown();
    if (coOwned) CoUninitialize();
    return caps;
}

struct WinGpuImportEdge::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    bool coOwned = false;
    bool mfStarted = false;
    std::function<void(const FrameHandle&)> importTap;
};

WinGpuImportEdge::WinGpuImportEdge() : m_impl(std::make_unique<Impl>()) {}

WinGpuImportEdge::~WinGpuImportEdge() {
    if (m_impl && m_impl->mfStarted) MFShutdown();
    if (m_impl && m_impl->coOwned) CoUninitialize();
}

std::unique_ptr<WinGpuImportEdge> WinGpuImportEdge::create(QString* error) {
    const WinGpuImportCapabilities caps = probeWinGpuImport();
    if (!caps.d3d11KeepTexture) {
        if (error) *error = caps.detail;
        return nullptr;
    }

    auto edge = std::unique_ptr<WinGpuImportEdge>(new WinGpuImportEdge());
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        if (error) *error = hresultString("CoInitializeEx", coHr);
        return nullptr;
    }
    edge->m_impl->coOwned = SUCCEEDED(coHr);

    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mfHr)) {
        if (error) *error = hresultString("MFStartup", mfHr);
        return nullptr;
    }
    edge->m_impl->mfStarted = true;

    QString detail;
    if (!createD3D11(&edge->m_impl->device, &edge->m_impl->manager, &edge->m_impl->resetToken,
                     &detail)) {
        if (error) *error = detail;
        return nullptr;
    }
    return edge;
}

bool WinGpuImportEdge::isAvailable() const {
    return m_impl && m_impl->device;
}

std::optional<FrameHandle> WinGpuImportEdge::tryImport(void* mfSampleOpaque, int feedIndex,
                                                       qint64 ptsMs, int width, int height) {
    if (!isAvailable() || !mfSampleOpaque || width <= 0 || height <= 0) return std::nullopt;

    auto* sample = static_cast<IMFSample*>(mfSampleOpaque);
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) || !buffer) return std::nullopt;

    ComPtr<IMFDXGIBuffer> dxgi;
    if (FAILED(buffer.As(&dxgi)) || !dxgi) return std::nullopt;

    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture))) || !texture) return std::nullopt;
    dxgi->GetSubresourceIndex(&subresource);

    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    auto surface = D3D11GpuSurface::createKept(textureDevice ? textureDevice : m_impl->device,
                                               texture, subresource, width, height);
    if (!surface) return std::nullopt;

    FrameMetadata meta;
    meta.key.feedIndex = feedIndex;
    meta.key.ptsMs = ptsMs;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = width;
    meta.key.height = height;
    return makeGpuFrameHandleForTest(std::move(surface), meta);
}

FrameHandle WinGpuImportEdge::makeGpuFrameHandleForTest(std::shared_ptr<D3D11GpuSurface> surface,
                                                        FrameMetadata meta) {
    if (!surface) return FrameHandle();
    if (meta.key.width <= 0) meta.key.width = surface->desc().width;
    if (meta.key.height <= 0) meta.key.height = surface->desc().height;
    meta.key.format = FramePixelFormat::Nv12;
    auto data = std::make_shared<D3D11IGpuFrameData>(std::move(surface));
    return FrameHandle(std::move(data), meta);
}

void WinGpuImportEdge::setImportTapForTest(std::function<void(const FrameHandle&)> tap) {
    if (!m_impl) return;
    m_impl->importTap = std::move(tap);
}

void* WinGpuImportEdge::d3d11Device() const {
    return (m_impl && m_impl->device) ? m_impl->device.Get() : nullptr;
}

bool WinGpuImportEdge::decodeOneForTest(ComPtr<ID3D11Device> device, ComPtr<ID3D11Texture2D> nv12,
                                        int width, int height) {
    auto surface =
        D3D11GpuSurface::createKept(std::move(device), std::move(nv12), 0, width, height);
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

    ID3D11Device* device = m_surface->device();
    ID3D11Texture2D* src = m_surface->texture();
    if (!device || !src) return out;

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx) return out;

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
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &readable))) return out;
    ctx->CopySubresourceRegion(readable.Get(), 0, 0, 0, 0, src, m_surface->subresource(), nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return out;

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
            out.plane[0] =
                QByteArray(reinterpret_cast<const char*>(frame->data[0]), frame->linesize[0] * h);
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
    if (out.isValid()) m_cpuCache.insert(int(target), out);
    return out;
}

#endif // _WIN32

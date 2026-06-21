// Phase-0 probe P0.2 / D4: on Windows, report the available Qt RHI D3D
// backend and prove that an ID3D11Texture2D can be wrapped as a QRhiTexture.
// Measurement only; the product does not link this target.
#include <QGuiApplication>
#include <QSize>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

std::unique_ptr<QRhi> createD3D11Rhi() {
    QRhiD3D11InitParams params;
    return std::unique_ptr<QRhi>(QRhi::create(QRhi::D3D11, &params));
}

bool probeBackend(QRhi::Implementation backend, const char* name) {
    QRhiD3D11InitParams d3d11Params;
    QRhiD3D12InitParams d3d12Params;
    QRhiInitParams* params = backend == QRhi::D3D11 ? static_cast<QRhiInitParams*>(&d3d11Params)
                                                    : static_cast<QRhiInitParams*>(&d3d12Params);

    std::unique_ptr<QRhi> rhi(QRhi::create(backend, params));
    std::printf("backend %s: %s\n", name, rhi ? "available" : "unavailable");
    return rhi != nullptr;
}

bool wrapD3D11Texture() {
    std::unique_ptr<QRhi> rhi = createD3D11Rhi();
    if (!rhi) {
        std::fprintf(stderr, "D3D11 QRhi unavailable; texture-wrap probe skipped\n");
        return false;
    }

    const auto* nativeHandles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
    if (!nativeHandles || !nativeHandles->dev) {
        std::fprintf(stderr, "D3D11 native device unavailable\n");
        return false;
    }

    auto* device = static_cast<ID3D11Device*>(nativeHandles->dev);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT created = device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(created)) {
        std::fprintf(stderr, "CreateTexture2D failed: 0x%08lx\n",
                     static_cast<unsigned long>(created));
        return false;
    }

    std::unique_ptr<QRhiTexture> imported(
        rhi->newTexture(QRhiTexture::RGBA8, QSize(kWidth, kHeight)));
    QRhiTexture::NativeTexture native{
        quint64(reinterpret_cast<uintptr_t>(texture.Get())),
        0,
    };
    const bool wrapped = imported->createFrom(native);
    std::printf("RHI<->D3D11 createFrom: %s\n", wrapped ? "OK" : "FAILED");
    return wrapped;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    const bool d3d11 = probeBackend(QRhi::D3D11, "D3D11");
    const bool d3d12 = probeBackend(QRhi::D3D12, "D3D12");
    const bool d3d11Wrap = d3d11 && wrapD3D11Texture();
    const char* chosenBackend = d3d11 ? "D3D11" : (d3d12 ? "D3D12" : "NONE");

    std::printf("chosen RHI D3D backend: %s; d3d11-wrap=%s\n", chosenBackend,
                d3d11 ? (d3d11Wrap ? "OK" : "FAILED") : "n/a");

    return (d3d11 || d3d12) && (!d3d11 || d3d11Wrap) ? 0 : 1;
}

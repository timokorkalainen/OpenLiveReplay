#include "nativevideodecoder.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QStringList>

#include <array>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

#ifndef MFVideoFormat_HEVC
const GUID kMfVideoFormatHevc = {
    0x43564548,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};
#define OLR_MFVideoFormat_HEVC kMfVideoFormatHevc
#else
#define OLR_MFVideoFormat_HEVC MFVideoFormat_HEVC
#endif

QString hrMessage(const QString& action, HRESULT hr) {
    return QStringLiteral("%1 (HRESULT 0x%2)")
        .arg(action)
        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

bool mftAvailable(REFGUID subtype) {
    MFT_REGISTER_TYPE_INFO input {};
    input.guidMajorType = MFMediaType_Video;
    input.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT
            | MFT_ENUM_FLAG_HARDWARE,
        &input,
        nullptr,
        &activates,
        &count);
    if (SUCCEEDED(hr) && activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) {
                activates[i]->Release();
            }
        }
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

bool d3d11Available(QStringList* detail) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const std::array<D3D_FEATURE_LEVEL, 4> levels {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels.data(),
        UINT(levels.size()),
        D3D11_SDK_VERSION,
        &device,
        &createdLevel,
        &context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels.data() + 1,
            UINT(levels.size() - 1),
            D3D11_SDK_VERSION,
            &device,
            &createdLevel,
            &context);
    }
    if (FAILED(hr)) {
        if (detail) {
            detail->append(hrMessage(QStringLiteral("D3D11 device creation failed"), hr));
        }
        return false;
    }
    return true;
}

} // namespace

class NativeVideoDecoder::Impl {
public:
    Impl(int, int) {}
};

NativeVideoDecoder::NativeVideoDecoder(int outputWidth, int outputHeight)
    : m_impl(new Impl(outputWidth, outputHeight)) {}

NativeVideoDecoder::~NativeVideoDecoder() {
    delete m_impl;
}

bool NativeVideoDecoder::decode(const CompressedAccessUnit&, FrameCallback, QString* error) {
    if (error) {
        *error = QStringLiteral("Windows native decode is not implemented yet");
    }
    return false;
}

void NativeVideoDecoder::reset() {}

NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities() {
    NativeVideoDecodeCapabilities caps;
    QStringList detail;

    bool comInitialized = false;
    const HRESULT comStartup = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comStartup)) {
        comInitialized = true;
    } else if (comStartup != RPC_E_CHANGED_MODE) {
        caps.detail = hrMessage(QStringLiteral("COM startup failed"), comStartup);
        return caps;
    }

    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startup)) {
        caps.detail = hrMessage(QStringLiteral("Media Foundation startup failed"), startup);
        if (comInitialized) {
            CoUninitialize();
        }
        return caps;
    }

    caps.d3d11 = d3d11Available(&detail);
    caps.h264 = mftAvailable(MFVideoFormat_H264);
    caps.hevc = mftAvailable(OLR_MFVideoFormat_HEVC);
#ifdef MFVideoFormat_HEVC_ES
    caps.hevc = caps.hevc || mftAvailable(MFVideoFormat_HEVC_ES);
#endif

    if (!caps.h264) {
        detail.append(QStringLiteral("H.264 decoder MFT unavailable"));
    }
    if (!caps.hevc) {
        detail.append(QStringLiteral("HEVC decoder MFT unavailable"));
    }
    if (detail.isEmpty()) {
        detail.append(QStringLiteral("Media Foundation native decode available"));
    }
    caps.detail = detail.join(QStringLiteral("; "));

    MFShutdown();
    if (comInitialized) {
        CoUninitialize();
    }
    return caps;
}

#endif

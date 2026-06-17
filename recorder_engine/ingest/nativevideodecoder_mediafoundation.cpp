#include "nativevideodecoder.h"
#include "nativeframecopy.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QByteArray>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstring>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
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

constexpr LONGLONG kMfTimePerSecond = 10000000;
constexpr LONGLONG kPtsClock = 90000;

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

bool isSupportedCodec(NativeVideoCodec codec) {
    return codec == NativeVideoCodec::H264 || codec == NativeVideoCodec::Hevc;
}

bool environmentFlagEnabled(const char* name) {
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return !value.isEmpty() && value != "0" && value != "false" && value != "no";
}

QString codecName(NativeVideoCodec codec) {
    if (codec == NativeVideoCodec::Hevc) {
        return QStringLiteral("HEVC");
    }
    return QStringLiteral("H.264");
}

QString decoderUnavailableMessage(NativeVideoCodec codec) {
    if (codec == NativeVideoCodec::Hevc) {
        return QStringLiteral(
            "Windows HEVC decoder is unavailable; install Windows HEVC media support or use FFmpeg fallback");
    }
    return QStringLiteral("Media Foundation H.264 decoder is unavailable");
}

DWORD decoderEnumFlags() {
    DWORD flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT;
    if (environmentFlagEnabled("OLR_MF_VIDEO_ENABLE_HARDWARE")) {
        flags |= MFT_ENUM_FLAG_HARDWARE;
    }
    return flags;
}

QString hrMessage(const QString& action, HRESULT hr) {
    return QStringLiteral("%1 (HRESULT 0x%2)")
        .arg(action)
        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

LONGLONG pts90kToMfTime(qint64 pts90k) {
    return pts90k * kMfTimePerSecond / kPtsClock;
}

qint64 mfTimeToPts90k(LONGLONG time) {
    return time * kPtsClock / kMfTimePerSecond;
}

void appendParameterSet(QByteArray* key, const QByteArray& nal) {
    if (!key) {
        return;
    }
    const quint32 size = quint32(nal.size());
    key->append(char((size >> 24) & 0xff));
    key->append(char((size >> 16) & 0xff));
    key->append(char((size >> 8) & 0xff));
    key->append(char(size & 0xff));
    key->append(nal);
}

QByteArray parameterSetKey(NativeVideoCodec codec, const H26xParameterSets& parameterSets) {
    QByteArray key;
    key.append(char(codec == NativeVideoCodec::H264 ? 1 : codec == NativeVideoCodec::Hevc ? 2 : 0));
    if (codec == NativeVideoCodec::H264) {
        for (const QByteArray& sps : parameterSets.h264Sps) appendParameterSet(&key, sps);
        key.append(char(0));
        for (const QByteArray& pps : parameterSets.h264Pps) appendParameterSet(&key, pps);
    } else if (codec == NativeVideoCodec::Hevc) {
        for (const QByteArray& vps : parameterSets.hevcVps) appendParameterSet(&key, vps);
        key.append(char(0));
        for (const QByteArray& sps : parameterSets.hevcSps) appendParameterSet(&key, sps);
        key.append(char(0));
        for (const QByteArray& pps : parameterSets.hevcPps) appendParameterSet(&key, pps);
    }
    return key.size() > 1 ? key : QByteArray();
}

bool mftAvailable(REFGUID subtype) {
    MFT_REGISTER_TYPE_INFO input {};
    input.guidMajorType = MFMediaType_Video;
    input.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        decoderEnumFlags(),
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

void releaseActivations(IMFActivate** activates, UINT32 count) {
    if (!activates) {
        return;
    }
    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i]) {
            activates[i]->Release();
        }
    }
    CoTaskMemFree(activates);
}

bool setVideoFrameSize(IMFMediaType* type, int width, int height, QString* error) {
    if (width <= 0 || height <= 0) {
        if (error) {
            *error = QStringLiteral("Media Foundation decode requires a configured output size");
        }
        return false;
    }
    const HRESULT hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, UINT32(width), UINT32(height));
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation video frame size setup failed"), hr);
        }
        return false;
    }
    return true;
}

bool shouldProvideOutputSample(const MFT_OUTPUT_STREAM_INFO& info) {
    return (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
}

void releaseOutputEvents(MFT_OUTPUT_DATA_BUFFER* output) {
    if (output && output->pEvents) {
        output->pEvents->Release();
        output->pEvents = nullptr;
    }
}

} // namespace

class NativeVideoDecoder::Impl {
public:
    Impl(int outputWidth, int outputHeight)
        : width(outputWidth)
        , height(outputHeight) {}

    ~Impl();

    bool decode(const CompressedAccessUnit& unit, FrameCallback onFrame, QString* error);
    void reset();

private:
    int width = 0;
    int height = 0;
    NativeVideoCodec codec = NativeVideoCodec::Unknown;
    QByteArray activeParameterSetKey;
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaEventGenerator> eventGenerator;
    ComPtr<IMFDXGIDeviceManager> deviceManager;
    ComPtr<ID3D11Device> d3dDevice;
    UINT resetToken = 0;
    GUID selectedInputSubtype = GUID_NULL;
    bool hasSelectedInputSubtype = false;
    bool asyncTransform = false;
    int asyncNeedInputEvents = 0;
    bool comInitialized = false;
    bool mfStarted = false;

    bool ensureRuntime(QString* error);
    void shutdownRuntime();
    bool ensureSession(const CompressedAccessUnit& unit, QString* error);
    bool createD3D(QString* error);
    bool createTransform(NativeVideoCodec nextCodec, QString* error);
    bool configureTypes(NativeVideoCodec nextCodec, QString* error);
    bool configureOutputType(QString* error);
    bool createInputSample(const CompressedAccessUnit& unit, ComPtr<IMFSample>* sample, QString* error);
    bool processInputSample(IMFSample* sample, QString* error);
    bool submitSample(const CompressedAccessUnit& unit, QString* error);
    bool drainSync(FrameCallback& onFrame, qint64 pts90k, QString* error);
    bool decodeAsync(const CompressedAccessUnit& unit, FrameCallback onFrame, QString* error);
    bool drainReadyAsyncEvents(FrameCallback& onFrame, qint64 pts90k, QString* error);
    bool handleAsyncEvent(IMFMediaEvent* event,
                          IMFSample* inputSample,
                          bool* submittedInput,
                          FrameCallback& onFrame,
                          qint64 pts90k,
                          QString* error);
    bool processOutputFrame(FrameCallback& onFrame,
                            qint64 pts90k,
                            bool allowNeedMoreInput,
                            bool* needMoreInput,
                            QString* error);
    bool copySampleToFrame(IMFSample* sample, qint64 fallbackPts90k, AVFrame** frame, QString* error);
};

NativeVideoDecoder::Impl::~Impl() {
    reset();
    shutdownRuntime();
}

bool NativeVideoDecoder::Impl::ensureRuntime(QString* error) {
    if (!comInitialized) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            comInitialized = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation COM startup failed"), hr);
            }
            return false;
        }
    }

    if (!mfStarted) {
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation startup failed"), hr);
            }
            return false;
        }
        mfStarted = true;
    }
    return true;
}

void NativeVideoDecoder::Impl::shutdownRuntime() {
    if (mfStarted) {
        MFShutdown();
        mfStarted = false;
    }
    if (comInitialized) {
        CoUninitialize();
        comInitialized = false;
    }
}

void NativeVideoDecoder::Impl::reset() {
    if (transform) {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    transform.Reset();
    eventGenerator.Reset();
    deviceManager.Reset();
    d3dDevice.Reset();
    resetToken = 0;
    selectedInputSubtype = GUID_NULL;
    hasSelectedInputSubtype = false;
    asyncTransform = false;
    asyncNeedInputEvents = 0;
    codec = NativeVideoCodec::Unknown;
    activeParameterSetKey.clear();
}

bool NativeVideoDecoder::Impl::createD3D(QString* error) {
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
        &d3dDevice,
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
            &d3dDevice,
            &createdLevel,
            &context);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation D3D11 device is unavailable"), hr);
        }
        return false;
    }

    hr = MFCreateDXGIDeviceManager(&resetToken, &deviceManager);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation DXGI device manager is unavailable"), hr);
        }
        return false;
    }

    hr = deviceManager->ResetDevice(d3dDevice.Get(), resetToken);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation DXGI device manager reset failed"), hr);
        }
        return false;
    }
    return true;
}

bool NativeVideoDecoder::Impl::createTransform(NativeVideoCodec nextCodec, QString* error) {
    if (!isSupportedCodec(nextCodec)) {
        if (error) {
            *error = QStringLiteral("Media Foundation native decode unsupported codec for this task");
        }
        return false;
    }

    std::array<GUID, 2> inputSubtypes {};
    int inputSubtypeCount = 0;
    if (nextCodec == NativeVideoCodec::H264) {
        inputSubtypes[inputSubtypeCount++] = MFVideoFormat_H264;
    } else {
        inputSubtypes[inputSubtypeCount++] = OLR_MFVideoFormat_HEVC;
#ifdef MFVideoFormat_HEVC_ES
        inputSubtypes[inputSubtypeCount++] = MFVideoFormat_HEVC_ES;
#endif
    }

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = S_OK;
    HRESULT firstFailure = S_OK;
    for (int i = 0; i < inputSubtypeCount; ++i) {
        MFT_REGISTER_TYPE_INFO input {};
        input.guidMajorType = MFMediaType_Video;
        input.guidSubtype = inputSubtypes[i];

        activates = nullptr;
        count = 0;
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            decoderEnumFlags(),
            &input,
            nullptr,
            &activates,
            &count);
        if (SUCCEEDED(hr) && count > 0 && activates) {
            selectedInputSubtype = inputSubtypes[i];
            hasSelectedInputSubtype = true;
            break;
        }
        if (FAILED(hr) && SUCCEEDED(firstFailure)) {
            firstFailure = hr;
        }
        releaseActivations(activates, count);
    }

    if (count == 0 || !activates) {
        if (error) {
            if (nextCodec == NativeVideoCodec::Hevc) {
                *error = decoderUnavailableMessage(nextCodec);
            } else if (FAILED(firstFailure)) {
                *error = hrMessage(QStringLiteral("Media Foundation H.264 decoder enumeration failed"), firstFailure);
            } else {
                *error = decoderUnavailableMessage(nextCodec);
            }
        }
        return false;
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform));
    releaseActivations(activates, count);
    if (FAILED(hr) || !transform) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation %1 decoder activation failed").arg(codecName(nextCodec)),
                hr);
        }
        return false;
    }

    ComPtr<IMFAttributes> attributes;
    UINT32 asyncValue = FALSE;
    if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asyncValue);
    }
    asyncTransform = asyncValue != FALSE;
    if (asyncTransform) {
        if (!attributes) {
            if (error) {
                *error = QStringLiteral("Media Foundation async decoder attributes are unavailable");
            }
            return false;
        }
        hr = transform.As(&eventGenerator);
        if (FAILED(hr) || !eventGenerator) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation async decoder event generator is unavailable"), hr);
            }
            return false;
        }
        hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation async decoder unlock failed"), hr);
            }
            return false;
        }
    }

    if (deviceManager) {
        hr = transform->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            ULONG_PTR(deviceManager.Get()));
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation decoder D3D manager setup failed"), hr);
            }
            return false;
        }
    }
    return true;
}

bool NativeVideoDecoder::Impl::configureOutputType(QString* error) {
    ComPtr<IMFMediaType> outputType;
    HRESULT hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation output type creation failed"), hr);
        }
        return false;
    }

    hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) {
        hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation output type setup failed"), hr);
        }
        return false;
    }
    if (!setVideoFrameSize(outputType.Get(), width, height, error)) {
        return false;
    }

    hr = transform->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation NV12 output type setup failed"), hr);
        }
        return false;
    }
    return true;
}

bool NativeVideoDecoder::Impl::configureTypes(NativeVideoCodec nextCodec, QString* error) {
    ComPtr<IMFMediaType> inputType;
    HRESULT hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input type creation failed"), hr);
        }
        return false;
    }

    const GUID inputSubtype = hasSelectedInputSubtype
        ? selectedInputSubtype
        : (nextCodec == NativeVideoCodec::H264 ? MFVideoFormat_H264 : OLR_MFVideoFormat_HEVC);
    hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) {
        hr = inputType->SetGUID(MF_MT_SUBTYPE, inputSubtype);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input type setup failed"), hr);
        }
        return false;
    }
    if (!setVideoFrameSize(inputType.Get(), width, height, error)) {
        return false;
    }

    hr = transform->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation %1 input type setup failed").arg(codecName(nextCodec)),
                hr);
        }
        return false;
    }

    if (!configureOutputType(error)) {
        return false;
    }

    hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) {
        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation decoder stream start failed"), hr);
        }
        return false;
    }
    return true;
}

bool NativeVideoDecoder::Impl::ensureSession(const CompressedAccessUnit& unit, QString* error) {
    if (!isSupportedCodec(unit.codec)) {
        if (error) {
            *error = QStringLiteral("Media Foundation native decode unsupported codec for this task");
        }
        return false;
    }

    const QByteArray nextKey = parameterSetKey(unit.codec, unit.parameterSets);
    if (transform && unit.codec == codec && (nextKey.isEmpty() || nextKey == activeParameterSetKey)) {
        return true;
    }

    reset();
    if (!ensureRuntime(error)
        || (environmentFlagEnabled("OLR_MF_VIDEO_ENABLE_D3D") && !createD3D(error))
        || !createTransform(unit.codec, error)
        || !configureTypes(unit.codec, error)) {
        reset();
        return false;
    }

    codec = unit.codec;
    activeParameterSetKey = nextKey;
    return true;
}

bool NativeVideoDecoder::Impl::createInputSample(const CompressedAccessUnit& unit,
                                                  ComPtr<IMFSample>* sample,
                                                  QString* error) {
    if (!sample) {
        if (error) {
            *error = QStringLiteral("Media Foundation input sample setup failed");
        }
        return false;
    }
    if (unit.annexB.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Media Foundation native decoder received an empty access unit");
        }
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(DWORD(unit.annexB.size()), &buffer);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input buffer creation failed"), hr);
        }
        return false;
    }

    BYTE* data = nullptr;
    hr = buffer->Lock(&data, nullptr, nullptr);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input buffer lock failed"), hr);
        }
        return false;
    }
    memcpy(data, unit.annexB.constData(), size_t(unit.annexB.size()));
    buffer->Unlock();
    hr = buffer->SetCurrentLength(DWORD(unit.annexB.size()));
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input buffer length setup failed"), hr);
        }
        return false;
    }

    ComPtr<IMFSample> createdSample;
    hr = MFCreateSample(&createdSample);
    if (SUCCEEDED(hr)) {
        hr = createdSample->AddBuffer(buffer.Get());
    }
    if (SUCCEEDED(hr) && unit.pts90k >= 0) {
        hr = createdSample->SetSampleTime(pts90kToMfTime(unit.pts90k));
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input sample setup failed"), hr);
        }
        return false;
    }
    *sample = createdSample;
    return true;
}

bool NativeVideoDecoder::Impl::processInputSample(IMFSample* sample, QString* error) {
    const HRESULT hr = transform->ProcessInput(0, sample, 0);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation %1 frame decode input failed").arg(codecName(codec)),
                hr);
        }
        return false;
    }
    return true;
}

bool NativeVideoDecoder::Impl::submitSample(const CompressedAccessUnit& unit, QString* error) {
    ComPtr<IMFSample> sample;
    return createInputSample(unit, &sample, error) && processInputSample(sample.Get(), error);
}

bool NativeVideoDecoder::Impl::copySampleToFrame(IMFSample* sample,
                                                  qint64 fallbackPts90k,
                                                  AVFrame** frame,
                                                  QString* error) {
    if (!sample || !frame) {
        if (error) {
            *error = QStringLiteral("Media Foundation decoded a frame but output buffer copy failed");
        }
        return false;
    }

    LONGLONG sampleTime = 0;
    const qint64 framePts = SUCCEEDED(sample->GetSampleTime(&sampleTime))
        ? mfTimeToPts90k(sampleTime)
        : fallbackPts90k;

    ComPtr<IMFMediaBuffer> firstBuffer;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &firstBuffer))) {
        ComPtr<IMF2DBuffer2> buffer2d;
        if (SUCCEEDED(firstBuffer.As(&buffer2d))) {
            BYTE* scanline0 = nullptr;
            LONG pitch = 0;
            BYTE* bufferStart = nullptr;
            DWORD bufferLength = 0;
            const HRESULT hr = buffer2d->Lock2DSize(
                MF2DBuffer_LockFlags_Read,
                &scanline0,
                &pitch,
                &bufferStart,
                &bufferLength);
            if (SUCCEEDED(hr)) {
                if (pitch > 0) {
                    const DWORD requiredLength = DWORD(pitch * height + pitch * (height / 2));
                    if (scanline0 && bufferStart
                        && scanline0 >= bufferStart
                        && bufferLength >= requiredLength
                        && scanline0 + requiredLength <= bufferStart + bufferLength) {
                        *frame = nativeCopyNv12ToYuv420p(
                            scanline0,
                            pitch,
                            scanline0 + pitch * height,
                            pitch,
                            width,
                            height);
                    }
                }
                buffer2d->Unlock2D();
                if (*frame) {
                    (*frame)->pts = framePts;
                    return true;
                }
            }
        }
    }

    ComPtr<IMFMediaBuffer> contiguous;
    HRESULT hr = sample->ConvertToContiguousBuffer(&contiguous);
    if (SUCCEEDED(hr)) {
        BYTE* data = nullptr;
        DWORD currentLength = 0;
        hr = contiguous->Lock(&data, nullptr, &currentLength);
        if (SUCCEEDED(hr)) {
            if (currentLength >= DWORD(width * height + width * (height / 2))) {
                *frame = nativeCopyNv12ToYuv420p(
                    data,
                    width,
                    data + width * height,
                    width,
                    width,
                    height);
            }
            contiguous->Unlock();
            if (*frame) {
                (*frame)->pts = framePts;
                return true;
            }
        }
    }

    if (error) {
        *error = QStringLiteral("Media Foundation decoded a frame but output buffer copy failed");
    }
    return false;
}

bool NativeVideoDecoder::Impl::processOutputFrame(FrameCallback& onFrame,
                                                  qint64 pts90k,
                                                  bool allowNeedMoreInput,
                                                  bool* needMoreInput,
                                                  QString* error) {
    if (needMoreInput) {
        *needMoreInput = false;
    }
    while (true) {
        MFT_OUTPUT_STREAM_INFO streamInfo {};
        HRESULT hr = transform->GetOutputStreamInfo(0, &streamInfo);
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation output stream info query failed"), hr);
            }
            return false;
        }

        ComPtr<IMFSample> providedSample;
        if (shouldProvideOutputSample(streamInfo)) {
            ComPtr<IMFMediaBuffer> outputBuffer;
            const DWORD outputSize = std::max<DWORD>(
                streamInfo.cbSize,
                DWORD(width * height + width * (height / 2)));
            hr = MFCreateSample(&providedSample);
            if (SUCCEEDED(hr)) {
                hr = MFCreateMemoryBuffer(outputSize, &outputBuffer);
            }
            if (SUCCEEDED(hr)) {
                hr = providedSample->AddBuffer(outputBuffer.Get());
            }
            if (FAILED(hr)) {
                if (error) {
                    *error = hrMessage(QStringLiteral("Media Foundation output sample allocation failed"), hr);
                }
                return false;
            }
        }

        MFT_OUTPUT_DATA_BUFFER output {};
        output.dwStreamID = 0;
        output.pSample = providedSample.Get();
        DWORD status = 0;
        hr = transform->ProcessOutput(0, 1, &output, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            releaseOutputEvents(&output);
            if (allowNeedMoreInput) {
                if (needMoreInput) {
                    *needMoreInput = true;
                }
                return true;
            }
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation async decoder unexpectedly requested input from output"), hr);
            }
            return false;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            releaseOutputEvents(&output);
            if (output.pSample && !providedSample) {
                output.pSample->Release();
                output.pSample = nullptr;
            }
            if (!configureOutputType(error)) {
                return false;
            }
            if (!allowNeedMoreInput) {
                return true;
            }
            continue;
        }
        if (FAILED(hr)) {
            releaseOutputEvents(&output);
            if (error) {
                *error = hrMessage(
                    QStringLiteral("Media Foundation %1 frame decode output failed").arg(codecName(codec)),
                    hr);
            }
            return false;
        }

        ComPtr<IMFSample> completedSample;
        if (providedSample) {
            completedSample = providedSample;
        } else {
            completedSample.Attach(output.pSample);
            output.pSample = nullptr;
        }
        releaseOutputEvents(&output);

        AVFrame* frame = nullptr;
        if (!copySampleToFrame(completedSample.Get(), pts90k, &frame, error)) {
            return false;
        }
        onFrame(frame);
        return true;
    }
}

bool NativeVideoDecoder::Impl::drainSync(FrameCallback& onFrame, qint64 pts90k, QString* error) {
    while (true) {
        bool needMoreInput = false;
        if (!processOutputFrame(onFrame, pts90k, true, &needMoreInput, error)) {
            return false;
        }
        if (needMoreInput) {
            return true;
        }
    }
}

bool NativeVideoDecoder::Impl::handleAsyncEvent(IMFMediaEvent* event,
                                                 IMFSample* inputSample,
                                                 bool* submittedInput,
                                                 FrameCallback& onFrame,
                                                 qint64 pts90k,
                                                 QString* error) {
    MediaEventType eventType = MediaEventType(0);
    HRESULT hr = event->GetType(&eventType);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation async decoder event type query failed"), hr);
        }
        return false;
    }

    HRESULT eventStatus = S_OK;
    hr = event->GetStatus(&eventStatus);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation async decoder event status query failed"), hr);
        }
        return false;
    }
    if (FAILED(eventStatus)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation async decoder event failed"), eventStatus);
        }
        return false;
    }

    if (eventType == METransformNeedInput) {
        if (inputSample && submittedInput && !*submittedInput) {
            if (!processInputSample(inputSample, error)) {
                return false;
            }
            *submittedInput = true;
        } else {
            ++asyncNeedInputEvents;
        }
        return true;
    }

    if (eventType == METransformHaveOutput) {
        bool needMoreInput = false;
        return processOutputFrame(onFrame, pts90k, false, &needMoreInput, error);
    }

    if (eventType == METransformDrainComplete || eventType == METransformMarker) {
        return true;
    }

    return true;
}

bool NativeVideoDecoder::Impl::drainReadyAsyncEvents(FrameCallback& onFrame,
                                                      qint64 pts90k,
                                                      QString* error) {
    while (true) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = eventGenerator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            return true;
        }
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation async decoder event query failed"), hr);
            }
            return false;
        }

        bool submittedInput = true;
        if (!handleAsyncEvent(event.Get(), nullptr, &submittedInput, onFrame, pts90k, error)) {
            return false;
        }
    }
}

bool NativeVideoDecoder::Impl::decodeAsync(const CompressedAccessUnit& unit,
                                           FrameCallback onFrame,
                                           QString* error) {
    ComPtr<IMFSample> sample;
    if (!createInputSample(unit, &sample, error)) {
        return false;
    }

    bool submittedInput = false;
    if (asyncNeedInputEvents > 0) {
        --asyncNeedInputEvents;
        if (!processInputSample(sample.Get(), error)) {
            return false;
        }
        submittedInput = true;
    }

    while (!submittedInput) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = eventGenerator->GetEvent(0, &event);
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation async decoder event query failed"), hr);
            }
            return false;
        }
        if (!handleAsyncEvent(event.Get(), sample.Get(), &submittedInput, onFrame, unit.pts90k, error)) {
            return false;
        }
    }

    return drainReadyAsyncEvents(onFrame, unit.pts90k, error);
}

bool NativeVideoDecoder::Impl::decode(const CompressedAccessUnit& unit,
                                       FrameCallback onFrame,
                                       QString* error) {
    if (!onFrame) {
        if (error) {
            *error = QStringLiteral("Media Foundation decode requires a frame callback");
        }
        return false;
    }
    if (!ensureSession(unit, error)) {
        return false;
    }
    if (asyncTransform) {
        return decodeAsync(unit, std::move(onFrame), error);
    }
    if (!submitSample(unit, error)) {
        return false;
    }
    return drainSync(onFrame, unit.pts90k, error);
}

NativeVideoDecoder::NativeVideoDecoder(int outputWidth, int outputHeight)
    : m_impl(new Impl(outputWidth, outputHeight)) {}

NativeVideoDecoder::~NativeVideoDecoder() {
    delete m_impl;
}

bool NativeVideoDecoder::decode(const CompressedAccessUnit& unit,
                                 FrameCallback onFrame,
                                 QString* error) {
    return m_impl->decode(unit, std::move(onFrame), error);
}

void NativeVideoDecoder::reset() {
    m_impl->reset();
}

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

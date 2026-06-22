#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/codec/avcc.h"
#include "recorder_engine/mediafoundationruntime.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <algorithm>
#include <cstring>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <objbase.h>
// codecapi.h declares the CODECAPI_* property GUIDs with DEFINE_GUID; including
// <initguid.h> first instantiates their definitions in this translation unit so
// we do not depend on a specific import library (MinGW's strmiids/codecapi
// coverage is incomplete). Mirrors nativeaacdecoder_mediafoundation.cpp.
#include <initguid.h>
#include <codecapi.h>
#include <wrl/client.h>

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8,
                0xda)
#endif

extern "C" {
#include <libavutil/frame.h>
}

using Microsoft::WRL::ComPtr;

namespace {

// All-intra, every output sample is a key frame. The MF H.264 encoder emits
// timestamps in 100 ns units; the encoder echoes the caller-supplied opaque
// ptsTicks back to the packet callback (it does not interpret them). We stamp
// each input sample with a monotonic MF sample time (purely to keep the encoder
// happy) and use that time as a key to recover the caller's opaque ptsTicks when
// the matching output sample emerges, so the callback always sees the caller's
// value rather than the internal MF clock.
constexpr LONGLONG kMfTimePerSecond = 10000000;

QString hrMessage(const QString& action, HRESULT hr) {
    return QStringLiteral("%1 (HRESULT 0x%2)")
        .arg(action)
        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

// Split an Annex B elementary stream into NAL payloads (start codes removed).
QList<QByteArray> splitAnnexB(const uint8_t* data, int size) {
    QList<QByteArray> nals;
    if (!data || size <= 0) {
        return nals;
    }
    auto startCode = [&](int p) -> int {
        if (p + 3 <= size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) {
            return 3;
        }
        if (p + 4 <= size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 &&
            data[p + 3] == 1) {
            return 4;
        }
        return 0;
    };
    QList<int> starts;
    for (int p = 0; p + 3 <= size;) {
        const int s = startCode(p);
        if (s) {
            starts.append(p);
            p += s;
        } else {
            ++p;
        }
    }
    for (int k = 0; k < starts.size(); ++k) {
        const int off = starts[k] + startCode(starts[k]);
        const int end = (k + 1 < starts.size()) ? starts[k + 1] : size;
        if (end > off) {
            nals.append(QByteArray(reinterpret_cast<const char*>(data + off), end - off));
        }
    }
    return nals;
}

// Convert a list of raw NAL payloads into a 4-byte-big-endian length-prefixed
// (AVCC) buffer, the form Matroska/the muxer expects for hardware-encoded H.264.
QByteArray annexBToLengthPrefixed(const QList<QByteArray>& nals) {
    QByteArray out;
    for (const QByteArray& nal : nals) {
        const quint32 n = quint32(nal.size());
        out.append(char((n >> 24) & 0xff));
        out.append(char((n >> 16) & 0xff));
        out.append(char((n >> 8) & 0xff));
        out.append(char(n & 0xff));
        out.append(nal);
    }
    return out;
}

QByteArray buildAvccFromNals(const QList<QByteArray>& nals) {
    QList<QByteArray> sps;
    QList<QByteArray> pps;
    for (const QByteArray& nal : nals) {
        if (nal.isEmpty()) {
            continue;
        }
        const int nalType = quint8(nal.at(0)) & 0x1f;
        if (nalType == 7) {
            sps.append(nal);
        } else if (nalType == 8) {
            pps.append(nal);
        }
    }
    return buildAvcCFromParameterSets(sps, pps);
}

bool containsH264Idr(const QList<QByteArray>& nals) {
    for (const QByteArray& nal : nals) {
        if (nal.isEmpty()) {
            continue;
        }
        if ((quint8(nal.at(0)) & 0x1f) == 5) {
            return true;
        }
    }
    return false;
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

} // namespace

class MediaFoundationEncoder : public NativeVideoEncoder {
public:
    MediaFoundationEncoder() = default;
    ~MediaFoundationEncoder() override;

    bool initialize(const Config& config, QString* error);

    bool encode(const AVFrame* frame, int64_t ptsTicks, const PacketCallback& onPacket,
                QString* error) override;
    bool flush(const PacketCallback& onPacket, QString* error) override;
    QByteArray avccExtradata() const override;

private:
    bool ensureRuntime(QString* error);
    void shutdownRuntime();
    bool createTransform(QString* error);
    bool configureOutputType(QString* error);
    bool configureInputType(QString* error);
    bool configureCodecApi(QString* error);
    bool beginStreaming(QString* error);

    bool buildInputSample(const AVFrame* frame, int64_t ptsTicks, ComPtr<IMFSample>* sample,
                          QString* error);
    bool drainOutput(const PacketCallback& onPacket, QString* error);
    bool buildAvccFromSequenceHeader(QString* error);
    bool emitOutputSample(IMFSample* sample, const PacketCallback& onPacket, QString* error);

    // Async event-pump helpers (used only when the chosen MFT is async).
    bool processInputSample(IMFSample* sample, QString* error);
    bool processOutputAsync(const PacketCallback& onPacket, QString* error);
    bool handleAsyncEvent(IMFMediaEvent* event, IMFSample* inputSample, bool* submittedInput,
                          const PacketCallback& onPacket, QString* error);
    bool encodeAsync(IMFSample* inputSample, const PacketCallback& onPacket, QString* error);
    bool drainReadyAsyncEvents(const PacketCallback& onPacket, QString* error);
    bool flushAsync(const PacketCallback& onPacket, QString* error);

    // Recover the caller's opaque ptsTicks for an output sample, keyed by the MF
    // sample time we stamped onto the matching input. Falls back to the MF time
    // converted to ticks when no mapping exists (e.g. encoder reordered output).
    int64_t resolvePtsTicks(LONGLONG sampleTime);

    Config m_config;
    ComPtr<IMFTransform> m_transform;
    ComPtr<IMFMediaEventGenerator> m_eventGenerator;
    QByteArray m_avcc;
    bool m_comInitialized = false;
    bool m_streaming = false;
    bool m_asyncTransform = false;
    int m_asyncNeedInputEvents = 0;
    LONGLONG m_sampleDuration = 0;
    LONGLONG m_nextSampleTime = 0;
    // Maps the MF input sample time (our monotonic stamp) to the caller's opaque
    // ptsTicks so the packet callback echoes the caller's value, per the contract.
    QHash<LONGLONG, int64_t> m_ptsByMfTime;
};

MediaFoundationEncoder::~MediaFoundationEncoder() {
    if (m_transform && m_streaming) {
        // On an async MFT, FLUSH discards any queued events/samples so the drain
        // messages below tear down cleanly without us pumping the event queue.
        if (m_asyncTransform) {
            m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    m_eventGenerator.Reset();
    m_transform.Reset();
    shutdownRuntime();
}

bool MediaFoundationEncoder::ensureRuntime(QString* error) {
    if (!m_comInitialized) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            m_comInitialized = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation COM startup failed"), hr);
            }
            return false;
        }
    }
    if (!ensureMediaFoundationRuntime(error)) {
        return false;
    }
    return true;
}

void MediaFoundationEncoder::shutdownRuntime() {
    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

bool MediaFoundationEncoder::createTransform(QString* error) {
    // HARDWARE ONLY: enumerate H.264 encoder MFTs that take NV12 input and emit
    // H.264, restricted to MFT_ENUM_FLAG_HARDWARE. If none exist we return false
    // and create() yields nullptr — we never fall back to a software MFT.
    MFT_REGISTER_TYPE_INFO outputInfo{};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = MFVideoFormat_H264;

    MFT_REGISTER_TYPE_INFO inputInfo{};
    inputInfo.guidMajorType = MFMediaType_Video;
    inputInfo.guidSubtype = MFVideoFormat_NV12;

    const DWORD enumFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, enumFlags, &inputInfo, &outputInfo,
                           &activates, &count);
    if (FAILED(hr) || count == 0 || !activates) {
        releaseActivations(activates, count);
        if (error) {
            *error =
                (FAILED(hr))
                    ? hrMessage(QStringLiteral(
                                    "Media Foundation H.264 hardware encoder enumeration failed"),
                                hr)
                    : QStringLiteral("No Media Foundation hardware H.264 encoder is available");
        }
        return false;
    }

    // Activate the first hardware encoder MFT.
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_transform));
    releaseActivations(activates, count);
    if (FAILED(hr) || !m_transform) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation H.264 hardware encoder activation failed"), hr);
        }
        return false;
    }

    // Async hardware MFTs (the common case for HW encoders) must be unlocked
    // before use and driven via the event-generator pump. Sync MFTs ignore the
    // unlock and are driven with the synchronous ProcessInput/ProcessOutput loop.
    ComPtr<IMFAttributes> attributes;
    UINT32 isAsync = FALSE;
    if (SUCCEEDED(m_transform->GetAttributes(&attributes)) && attributes) {
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
    }
    m_asyncTransform = isAsync != FALSE;
    if (m_asyncTransform) {
        if (!attributes) {
            if (error) {
                *error =
                    QStringLiteral("Media Foundation async encoder attributes are unavailable");
            }
            return false;
        }
        hr = m_transform.As(&m_eventGenerator);
        if (FAILED(hr) || !m_eventGenerator) {
            if (error) {
                *error = hrMessage(
                    QStringLiteral("Media Foundation async encoder event generator is unavailable"),
                    hr);
            }
            return false;
        }
        hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(hr)) {
            if (error) {
                *error =
                    hrMessage(QStringLiteral("Media Foundation async encoder unlock failed"), hr);
            }
            return false;
        }
    }
    return true;
}

bool MediaFoundationEncoder::configureOutputType(QString* error) {
    // The H.264 encoder requires the OUTPUT type to be set before the input type.
    ComPtr<IMFMediaType> outputType;
    HRESULT hr = MFCreateMediaType(&outputType);
    if (SUCCEEDED(hr)) {
        hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, UINT32(m_config.bitrate));
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, UINT32(m_config.width),
                                UINT32(m_config.height));
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, UINT32(m_config.fpsNum),
                                 UINT32(m_config.fpsDen));
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    }
    if (FAILED(hr)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation H.264 output type setup failed"), hr);
        }
        return false;
    }

    hr = m_transform->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation H.264 SetOutputType failed"), hr);
        }
        return false;
    }
    return true;
}

bool MediaFoundationEncoder::configureInputType(QString* error) {
    ComPtr<IMFMediaType> inputType;
    HRESULT hr = MFCreateMediaType(&inputType);
    if (SUCCEEDED(hr)) {
        hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, UINT32(m_config.width),
                                UINT32(m_config.height));
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, UINT32(m_config.fpsNum),
                                 UINT32(m_config.fpsDen));
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation NV12 input type setup failed"), hr);
        }
        return false;
    }

    hr = m_transform->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation NV12 SetInputType failed"), hr);
        }
        return false;
    }
    return true;
}

bool MediaFoundationEncoder::configureCodecApi(QString* error) {
    // All-intra + low-latency configuration via ICodecAPI. These properties are
    // best-effort on some encoders; we treat a failed SetValue on an optional
    // property as non-fatal, but require GOPSize=1 (the single most important
    // all-intra knob) to succeed.
    ComPtr<ICodecAPI> codecApi;
    HRESULT hr = m_transform.As(&codecApi);
    if (FAILED(hr) || !codecApi) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation H.264 encoder does not expose ICodecAPI"), hr);
        }
        return false;
    }

    auto setU32 = [&](const GUID& api, UINT32 value) -> HRESULT {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = value;
        const HRESULT setHr = codecApi->SetValue(&api, &var);
        VariantClear(&var);
        return setHr;
    };

    auto setBool = [&](const GUID& api, bool value) -> HRESULT {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_BOOL;
        var.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        const HRESULT setHr = codecApi->SetValue(&api, &var);
        VariantClear(&var);
        return setHr;
    };

    // Rate control mode must be set before bitrate-dependent knobs on most MFTs.
    if (codecApi->IsModifiable(&CODECAPI_AVEncCommonRateControlMode) == S_OK ||
        codecApi->IsSupported(&CODECAPI_AVEncCommonRateControlMode) == S_OK) {
        setU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    }
    if (codecApi->IsModifiable(&CODECAPI_AVEncCommonMeanBitRate) == S_OK ||
        codecApi->IsSupported(&CODECAPI_AVEncCommonMeanBitRate) == S_OK) {
        setU32(CODECAPI_AVEncCommonMeanBitRate, UINT32(m_config.bitrate));
    }

    // No B-frames: required for all-intra and for in-order keyframe-only output.
    if (codecApi->IsModifiable(&CODECAPI_AVEncMPVDefaultBPictureCount) == S_OK ||
        codecApi->IsSupported(&CODECAPI_AVEncMPVDefaultBPictureCount) == S_OK) {
        setU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    }

    // Low-latency: disable lookahead/reordering where supported.
    if (codecApi->IsModifiable(&CODECAPI_AVLowLatencyMode) == S_OK ||
        codecApi->IsSupported(&CODECAPI_AVLowLatencyMode) == S_OK) {
        setBool(CODECAPI_AVLowLatencyMode, true);
    }

    // GOP size of 1 → every frame is an IDR. This is the load-bearing all-intra
    // setting; if the encoder rejects it, we cannot honor the all-intra contract.
    hr = setU32(CODECAPI_AVEncMPVGOPSize, 1);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation H.264 encoder rejected GOPSize=1 (all-intra)"),
                hr);
        }
        return false;
    }
    return true;
}

bool MediaFoundationEncoder::beginStreaming(QString* error) {
    HRESULT hr = m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (SUCCEEDED(hr)) {
        hr = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation H.264 encoder stream start failed"), hr);
        }
        return false;
    }
    m_streaming = true;
    return true;
}

bool MediaFoundationEncoder::initialize(const Config& config, QString* error) {
    m_config = config;
    if (m_config.width <= 0 || m_config.height <= 0) {
        if (error) {
            *error =
                QStringLiteral("Media Foundation H.264 encoder requires a positive frame size");
        }
        return false;
    }
    if (m_config.fpsNum <= 0) {
        m_config.fpsNum = 30;
    }
    if (m_config.fpsDen <= 0) {
        m_config.fpsDen = 1;
    }
    // Per-frame duration in 100 ns units, used only for MF sample bookkeeping.
    m_sampleDuration = kMfTimePerSecond * LONGLONG(m_config.fpsDen) / LONGLONG(m_config.fpsNum);

    if (!ensureRuntime(error)) {
        return false;
    }
    if (!createTransform(error)) {
        return false;
    }
    // Output type must precede input type for the H.264 encoder MFT.
    if (!configureOutputType(error)) {
        return false;
    }
    if (!configureInputType(error)) {
        return false;
    }
    if (!configureCodecApi(error)) {
        return false;
    }
    if (!beginStreaming(error)) {
        return false;
    }
    return true;
}

bool MediaFoundationEncoder::buildInputSample(const AVFrame* frame, int64_t ptsTicks,
                                              ComPtr<IMFSample>* sample, QString* error) {
    if (!frame || !frame->data[0] || !frame->data[1] || !frame->data[2]) {
        if (error) {
            *error = QStringLiteral("Media Foundation H.264 encoder received an invalid YUV frame");
        }
        return false;
    }
    const int width = frame->width;
    const int height = frame->height;
    if (width != m_config.width || height != m_config.height) {
        if (error) {
            *error = QStringLiteral("Media Foundation H.264 encoder frame size %1x%2 does not "
                                    "match configured %3x%4")
                         .arg(width)
                         .arg(height)
                         .arg(m_config.width)
                         .arg(m_config.height);
        }
        return false;
    }

    // NV12 = full-res Y plane followed by an interleaved (Cb,Cr) plane at half
    // resolution in both dimensions. We pack tightly (stride == width).
    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    const DWORD ySize = DWORD(width) * DWORD(height);
    const DWORD uvSize = DWORD(width) * DWORD(chromaHeight); // chromaWidth*2 == width
    const DWORD totalSize = ySize + uvSize;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(totalSize, &buffer);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input buffer creation failed"), hr);
        }
        return false;
    }

    BYTE* dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input buffer lock failed"), hr);
        }
        return false;
    }

    // Y plane: copy row-by-row to drop any source line padding.
    const uint8_t* srcY = frame->data[0];
    const int srcYStride = frame->linesize[0];
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst + size_t(y) * width, srcY + size_t(y) * srcYStride, size_t(width));
    }

    // Interleave U and V into the NV12 chroma plane (U then V per pixel pair).
    BYTE* dstUV = dst + ySize;
    const uint8_t* srcU = frame->data[1];
    const uint8_t* srcV = frame->data[2];
    const int srcUStride = frame->linesize[1];
    const int srcVStride = frame->linesize[2];
    for (int y = 0; y < chromaHeight; ++y) {
        BYTE* row = dstUV + size_t(y) * width;
        const uint8_t* uRow = srcU + size_t(y) * srcUStride;
        const uint8_t* vRow = srcV + size_t(y) * srcVStride;
        for (int x = 0; x < chromaWidth; ++x) {
            row[2 * x] = uRow[x];
            row[2 * x + 1] = vRow[x];
        }
    }

    buffer->Unlock();
    hr = buffer->SetCurrentLength(totalSize);
    if (FAILED(hr)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation input buffer length setup failed"), hr);
        }
        return false;
    }

    const LONGLONG stampedTime = m_nextSampleTime;
    ComPtr<IMFSample> createdSample;
    hr = MFCreateSample(&createdSample);
    if (SUCCEEDED(hr)) {
        hr = createdSample->AddBuffer(buffer.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = createdSample->SetSampleTime(stampedTime);
    }
    if (SUCCEEDED(hr)) {
        hr = createdSample->SetSampleDuration(m_sampleDuration);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation input sample setup failed"), hr);
        }
        return false;
    }
    m_nextSampleTime += m_sampleDuration;
    // Remember the caller's opaque ptsTicks keyed by the stamp so the output can
    // echo it back per the interface contract.
    m_ptsByMfTime.insert(stampedTime, ptsTicks);
    // Bound the map: for all-intra monotonic-PTS streams the encoder emits output
    // in input order, so any entry whose stamp is strictly less than the stamp we
    // just inserted can never match a future output sample. Cap at 64 in-flight
    // entries; evict the stalest (smallest-key) entries until within the cap.
    // This prevents unbounded growth when the encoder occasionally produces an
    // output whose sample time doesn't match any stamped input (latent leak over
    // a long recording).
    constexpr int kMaxInFlight = 64;
    while (m_ptsByMfTime.size() > kMaxInFlight) {
        // QHash has no ordered iteration, but stampedTime is strictly increasing
        // (m_nextSampleTime advances monotonically). Find and evict the entry
        // with the smallest key (furthest behind the current stamp).
        auto minIt = m_ptsByMfTime.begin();
        for (auto it = m_ptsByMfTime.begin(); it != m_ptsByMfTime.end(); ++it) {
            if (it.key() < minIt.key()) minIt = it;
        }
        m_ptsByMfTime.erase(minIt);
    }
    *sample = createdSample;
    return true;
}

int64_t MediaFoundationEncoder::resolvePtsTicks(LONGLONG sampleTime) {
    const auto it = m_ptsByMfTime.constFind(sampleTime);
    if (it != m_ptsByMfTime.constEnd()) {
        const int64_t ticks = it.value();
        m_ptsByMfTime.erase(it);
        // Evict any entries with a stamp strictly less than sampleTime: for an
        // all-intra, monotonic-PTS stream they can never match a future output.
        QList<LONGLONG> stale;
        for (auto jt = m_ptsByMfTime.constBegin(); jt != m_ptsByMfTime.constEnd(); ++jt) {
            if (jt.key() < sampleTime) stale.append(jt.key());
        }
        for (LONGLONG k : stale) m_ptsByMfTime.remove(k);
        return ticks;
    }
    // No mapping (encoder produced an unexpected timestamp): fall back to the MF
    // sample time so we still emit a monotonic, usable value.
    return int64_t(sampleTime);
}

bool MediaFoundationEncoder::buildAvccFromSequenceHeader(QString* error) {
    // After the first ProcessOutput succeeds, the encoder's negotiated output
    // type carries the SPS/PPS as an Annex B blob in MF_MT_MPEG_SEQUENCE_HEADER.
    ComPtr<IMFMediaType> outputType;
    HRESULT hr = m_transform->GetOutputCurrentType(0, &outputType);
    if (FAILED(hr) || !outputType) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation H.264 encoder current output type unavailable"),
                hr);
        }
        return false;
    }

    UINT32 blobSize = 0;
    hr = outputType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blobSize);
    if (FAILED(hr) || blobSize == 0) {
        // Not fatal: some encoders expose the sequence header only inside the
        // first bitstream NALs. We leave m_avcc empty and try again later.
        return true;
    }

    QByteArray blob(int(blobSize), Qt::Uninitialized);
    hr = outputType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, reinterpret_cast<UINT8*>(blob.data()),
                             blobSize, nullptr);
    if (FAILED(hr)) {
        return true; // best effort; keep encoding
    }

    const QList<QByteArray> nals =
        splitAnnexB(reinterpret_cast<const uint8_t*>(blob.constData()), blob.size());
    m_avcc = buildAvccFromNals(nals);
    return true;
}

bool MediaFoundationEncoder::emitOutputSample(IMFSample* sample, const PacketCallback& onPacket,
                                              QString* error) {
    // Lazily build the avcC from the (now negotiated) output type.
    if (m_avcc.isEmpty()) {
        QString avccError;
        buildAvccFromSequenceHeader(&avccError); // best-effort; ignore failure
    }

    // Determine keyframe via MFSampleExtension_CleanPoint (default: keyframe).
    UINT32 cleanPoint = 1;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);

    // Recover the caller's opaque ptsTicks via the MF sample time we stamped on
    // the matching input, so the callback echoes the caller's value (contract).
    LONGLONG sampleTime = 0;
    sample->GetSampleTime(&sampleTime);
    const int64_t ptsTicks = resolvePtsTicks(sampleTime);

    ComPtr<IMFMediaBuffer> outBuffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(&outBuffer);
    if (FAILED(hr) || !outBuffer) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation H.264 output buffer access failed"), hr);
        }
        return false;
    }

    BYTE* data = nullptr;
    DWORD currentLength = 0;
    hr = outBuffer->Lock(&data, nullptr, &currentLength);
    if (FAILED(hr)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation H.264 output buffer lock failed"), hr);
        }
        return false;
    }
    const QList<QByteArray> nals =
        splitAnnexB(reinterpret_cast<const uint8_t*>(data), int(currentLength));
    outBuffer->Unlock();

    if (m_avcc.isEmpty()) {
        m_avcc = buildAvccFromNals(nals);
    }

    const bool nalKeyframe = containsH264Idr(nals);
    if (nalKeyframe) {
        cleanPoint = 1;
    }

    const QByteArray packet = annexBToLengthPrefixed(nals);
    if (!packet.isEmpty() && onPacket) {
        onPacket(packet, ptsTicks, cleanPoint != 0);
    }
    return true;
}

bool MediaFoundationEncoder::drainOutput(const PacketCallback& onPacket, QString* error) {
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    HRESULT hr = m_transform->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation output stream info query failed"), hr);
        }
        return false;
    }
    const bool encoderProvidesSamples =
        (streamInfo.dwFlags &
         (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    while (true) {
        ComPtr<IMFSample> outputSample;
        if (!encoderProvidesSamples) {
            // Floor the allocation to a frame-sized buffer: cbSize can be a
            // conservative (or zero) hint, and an undersized buffer makes
            // ProcessOutput fail with MF_E_BUFFERTOOSMALL, losing the frame.
            const DWORD cb = std::max<DWORD>(
                streamInfo.cbSize, DWORD(m_config.width) * DWORD(m_config.height) * 3u / 2u);
            ComPtr<IMFMediaBuffer> outputBuffer;
            hr = MFCreateMemoryBuffer(cb, &outputBuffer);
            if (SUCCEEDED(hr)) {
                hr = MFCreateSample(&outputSample);
            }
            if (SUCCEEDED(hr)) {
                hr = outputSample->AddBuffer(outputBuffer.Get());
            }
            if (FAILED(hr)) {
                if (error) {
                    *error = hrMessage(
                        QStringLiteral("Media Foundation output sample allocation failed"), hr);
                }
                return false;
            }
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = outputSample.Get();
        DWORD status = 0;
        hr = m_transform->ProcessOutput(0, 1, &output, &status);

        // Release any event collection the MFT attached, regardless of outcome.
        if (output.pEvents) {
            output.pEvents->Release();
            output.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true; // nothing more buffered right now
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The encoder renegotiated its output type (it can do so to publish
            // the sequence header). Re-set the output type and continue draining.
            ComPtr<IMFMediaType> renegotiated;
            if (output.pSample && encoderProvidesSamples) {
                output.pSample->Release();
                output.pSample = nullptr;
            }
            if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, &renegotiated)) &&
                renegotiated) {
                m_transform->SetOutputType(0, renegotiated.Get(), 0);
            }
            // Recompute provides-samples for the new type and retry.
            MFT_OUTPUT_STREAM_INFO refreshed{};
            if (SUCCEEDED(m_transform->GetOutputStreamInfo(0, &refreshed))) {
                streamInfo = refreshed;
            }
            continue;
        }
        if (FAILED(hr)) {
            if (output.pSample && encoderProvidesSamples) {
                output.pSample->Release();
                output.pSample = nullptr;
            }
            if (error) {
                *error =
                    hrMessage(QStringLiteral("Media Foundation H.264 ProcessOutput failed"), hr);
            }
            return false;
        }

        // Take ownership of the completed sample.
        ComPtr<IMFSample> completed;
        if (encoderProvidesSamples) {
            completed.Attach(output.pSample); // MFT allocated it; we own a ref now
            output.pSample = nullptr;
        } else {
            completed = outputSample;
        }

        if (!emitOutputSample(completed.Get(), onPacket, error)) {
            return false;
        }
        // Loop again to pull any further buffered output samples.
    }
}

// ---------------------------------------------------------------------------
// Async event-pump path (hardware encoder MFTs are almost always async).
//
// Contract for an async MFT: never call ProcessInput/ProcessOutput speculatively.
// Wait for METransformNeedInput before ProcessInput, and METransformHaveOutput
// before ProcessOutput. We drive the pump to completion inside encode()/flush()
// so the caller still sees a synchronous interface.
// ---------------------------------------------------------------------------

bool MediaFoundationEncoder::processInputSample(IMFSample* sample, QString* error) {
    const HRESULT hr = m_transform->ProcessInput(0, sample, 0);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation H.264 ProcessInput failed"), hr);
        }
        return false;
    }
    return true;
}

// Pull exactly one output sample in response to a METransformHaveOutput event.
bool MediaFoundationEncoder::processOutputAsync(const PacketCallback& onPacket, QString* error) {
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    HRESULT hr = m_transform->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation output stream info query failed"), hr);
        }
        return false;
    }
    const bool encoderProvidesSamples =
        (streamInfo.dwFlags &
         (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    ComPtr<IMFSample> outputSample;
    if (!encoderProvidesSamples) {
        // Floor the allocation to a frame-sized buffer (see drainOutput()).
        const DWORD cb = std::max<DWORD>(streamInfo.cbSize,
                                         DWORD(m_config.width) * DWORD(m_config.height) * 3u / 2u);
        ComPtr<IMFMediaBuffer> outputBuffer;
        hr = MFCreateMemoryBuffer(cb, &outputBuffer);
        if (SUCCEEDED(hr)) {
            hr = MFCreateSample(&outputSample);
        }
        if (SUCCEEDED(hr)) {
            hr = outputSample->AddBuffer(outputBuffer.Get());
        }
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(
                    QStringLiteral("Media Foundation output sample allocation failed"), hr);
            }
            return false;
        }
    }

    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    output.pSample = outputSample.Get();
    DWORD status = 0;
    hr = m_transform->ProcessOutput(0, 1, &output, &status);

    if (output.pEvents) {
        output.pEvents->Release();
        output.pEvents = nullptr;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Renegotiate output type and treat this event as drained (the encoder
        // republishes the sample via a subsequent HaveOutput event).
        ComPtr<IMFMediaType> renegotiated;
        if (output.pSample && encoderProvidesSamples) {
            output.pSample->Release();
            output.pSample = nullptr;
        }
        if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, &renegotiated)) && renegotiated) {
            m_transform->SetOutputType(0, renegotiated.Get(), 0);
        }
        return true;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        // Spurious HaveOutput with nothing ready; harmless, treat as drained.
        return true;
    }
    if (FAILED(hr)) {
        if (output.pSample && encoderProvidesSamples) {
            output.pSample->Release();
            output.pSample = nullptr;
        }
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation H.264 ProcessOutput failed"), hr);
        }
        return false;
    }

    ComPtr<IMFSample> completed;
    if (encoderProvidesSamples) {
        completed.Attach(output.pSample); // MFT allocated it; we own a ref now
        output.pSample = nullptr;
    } else {
        completed = outputSample;
    }
    return emitOutputSample(completed.Get(), onPacket, error);
}

bool MediaFoundationEncoder::handleAsyncEvent(IMFMediaEvent* event, IMFSample* inputSample,
                                              bool* submittedInput, const PacketCallback& onPacket,
                                              QString* error) {
    MediaEventType eventType = MediaEventType(0);
    HRESULT hr = event->GetType(&eventType);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation async encoder event type query failed"), hr);
        }
        return false;
    }

    HRESULT eventStatus = S_OK;
    hr = event->GetStatus(&eventStatus);
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation async encoder event status query failed"), hr);
        }
        return false;
    }
    if (FAILED(eventStatus)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation async encoder event failed"),
                               eventStatus);
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
            // No sample to give right now; remember the credit for next encode().
            ++m_asyncNeedInputEvents;
        }
        return true;
    }

    if (eventType == METransformHaveOutput) {
        return processOutputAsync(onPacket, error);
    }

    // METransformDrainComplete / METransformMarker and anything else: nothing to do.
    return true;
}

// Submit one input sample, blocking on the pump until the MFT accepts it, then
// drain any output that is already queued.
bool MediaFoundationEncoder::encodeAsync(IMFSample* inputSample, const PacketCallback& onPacket,
                                         QString* error) {
    bool submittedInput = false;
    if (m_asyncNeedInputEvents > 0) {
        --m_asyncNeedInputEvents;
        if (!processInputSample(inputSample, error)) {
            return false;
        }
        submittedInput = true;
    }

    while (!submittedInput) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = m_eventGenerator->GetEvent(0, &event); // block for the next event
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(
                    QStringLiteral("Media Foundation async encoder event query failed"), hr);
            }
            return false;
        }
        if (!handleAsyncEvent(event.Get(), inputSample, &submittedInput, onPacket, error)) {
            return false;
        }
    }

    return drainReadyAsyncEvents(onPacket, error);
}

// Drain every event currently queued without blocking (pull ready output).
bool MediaFoundationEncoder::drainReadyAsyncEvents(const PacketCallback& onPacket, QString* error) {
    while (true) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = m_eventGenerator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            return true;
        }
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(
                    QStringLiteral("Media Foundation async encoder event query failed"), hr);
            }
            return false;
        }
        bool submittedInput = true; // no input to feed during a non-blocking drain
        if (!handleAsyncEvent(event.Get(), nullptr, &submittedInput, onPacket, error)) {
            return false;
        }
    }
}

// Signal end-of-stream + drain, then block on the pump until the MFT reports
// METransformDrainComplete, emitting every remaining output sample.
bool MediaFoundationEncoder::flushAsync(const PacketCallback& onPacket, QString* error) {
    HRESULT hr = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (SUCCEEDED(hr)) {
        hr = m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation H.264 encoder drain failed"), hr);
        }
        return false;
    }

    while (true) {
        ComPtr<IMFMediaEvent> event;
        hr = m_eventGenerator->GetEvent(0, &event); // block until drain completes
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(
                    QStringLiteral("Media Foundation async encoder event query failed"), hr);
            }
            return false;
        }
        MediaEventType eventType = MediaEventType(0);
        if (FAILED(event->GetType(&eventType))) {
            continue;
        }
        if (eventType == METransformDrainComplete) {
            return true;
        }
        bool submittedInput = true; // do not feed input while draining
        if (!handleAsyncEvent(event.Get(), nullptr, &submittedInput, onPacket, error)) {
            return false;
        }
    }
}

bool MediaFoundationEncoder::encode(const AVFrame* frame, int64_t ptsTicks,
                                    const PacketCallback& onPacket, QString* error) {
    if (!m_transform) {
        if (error) {
            *error = QStringLiteral("Media Foundation H.264 encoder is not initialized");
        }
        return false;
    }

    ComPtr<IMFSample> inputSample;
    if (!buildInputSample(frame, ptsTicks, &inputSample, error)) {
        return false;
    }

    if (m_asyncTransform) {
        return encodeAsync(inputSample.Get(), onPacket, error);
    }

    HRESULT hr = m_transform->ProcessInput(0, inputSample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // Drain pending output to free the encoder, then retry once.
        if (!drainOutput(onPacket, error)) {
            return false;
        }
        hr = m_transform->ProcessInput(0, inputSample.Get(), 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation H.264 ProcessInput failed"), hr);
        }
        return false;
    }

    // All-intra + low-latency: drain immediately so each frame yields its packet.
    return drainOutput(onPacket, error);
}

bool MediaFoundationEncoder::flush(const PacketCallback& onPacket, QString* error) {
    if (!m_transform || !m_streaming) {
        return true;
    }
    if (m_asyncTransform) {
        return flushAsync(onPacket, error);
    }
    HRESULT hr = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (SUCCEEDED(hr)) {
        hr = m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation H.264 encoder drain failed"), hr);
        }
        return false;
    }
    // Pull every remaining buffered output sample.
    return drainOutput(onPacket, error);
}

QByteArray MediaFoundationEncoder::avccExtradata() const {
    return m_avcc;
}

NativeVideoEncoder::~NativeVideoEncoder() = default;

std::unique_ptr<NativeVideoEncoder> NativeVideoEncoder::create(const Config& config,
                                                               QString* error) {
    auto encoder = std::unique_ptr<MediaFoundationEncoder>(new MediaFoundationEncoder());
    if (!encoder->initialize(config, error)) {
        return nullptr;
    }
    return encoder;
}

NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities() {
    NativeVideoEncodeCapabilities caps;
    QString err;
    // Probe a representative 720p30 config; create() is hardware-or-null, so a
    // non-null result means a hardware H.264 encoder MFT is present and usable.
    auto probe = NativeVideoEncoder::create({1280, 720, 30, 1, 4'000'000}, &err);
    caps.h264 = probe != nullptr;
    caps.detail =
        caps.h264
            ? QStringLiteral("Media Foundation H.264 hardware encode available")
            : QStringLiteral("Media Foundation H.264 hardware encode unavailable: %1").arg(err);
    return caps;
}

#endif // _WIN32

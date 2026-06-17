#include "nativeaacdecoder.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QByteArray>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <objbase.h>
// MinGW's mfuuid import library does not export CLSID_CMSAACDecMFT, so instantiate
// wmcodecdsp.h's DEFINE_GUID declarations in this translation unit.
#include <initguid.h>
#include <vector>
#include <wmcodecdsp.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

// Engine-canonical output: 48 kHz, stereo, signed 16-bit interleaved PCM. Downstream
// consumers assume this exact layout, identical to nativeaacdecoder_audiotoolbox.mm.
constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int kBytesPerOutputFrame = kOutputChannels * int(sizeof(int16_t));

// --- Platform-agnostic ADTS parse helpers (kept verbatim in the stub/.mm/MF TUs;
//     intentionally NOT factored into a shared TU to mirror existing duplication). ---
const int kAdtsSampleRates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

quint8 byteAt(const QByteArray& bytes, int offset) {
    return quint8(uchar(bytes[offset]));
}

QString hrMessage(const QString& action, HRESULT hr) {
    return QStringLiteral("%1 (HRESULT 0x%2)")
        .arg(action)
        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

// Map a decoded sample rate back to its 4-bit ADTS/AudioSpecificConfig index.
int sampleRateIndexFor(int sampleRate) {
    for (int i = 0; i < int(std::size(kAdtsSampleRates)); ++i) {
        if (kAdtsSampleRates[i] == sampleRate) {
            return i;
        }
    }
    return -1;
}

} // namespace

class NativeAacDecoder::Impl {
public:
    Impl() = default;
    ~Impl() { shutdownRuntime(); }

    bool decodeAdtsFrame(const QByteArray& frame, const AacAdtsFrameInfo& info,
                         QByteArray* pcmS16Stereo, QString* error);
    void reset();

private:
    ComPtr<IMFTransform> transform;
    int sampleRate = 0;
    int channelCount = 0;
    int audioObjectType = 0;
    // The PCM output format the MFT is ACTUALLY producing (read back from the just-set
    // output type), as opposed to the rate/channels we asked for. The resampler keys off
    // these so a STREAM_CHANGE renegotiation can't silently corrupt pitch/channels.
    int m_outputRate = 0;
    int m_outputChannels = 0;
    bool comInitialized = false;
    bool mfStarted = false;

    bool ensureRuntime(QString* error);
    void shutdownRuntime();
    void teardownTransform();
    bool ensureTransform(const AacAdtsFrameInfo& info, QString* error);
    bool createTransform(QString* error);
    bool configureTypes(const AacAdtsFrameInfo& info, QString* error);
    bool applyOutputType(int rate, int channels, QString* error);
    bool storeNegotiatedOutputFormat(QString* error);
    bool processFrame(const QByteArray& rawAac, std::vector<int16_t>* sourcePcm, QString* error);
    void appendResampledStereo(const std::vector<int16_t>& sourcePcm, int sourceChannels,
                               int sourceRate, QByteArray* pcmS16Stereo) const;
};

bool NativeAacDecoder::Impl::ensureRuntime(QString* error) {
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

void NativeAacDecoder::Impl::shutdownRuntime() {
    teardownTransform();
    if (mfStarted) {
        MFShutdown();
        mfStarted = false;
    }
    if (comInitialized) {
        CoUninitialize();
        comInitialized = false;
    }
}

// Full teardown: end streaming and release the MFT entirely. Used when the configured
// stream format actually changes (new AudioSpecificConfig) and from shutdownRuntime/dtor.
void NativeAacDecoder::Impl::teardownTransform() {
    if (transform) {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    transform.Reset();
    sampleRate = 0;
    channelCount = 0;
    audioObjectType = 0;
    m_outputRate = 0;
    m_outputChannels = 0;
}

// reset() is called by the RTMP caller every audio frame (~21 ms). Releasing and
// re-activating the MFT per frame is very heavy on Windows, so when the stream format is
// unchanged we keep the transform alive and only FLUSH it; ensureTransform() rebuilds it
// lazily if the configured (rate, channels, audioObjectType) changes.
void NativeAacDecoder::Impl::reset() {
    if (transform) {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
}

bool NativeAacDecoder::Impl::createTransform(QString* error) {
    MFT_REGISTER_TYPE_INFO input{};
    input.guidMajorType = MFMediaType_Audio;
    input.guidSubtype = MFAudioFormat_AAC;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr =
        MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT,
                  &input, nullptr, &activates, &count);
    if (SUCCEEDED(hr) && count > 0 && activates) {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform));
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) {
                activates[i]->Release();
            }
        }
        CoTaskMemFree(activates);
        if (SUCCEEDED(hr) && transform) {
            return true;
        }
    } else if (activates) {
        CoTaskMemFree(activates);
    }

    // Fall back to the in-box Microsoft AAC Decoder MFT by CLSID.
    hr = CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&transform));
    if (FAILED(hr) || !transform) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation AAC decoder is unavailable"), hr);
        }
        return false;
    }
    return true;
}

bool NativeAacDecoder::Impl::configureTypes(const AacAdtsFrameInfo& info, QString* error) {
    const int srIndex = sampleRateIndexFor(info.sampleRate);
    if (srIndex < 0) {
        if (error) {
            *error = QStringLiteral("Native RTMP/SRT AAC sample rate %1 is unsupported.")
                         .arg(info.sampleRate);
        }
        return false;
    }

    // AudioSpecificConfig (2 bytes for AAC-LC):
    //   5 bits audioObjectType | 4 bits samplingFrequencyIndex | 4 bits channelConfiguration
    //   | 3 bits (frameLengthFlag, dependsOnCoreCoder, extensionFlag) = 0.
    const quint8 asc0 = quint8(((info.audioObjectType & 0x1f) << 3) | ((srIndex >> 1) & 0x07));
    const quint8 asc1 = quint8(((srIndex & 0x01) << 7) | ((info.channelCount & 0x0f) << 3));

    // MF_MT_USER_DATA for the AAC decoder = the HEAACWAVEINFO tail after WAVEFORMATEX,
    // followed by AudioSpecificConfig bytes.
    std::array<BYTE, 14> userData{};
    userData[0] = 0x00; // wPayloadType lo (0 = raw AAC stream; we strip the ADTS header)
    userData[1] = 0x00; // wPayloadType hi
    userData[2] = 0xFE; // wAudioProfileLevelIndication lo (0xFE = unspecified)
    userData[3] = 0x00; // wAudioProfileLevelIndication hi
    userData[4] = 0x00; // wStructType lo
    userData[5] = 0x00; // wStructType hi
    userData[6] = 0x00; // wReserved1 lo
    userData[7] = 0x00; // wReserved1 hi
    userData[8] = 0x00; // dwReserved2 byte 0
    userData[9] = 0x00; // dwReserved2 byte 1
    userData[10] = 0x00; // dwReserved2 byte 2
    userData[11] = 0x00; // dwReserved2 byte 3
    userData[12] = asc0; // AudioSpecificConfig byte 0
    userData[13] = asc1; // AudioSpecificConfig byte 1

    ComPtr<IMFMediaType> inputType;
    HRESULT hr = MFCreateMediaType(&inputType);
    if (SUCCEEDED(hr)) {
        hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(info.sampleRate));
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(info.channelCount));
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (SUCCEEDED(hr)) {
        // 0 = raw AAC (the MFT input is stripped of the ADTS header). 1 = ADTS.
        hr = inputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);
    }
    if (SUCCEEDED(hr)) {
        hr = inputType->SetBlob(MF_MT_USER_DATA, userData.data(), UINT32(userData.size()));
    }
    if (SUCCEEDED(hr)) {
        hr = transform->SetInputType(0, inputType.Get(), 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation AAC input type setup failed"), hr);
        }
        return false;
    }

    // Output: PCM, 16-bit, at the decoder's native (source) rate and channel count. We
    // resample/upmix to 48 kHz stereo ourselves so the contract matches the Apple path.
    if (!applyOutputType(info.sampleRate, info.channelCount, error)) {
        return false;
    }

    hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) {
        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation AAC decoder stream start failed"), hr);
        }
        return false;
    }
    return true;
}

// Build and set an explicit PCM output type at the given rate/channels, then read back the
// type the MFT actually accepted into m_outputRate/m_outputChannels. Keeping control of the
// output format (rather than accepting the MFT's preferred type) means the resampler always
// sees the real negotiated layout.
bool NativeAacDecoder::Impl::applyOutputType(int rate, int channels, QString* error) {
    ComPtr<IMFMediaType> outputType;
    HRESULT hr = MFCreateMediaType(&outputType);
    if (SUCCEEDED(hr)) {
        hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(rate));
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(channels));
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (SUCCEEDED(hr)) {
        const UINT32 blockAlign = UINT32(channels) * sizeof(int16_t);
        hr = outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        if (SUCCEEDED(hr)) {
            hr = outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                       UINT32(rate) * blockAlign);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = outputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = transform->SetOutputType(0, outputType.Get(), 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation AAC output type setup failed"), hr);
        }
        return false;
    }

    return storeNegotiatedOutputFormat(error);
}

// Query the output type currently set on the MFT and cache its real rate/channels. If the
// MFT does not report a usable rate/channel count we fail rather than guess, so the resampler
// is never fed a stale or wrong layout.
bool NativeAacDecoder::Impl::storeNegotiatedOutputFormat(QString* error) {
    ComPtr<IMFMediaType> currentType;
    HRESULT hr = transform->GetOutputCurrentType(0, &currentType);
    if (FAILED(hr) || !currentType) {
        if (error) {
            *error = hrMessage(
                QStringLiteral("Media Foundation AAC output type query failed"), hr);
        }
        return false;
    }

    UINT32 rate = 0;
    UINT32 channels = 0;
    if (FAILED(currentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) || rate == 0) {
        if (error) {
            *error = QStringLiteral("Media Foundation AAC output type is missing a sample rate.");
        }
        return false;
    }
    if (FAILED(currentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) || channels == 0) {
        if (error) {
            *error =
                QStringLiteral("Media Foundation AAC output type is missing a channel count.");
        }
        return false;
    }

    m_outputRate = int(rate);
    m_outputChannels = int(channels);
    return true;
}

bool NativeAacDecoder::Impl::ensureTransform(const AacAdtsFrameInfo& info, QString* error) {
    // Same configured format → reuse the live MFT (the per-frame reset() only flushed it).
    if (transform && sampleRate == info.sampleRate && channelCount == info.channelCount &&
        audioObjectType == info.audioObjectType) {
        return true;
    }

    // Format actually changed (or first frame) → fully release and rebuild the MFT.
    teardownTransform();
    if (!ensureRuntime(error) || !createTransform(error) || !configureTypes(info, error)) {
        teardownTransform();
        return false;
    }

    sampleRate = info.sampleRate;
    channelCount = info.channelCount;
    audioObjectType = info.audioObjectType;
    return true;
}

bool NativeAacDecoder::Impl::processFrame(const QByteArray& rawAac, std::vector<int16_t>* sourcePcm,
                                          QString* error) {
    ComPtr<IMFMediaBuffer> inputBuffer;
    HRESULT hr = MFCreateMemoryBuffer(DWORD(rawAac.size()), &inputBuffer);
    if (SUCCEEDED(hr)) {
        BYTE* data = nullptr;
        hr = inputBuffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(data, rawAac.constData(), size_t(rawAac.size()));
            inputBuffer->Unlock();
            hr = inputBuffer->SetCurrentLength(DWORD(rawAac.size()));
        }
    }
    if (FAILED(hr)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation AAC input buffer setup failed"), hr);
        }
        return false;
    }

    ComPtr<IMFSample> inputSample;
    hr = MFCreateSample(&inputSample);
    if (SUCCEEDED(hr)) {
        hr = inputSample->AddBuffer(inputBuffer.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = transform->ProcessInput(0, inputSample.Get(), 0);
    }
    if (FAILED(hr)) {
        if (error) {
            *error = hrMessage(QStringLiteral("Media Foundation AAC decode input failed"), hr);
        }
        return false;
    }

    while (true) {
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        hr = transform->GetOutputStreamInfo(0, &streamInfo);
        if (FAILED(hr)) {
            if (error) {
                *error = hrMessage(
                    QStringLiteral("Media Foundation AAC output stream info query failed"), hr);
            }
            return false;
        }

        const bool provideSample = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
        ComPtr<IMFSample> outputSample;
        if (provideSample) {
            const DWORD outputSize = std::max<DWORD>(streamInfo.cbSize, 4096);
            ComPtr<IMFMediaBuffer> outputBuffer;
            hr = MFCreateMemoryBuffer(outputSize, &outputBuffer);
            if (SUCCEEDED(hr)) {
                hr = MFCreateSample(&outputSample);
            }
            if (SUCCEEDED(hr)) {
                hr = outputSample->AddBuffer(outputBuffer.Get());
            }
            if (FAILED(hr)) {
                if (error) {
                    *error = hrMessage(
                        QStringLiteral("Media Foundation AAC output sample allocation failed"), hr);
                }
                return false;
            }
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = outputSample.Get();
        DWORD status = 0;
        hr = transform->ProcessOutput(0, 1, &output, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (output.pEvents) {
                output.pEvents->Release();
            }
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (output.pEvents) {
                output.pEvents->Release();
                output.pEvents = nullptr;
            }
            // The MFT renegotiated its output. Re-assert the explicit PCM shape we want
            // (and re-cache the actually-negotiated rate/channels) rather than blindly
            // accepting the MFT's preferred type. On failure we must NOT continue, or the
            // next ProcessOutput returns STREAM_CHANGE again → unbounded loop.
            if (!applyOutputType(sampleRate, channelCount, error)) {
                return false;
            }
            continue;
        }
        if (FAILED(hr)) {
            if (output.pEvents) {
                output.pEvents->Release();
            }
            if (error) {
                *error = hrMessage(QStringLiteral("Media Foundation AAC decode output failed"), hr);
            }
            return false;
        }

        ComPtr<IMFSample> producedSample;
        if (provideSample) {
            producedSample = outputSample;
        } else {
            producedSample.Attach(output.pSample);
            output.pSample = nullptr;
        }
        if (output.pEvents) {
            output.pEvents->Release();
            output.pEvents = nullptr;
        }

        ComPtr<IMFMediaBuffer> contiguous;
        hr = producedSample->ConvertToContiguousBuffer(&contiguous);
        if (SUCCEEDED(hr)) {
            BYTE* data = nullptr;
            DWORD currentLength = 0;
            hr = contiguous->Lock(&data, nullptr, &currentLength);
            if (SUCCEEDED(hr)) {
                const size_t sampleCount = size_t(currentLength) / sizeof(int16_t);
                const auto* samples = reinterpret_cast<const int16_t*>(data);
                sourcePcm->insert(sourcePcm->end(), samples, samples + sampleCount);
                contiguous->Unlock();
            }
        }
        if (FAILED(hr)) {
            if (error) {
                *error =
                    hrMessage(QStringLiteral("Media Foundation AAC output buffer copy failed"), hr);
            }
            return false;
        }
    }
}

void NativeAacDecoder::Impl::appendResampledStereo(const std::vector<int16_t>& sourcePcm,
                                                   int sourceChannels, int sourceRate,
                                                   QByteArray* pcmS16Stereo) const {
    if (sourceChannels <= 0 || sourceRate <= 0 || sourcePcm.empty()) {
        return;
    }

    const size_t sourceFrames = sourcePcm.size() / size_t(sourceChannels);
    if (sourceFrames == 0) {
        return;
    }

    // Reduce each source frame to a stereo (L, R) pair: mono is duplicated, >=2 channels
    // take the first two. Then linearly resample the stereo stream to 48 kHz if needed.
    auto leftAt = [&](size_t frame) -> int { return sourcePcm[frame * size_t(sourceChannels)]; };
    auto rightAt = [&](size_t frame) -> int {
        return sourceChannels >= 2 ? sourcePcm[frame * size_t(sourceChannels) + 1]
                                   : sourcePcm[frame * size_t(sourceChannels)];
    };

    if (sourceRate == kOutputSampleRate) {
        for (size_t f = 0; f < sourceFrames; ++f) {
            const int16_t l = int16_t(leftAt(f));
            const int16_t r = int16_t(rightAt(f));
            pcmS16Stereo->append(reinterpret_cast<const char*>(&l), sizeof(int16_t));
            pcmS16Stereo->append(reinterpret_cast<const char*>(&r), sizeof(int16_t));
        }
        return;
    }

    const double ratio = double(kOutputSampleRate) / double(sourceRate);
    const size_t outputFrames = size_t(double(sourceFrames) * ratio);
    for (size_t o = 0; o < outputFrames; ++o) {
        const double srcPos = double(o) / ratio;
        const size_t i0 = size_t(srcPos);
        const size_t i1 = std::min(i0 + 1, sourceFrames - 1);
        const double frac = srcPos - double(i0);

        const auto lerp = [&](int a, int b) -> int16_t {
            const double v = double(a) + (double(b) - double(a)) * frac;
            return int16_t(std::clamp<long>(long(v), -32768, 32767));
        };
        const int16_t l = lerp(leftAt(i0), leftAt(i1));
        const int16_t r = lerp(rightAt(i0), rightAt(i1));
        pcmS16Stereo->append(reinterpret_cast<const char*>(&l), sizeof(int16_t));
        pcmS16Stereo->append(reinterpret_cast<const char*>(&r), sizeof(int16_t));
    }
}

bool NativeAacDecoder::Impl::decodeAdtsFrame(const QByteArray& frame, const AacAdtsFrameInfo& info,
                                             QByteArray* pcmS16Stereo, QString* error) {
    if (!pcmS16Stereo) {
        return false;
    }
    pcmS16Stereo->clear();
    if (info.audioObjectType != 2) {
        if (error) {
            *error = QStringLiteral("NativeAacDecoder: only AAC-LC is supported");
        }
        return false;
    }

    const int payloadSize = info.frameSize - info.headerSize;
    if (payloadSize <= 0 || info.headerSize < 0 || info.headerSize + payloadSize > frame.size()) {
        if (error) {
            *error = QStringLiteral("Native RTMP/SRT AAC ADTS frame is malformed.");
        }
        return false;
    }

    if (!ensureTransform(info, error)) {
        return false;
    }

    // The MF AAC decoder MFT wants raw AAC, so drop the ADTS header.
    const QByteArray rawAac = frame.mid(info.headerSize, payloadSize);
    std::vector<int16_t> sourcePcm;
    if (!processFrame(rawAac, &sourcePcm, error)) {
        return false;
    }

    // Key the resampler off the format the MFT actually produced (see m_outputRate /
    // m_outputChannels), NOT the requested info.*, so a STREAM_CHANGE renegotiation cannot
    // silently pitch-shift or channel-swizzle the audio.
    appendResampledStereo(sourcePcm, m_outputChannels, m_outputRate, pcmS16Stereo);
    return true;
}

NativeAacDecoder::NativeAacDecoder() : m_impl(new Impl) {}

NativeAacDecoder::~NativeAacDecoder() {
    delete m_impl;
}

bool NativeAacDecoder::parseAdtsFrame(const QByteArray& bytes, int offset, AacAdtsFrameInfo* info) {
    if (!hasAdtsSync(bytes, offset) || offset + 7 > bytes.size()) {
        return false;
    }

    const int protectionAbsent = byteAt(bytes, offset + 1) & 0x01;
    const int profile = (byteAt(bytes, offset + 2) >> 6) & 0x03;
    const int sampleRateIndex = (byteAt(bytes, offset + 2) >> 2) & 0x0f;
    const int channelConfig =
        ((byteAt(bytes, offset + 2) & 0x01) << 2) | ((byteAt(bytes, offset + 3) >> 6) & 0x03);
    const int frameLength = ((byteAt(bytes, offset + 3) & 0x03) << 11) |
                            (byteAt(bytes, offset + 4) << 3) |
                            ((byteAt(bytes, offset + 5) >> 5) & 0x07);
    const int rawDataBlocks = byteAt(bytes, offset + 6) & 0x03;
    const int headerSize = protectionAbsent ? 7 : 9;

    if (sampleRateIndex < 0 || sampleRateIndex >= int(std::size(kAdtsSampleRates)) ||
        channelConfig <= 0 || frameLength < headerSize || offset + frameLength > bytes.size()) {
        return false;
    }

    if (info) {
        info->headerSize = headerSize;
        info->frameSize = frameLength;
        info->sampleRate = kAdtsSampleRates[sampleRateIndex];
        info->channelCount = channelConfig;
        info->samplesPerFrame = 1024 * (rawDataBlocks + 1);
        info->audioObjectType = profile + 1;
    }
    return true;
}

bool NativeAacDecoder::hasAdtsSync(const QByteArray& bytes, int offset) {
    return offset >= 0 && offset + 2 <= bytes.size() && byteAt(bytes, offset) == 0xff &&
           (byteAt(bytes, offset + 1) & 0xf6) == 0xf0;
}

bool NativeAacDecoder::hasLatmLoasSync(const QByteArray& bytes, int offset) {
    return offset >= 0 && offset + 2 <= bytes.size() && byteAt(bytes, offset) == 0x56 &&
           (byteAt(bytes, offset + 1) & 0xe0) == 0xe0;
}

bool NativeAacDecoder::decodeAdtsFrame(const QByteArray& frame, const AacAdtsFrameInfo& info,
                                       QByteArray* pcmS16Stereo, QString* error) {
    return m_impl && m_impl->decodeAdtsFrame(frame, info, pcmS16Stereo, error);
}

void NativeAacDecoder::reset() {
    if (m_impl) {
        m_impl->reset();
    }
}

#endif // _WIN32

#include "nativeaacdecoder.h"

#ifdef __APPLE__

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int kBytesPerOutputFrame = kOutputChannels * int(sizeof(int16_t));

// Returned by the input proc when it has no more packets for THIS call. Returning
// noErr with 0 packets makes AudioConverter treat the stream as ended, after which
// it permanently stops producing output; a non-noErr sentinel makes it return the
// partial output for this call while keeping the (reused) converter alive.
constexpr OSStatus kNoMoreInputData = 'NOMD';

const int kAdtsSampleRates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350,
};

quint8 byteAt(const QByteArray& bytes, int offset)
{
    return quint8(uchar(bytes[offset]));
}

QString statusMessage(const QString& action, OSStatus status)
{
    return QStringLiteral("%1 (OSStatus %2)").arg(action).arg(status);
}

struct ConverterInput {
    const char* data = nullptr;
    UInt32 size = 0;
    bool consumed = false;
    AudioStreamPacketDescription packetDescription {};
};

OSStatus compressedInputProc(AudioConverterRef, UInt32* ioNumberDataPackets,
                             AudioBufferList* ioData,
                             AudioStreamPacketDescription** outDataPacketDescription,
                             void* inUserData)
{
    auto* input = static_cast<ConverterInput*>(inUserData);
    if (!input || !ioNumberDataPackets || !ioData || input->consumed || input->size == 0) {
        if (ioNumberDataPackets) {
            *ioNumberDataPackets = 0;
        }
        // Signal "no more data for this call" WITHOUT ending the stream (see above).
        return kNoMoreInputData;
    }

    input->packetDescription.mStartOffset = 0;
    input->packetDescription.mVariableFramesInPacket = 0;
    input->packetDescription.mDataByteSize = input->size;

    ioData->mNumberBuffers = 1;
    ioData->mBuffers[0].mNumberChannels = 0;
    ioData->mBuffers[0].mDataByteSize = input->size;
    ioData->mBuffers[0].mData = const_cast<char*>(input->data);
    if (outDataPacketDescription) {
        *outDataPacketDescription = &input->packetDescription;
    }
    *ioNumberDataPackets = 1;
    input->consumed = true;
    return noErr;
}

AudioStreamBasicDescription inputDescription(const AacAdtsFrameInfo& info)
{
    AudioStreamBasicDescription desc {};
    desc.mSampleRate = info.sampleRate;
    desc.mFormatID = kAudioFormatMPEG4AAC;
    desc.mFormatFlags = UInt32(info.audioObjectType);
    desc.mBytesPerPacket = 0;
    desc.mFramesPerPacket = UInt32(info.samplesPerFrame);
    desc.mBytesPerFrame = 0;
    desc.mChannelsPerFrame = UInt32(info.channelCount);
    desc.mBitsPerChannel = 0;
    return desc;
}

AudioStreamBasicDescription outputDescription()
{
    AudioStreamBasicDescription desc {};
    desc.mSampleRate = kOutputSampleRate;
    desc.mFormatID = kAudioFormatLinearPCM;
    desc.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    desc.mBytesPerPacket = kBytesPerOutputFrame;
    desc.mFramesPerPacket = 1;
    desc.mBytesPerFrame = kBytesPerOutputFrame;
    desc.mChannelsPerFrame = kOutputChannels;
    desc.mBitsPerChannel = 16;
    return desc;
}

} // namespace

class NativeAacDecoder::Impl {
public:
    ~Impl() { reset(); }

    bool decodeAdtsFrame(const QByteArray& frame, const AacAdtsFrameInfo& info,
                         QByteArray* pcmS16Stereo, QString* error);
    void reset();

private:
    AudioConverterRef converter = nullptr;
    int sampleRate = 0;
    int channelCount = 0;
    int audioObjectType = 0;

    bool ensureConverter(const AacAdtsFrameInfo& info, QString* error);
};

bool NativeAacDecoder::Impl::ensureConverter(const AacAdtsFrameInfo& info, QString* error)
{
    if (converter && sampleRate == info.sampleRate && channelCount == info.channelCount
        && audioObjectType == info.audioObjectType) {
        return true;
    }

    reset();

    AudioStreamBasicDescription input = inputDescription(info);
    AudioStreamBasicDescription output = outputDescription();
    const OSStatus status = AudioConverterNew(&input, &output, &converter);
    if (status != noErr || !converter) {
        if (error) {
            *error = statusMessage(QStringLiteral("AudioToolbox AAC converter creation failed"),
                                   status);
        }
        return false;
    }

    sampleRate = info.sampleRate;
    channelCount = info.channelCount;
    audioObjectType = info.audioObjectType;
    return true;
}

bool NativeAacDecoder::Impl::decodeAdtsFrame(const QByteArray& frame,
                                                   const AacAdtsFrameInfo& info,
                                                   QByteArray* pcmS16Stereo, QString* error)
{
    if (!pcmS16Stereo) {
        return false;
    }
    pcmS16Stereo->clear();
    if (info.audioObjectType != 2) {
        if (error) {
            *error = QStringLiteral("Native SRT AAC profile %1 is unsupported.")
                         .arg(info.audioObjectType);
        }
        return false;
    }
    if (!ensureConverter(info, error)) {
        return false;
    }

    const int payloadSize = info.frameSize - info.headerSize;
    if (payloadSize <= 0 || info.headerSize < 0 || info.headerSize + payloadSize > frame.size()) {
        if (error) {
            *error = QStringLiteral("Native SRT AAC ADTS frame is malformed.");
        }
        return false;
    }

    ConverterInput input;
    input.data = frame.constData() + info.headerSize;
    input.size = UInt32(payloadSize);

    const int estimatedOutputFrames =
        std::max(1, int(std::ceil(double(info.samplesPerFrame) * kOutputSampleRate
                                  / double(info.sampleRate))) + 512);
    QByteArray output(estimatedOutputFrames * kBytesPerOutputFrame, Qt::Uninitialized);

    AudioBufferList outputBuffers {};
    outputBuffers.mNumberBuffers = 1;
    outputBuffers.mBuffers[0].mNumberChannels = kOutputChannels;
    outputBuffers.mBuffers[0].mDataByteSize = UInt32(output.size());
    outputBuffers.mBuffers[0].mData = output.data();

    UInt32 outputPackets = UInt32(estimatedOutputFrames);
    const OSStatus status = AudioConverterFillComplexBuffer(
        converter, compressedInputProc, &input, &outputPackets, &outputBuffers, nullptr);
    // kNoMoreInputData is our own "ran out of input for this call" sentinel from the
    // input proc — not a failure; the partial output (this packet's samples) is valid.
    if (status != noErr && status != kNoMoreInputData) {
        if (error) {
            *error = statusMessage(QStringLiteral("AudioToolbox AAC decode failed"), status);
        }
        return false;
    }
    if (outputPackets == 0 || outputBuffers.mBuffers[0].mDataByteSize == 0) {
        return true;
    }

    output.truncate(int(outputBuffers.mBuffers[0].mDataByteSize));
    *pcmS16Stereo = output;
    return true;
}

void NativeAacDecoder::Impl::reset()
{
    if (converter) {
        AudioConverterDispose(converter);
        converter = nullptr;
    }
    sampleRate = 0;
    channelCount = 0;
    audioObjectType = 0;
}

NativeAacDecoder::NativeAacDecoder()
    : m_impl(new Impl)
{
}

NativeAacDecoder::~NativeAacDecoder()
{
    delete m_impl;
}

bool NativeAacDecoder::parseAdtsFrame(const QByteArray& bytes, int offset,
                                            AacAdtsFrameInfo* info)
{
    if (!hasAdtsSync(bytes, offset) || offset + 7 > bytes.size()) {
        return false;
    }

    const int protectionAbsent = byteAt(bytes, offset + 1) & 0x01;
    const int profile = (byteAt(bytes, offset + 2) >> 6) & 0x03;
    const int sampleRateIndex = (byteAt(bytes, offset + 2) >> 2) & 0x0f;
    const int channelConfig = ((byteAt(bytes, offset + 2) & 0x01) << 2)
        | ((byteAt(bytes, offset + 3) >> 6) & 0x03);
    const int frameLength = ((byteAt(bytes, offset + 3) & 0x03) << 11)
        | (byteAt(bytes, offset + 4) << 3)
        | ((byteAt(bytes, offset + 5) >> 5) & 0x07);
    const int rawDataBlocks = byteAt(bytes, offset + 6) & 0x03;
    const int headerSize = protectionAbsent ? 7 : 9;

    if (sampleRateIndex < 0 || sampleRateIndex >= int(std::size(kAdtsSampleRates))
        || channelConfig <= 0 || frameLength < headerSize
        || offset + frameLength > bytes.size()) {
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

bool NativeAacDecoder::hasAdtsSync(const QByteArray& bytes, int offset)
{
    return offset >= 0 && offset + 2 <= bytes.size()
        && byteAt(bytes, offset) == 0xff
        && (byteAt(bytes, offset + 1) & 0xf6) == 0xf0;
}

bool NativeAacDecoder::hasLatmLoasSync(const QByteArray& bytes, int offset)
{
    return offset >= 0 && offset + 2 <= bytes.size()
        && byteAt(bytes, offset) == 0x56
        && (byteAt(bytes, offset + 1) & 0xe0) == 0xe0;
}

bool NativeAacDecoder::decodeAdtsFrame(const QByteArray& frame,
                                             const AacAdtsFrameInfo& info,
                                             QByteArray* pcmS16Stereo, QString* error)
{
    return m_impl && m_impl->decodeAdtsFrame(frame, info, pcmS16Stereo, error);
}

void NativeAacDecoder::reset()
{
    if (m_impl) {
        m_impl->reset();
    }
}

#endif // __APPLE__

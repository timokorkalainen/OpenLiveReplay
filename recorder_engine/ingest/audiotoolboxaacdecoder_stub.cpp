#include "audiotoolboxaacdecoder.h"

#ifndef __APPLE__

#include <algorithm>
#include <iterator>

namespace {

const int kAdtsSampleRates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350,
};

quint8 byteAt(const QByteArray& bytes, int offset)
{
    return quint8(uchar(bytes[offset]));
}

} // namespace

class AudioToolboxAacDecoder::Impl {};

AudioToolboxAacDecoder::AudioToolboxAacDecoder()
    : m_impl(new Impl)
{
}

AudioToolboxAacDecoder::~AudioToolboxAacDecoder()
{
    delete m_impl;
}

bool AudioToolboxAacDecoder::parseAdtsFrame(const QByteArray& bytes, int offset,
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

bool AudioToolboxAacDecoder::hasAdtsSync(const QByteArray& bytes, int offset)
{
    return offset >= 0 && offset + 2 <= bytes.size()
        && byteAt(bytes, offset) == 0xff
        && (byteAt(bytes, offset + 1) & 0xf6) == 0xf0;
}

bool AudioToolboxAacDecoder::hasLatmLoasSync(const QByteArray& bytes, int offset)
{
    return offset >= 0 && offset + 2 <= bytes.size()
        && byteAt(bytes, offset) == 0x56
        && (byteAt(bytes, offset + 1) & 0xe0) == 0xe0;
}

bool AudioToolboxAacDecoder::decodeAdtsFrame(const QByteArray&, const AacAdtsFrameInfo&,
                                             QByteArray*, QString* error)
{
    if (error) {
        *error = QStringLiteral("AudioToolbox is unavailable on this platform");
    }
    return false;
}

void AudioToolboxAacDecoder::reset() {}

#endif // !__APPLE__

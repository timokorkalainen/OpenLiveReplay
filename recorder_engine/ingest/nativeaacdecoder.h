#ifndef NATIVEAACDECODER_H
#define NATIVEAACDECODER_H

#include <QByteArray>
#include <QString>

struct AacAdtsFrameInfo {
    int headerSize = 0;
    int frameSize = 0;
    int sampleRate = 0;
    int channelCount = 0;
    int samplesPerFrame = 1024;
    int audioObjectType = 0;
};

// Native AAC (ADTS) decoder: AudioToolbox on Apple, Media Foundation on Windows, stub elsewhere.
class NativeAacDecoder {
public:
    NativeAacDecoder();
    ~NativeAacDecoder();

    NativeAacDecoder(const NativeAacDecoder&) = delete;
    NativeAacDecoder& operator=(const NativeAacDecoder&) = delete;

    static bool parseAdtsFrame(const QByteArray& bytes, int offset, AacAdtsFrameInfo* info);
    static bool hasAdtsSync(const QByteArray& bytes, int offset);
    static bool hasLatmLoasSync(const QByteArray& bytes, int offset);

    bool decodeAdtsFrame(const QByteArray& frame, const AacAdtsFrameInfo& info,
                         QByteArray* pcmS16Stereo, QString* error);
    void reset();

private:
    class Impl;
    Impl* m_impl = nullptr;
};

#endif // NATIVEAACDECODER_H

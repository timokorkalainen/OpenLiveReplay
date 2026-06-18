#ifndef NATIVEVIDEOENCODER_H
#define NATIVEVIDEOENCODER_H

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

extern "C" {
struct AVFrame;
}

struct NativeVideoEncodeCapabilities {
    bool h264 = false;
    QString detail;
};

class NativeVideoEncoder {
public:
    struct Config {
        int width = 0;
        int height = 0;
        int fpsNum = 30;
        int fpsDen = 1;
        int bitrate = 30'000'000;
    };
    using PacketCallback =
        std::function<void(const QByteArray& data, int64_t ptsTicks, bool keyframe)>;

    // Returns nullptr (and sets *error) if a hardware H.264 encoder cannot be
    // opened. Never returns a software encoder.
    static std::unique_ptr<NativeVideoEncoder> create(const Config& config, QString* error);

    virtual ~NativeVideoEncoder();

    NativeVideoEncoder(const NativeVideoEncoder&) = delete;
    NativeVideoEncoder& operator=(const NativeVideoEncoder&) = delete;

    // Encode one CPU YUV420P frame (all-intra → one keyframe packet),
    // synchronously draining output to onPacket. ptsTicks is opaque (echoed).
    virtual bool encode(const AVFrame* frame, int64_t ptsTicks,
                        const PacketCallback& onPacket, QString* error) = 0;
    virtual bool flush(const PacketCallback& onPacket, QString* error) = 0;
    virtual QByteArray avccExtradata() const = 0;

protected:
    NativeVideoEncoder() = default;
};

NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities();

#endif // NATIVEVIDEOENCODER_H

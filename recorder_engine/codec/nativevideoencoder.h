#ifndef NATIVEVIDEOENCODER_H
#define NATIVEVIDEOENCODER_H

#include "playback/output/colormetadata.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <memory>
#include <type_traits>

extern "C" {
struct AVFrame;
}

class GpuSurface;

struct NativeVideoEncodeCapabilities {
    bool h264 = false;
    QString detail;
};

class NativeVideoEncoder {
public:
    struct Config {
        Config() = default;
        Config(int width_, int height_, int fpsNum_ = 30, int fpsDen_ = 1,
               int bitrate_ = 30'000'000, ColorMetadata color_ = {})
            : width(width_), height(height_), fpsNum(fpsNum_), fpsDen(fpsDen_), bitrate(bitrate_),
              color(color_) {}

        int width = 0;
        int height = 0;
        int fpsNum = 30;
        int fpsDen = 1;
        int bitrate = 30'000'000;
        ColorMetadata color;
    };
    struct PacketCallback {
        using Function = void (*)(void* context, uint64_t id, const QByteArray& data,
                                  int64_t ptsTicks, bool keyframe);

        void* context = nullptr;
        uint64_t id = 0;
        Function function = nullptr;

        void operator()(const QByteArray& data, int64_t ptsTicks, bool keyframe) const {
            if (function) function(context, id, data, ptsTicks, keyframe);
        }
        explicit operator bool() const noexcept { return function != nullptr; }

        template <typename Callable>
        static PacketCallback bind(Callable& callable) noexcept {
            return PacketCallback{&callable, 0,
                                  [](void* context, uint64_t, const QByteArray& data,
                                     int64_t ptsTicks, bool keyframe) {
                                      (*static_cast<Callable*>(context))(data, ptsTicks, keyframe);
                                  }};
        }
    };
    static_assert(std::is_trivially_copyable_v<PacketCallback>);

    // Returns nullptr (and sets *error) if a hardware H.264 encoder cannot be
    // opened. Never returns a software encoder.
    static std::unique_ptr<NativeVideoEncoder> create(const Config& config, QString* error);

    virtual ~NativeVideoEncoder();

    NativeVideoEncoder(const NativeVideoEncoder&) = delete;
    NativeVideoEncoder& operator=(const NativeVideoEncoder&) = delete;

    // Encode one CPU YUV420P frame (all-intra → one keyframe packet),
    // synchronously draining output to onPacket. ptsTicks is opaque (echoed).
    virtual bool encode(const AVFrame* frame, int64_t ptsTicks, const PacketCallback& onPacket,
                        QString* error) = 0;
    // Encode one GPU-resident NV12 surface without a CPU re-upload. The caller
    // must fence the surface before calling; this synchronous API completes VT/MF
    // frame submission before returning.
    virtual bool encodeSurface(GpuSurface* surface, int64_t ptsTicks, const ColorMetadata& color,
                               const PacketCallback& onPacket, QString* error) = 0;
    virtual bool flush(const PacketCallback& onPacket, QString* error) = 0;
    // Returns the avcC (AVCDecoderConfigurationRecord) blob for the current
    // encoding session. PRECONDITION: at least one successful encode() call must
    // have completed before avccExtradata() returns a valid (non-empty) result.
    // The SPS/PPS are derived from the first encoded frame's output; before that
    // call the encoder has not yet negotiated its output type and this returns an
    // empty QByteArray.
    virtual QByteArray avccExtradata() const = 0;

protected:
    NativeVideoEncoder() = default;
};

NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities();

#endif // NATIVEVIDEOENCODER_H

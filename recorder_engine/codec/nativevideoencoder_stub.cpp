#include "recorder_engine/codec/nativevideoencoder.h"

NativeVideoEncoder::~NativeVideoEncoder() = default;

bool NativeVideoEncoder::encodeSurface(GpuSurface*, int64_t, const ColorMetadata&,
                                       const PacketCallback&, QString* error) {
    if (error) *error = QStringLiteral("GPU-surface encode unavailable on this platform");
    return false;
}

std::unique_ptr<NativeVideoEncoder> NativeVideoEncoder::create(const Config&, QString* error) {
    if (error) *error = QStringLiteral("No hardware H.264 encoder on this platform");
    return nullptr;
}

NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities() {
    NativeVideoEncodeCapabilities caps;
    caps.h264 = false;
    caps.detail = QStringLiteral("No native H.264 encoder backend for this platform");
    return caps;
}

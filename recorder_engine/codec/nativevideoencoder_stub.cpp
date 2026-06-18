#include "recorder_engine/codec/nativevideoencoder.h"

NativeVideoEncoder::~NativeVideoEncoder() = default;

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

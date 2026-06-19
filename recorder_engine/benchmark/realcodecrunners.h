#ifndef OLR_REALCODECRUNNERS_H
#define OLR_REALCODECRUNNERS_H

#include "recorder_engine/benchmark/codecrunner.h"
#include "recorder_engine/benchmark/syntheticframes.h"

// FFmpeg MPEG-2 intra encode + decode pipeline. Always available.
class Mpeg2CodecRunner : public CodecRunner {
public:
    Mpeg2CodecRunner() = default;
    bool available() const override { return true; }
    RampStepResult runStep(int concurrency, const BenchmarkConfig& config,
                           const std::atomic<bool>& cancel) override;
};

// Native hardware H.264 encode (NativeVideoEncoder) + decode (NativeVideoDecoder).
// available() = VideoToolbox/MediaFoundation H.264 encoder AND decoder present.
class H264CodecRunner : public CodecRunner {
public:
    H264CodecRunner() = default;
    bool available() const override;
    RampStepResult runStep(int concurrency, const BenchmarkConfig& config,
                           const std::atomic<bool>& cancel) override;
};

#endif // OLR_REALCODECRUNNERS_H

#include "recorder_engine/benchmark/runcodecbenchmark.h"
#include "recorder_engine/benchmark/benchmarkcache.h"
#include "recorder_engine/benchmark/benchmarkplan.h"
#include "recorder_engine/benchmark/realcodecrunners.h"
#include "recorder_engine/codec/videocodecchoice.h"

CodecBenchmarkResult runCodecBenchmark(const BenchmarkConfig& config,
                                       const CodecBenchmark::ProgressFn& onStep,
                                       const std::atomic<bool>& cancel) {
    CodecBenchmarkResult result;

    // --- MPEG-2 (always available) ---
    Mpeg2CodecRunner mpeg2Runner;
    const CodecBenchmark::CodecResult mpeg2 =
        CodecBenchmark::rampCodec(mpeg2Runner, config, onStep, cancel);
    result.mpeg2SafeFeeds = mpeg2.safeFeeds;
    result.mpeg2EncodeMs = mpeg2.encodeMs;
    result.mpeg2DecodeMs = mpeg2.decodeMs;

    // --- H.264 (hardware, only if available) ---
    H264CodecRunner h264Runner;
    result.h264Available = H264CodecRunner::hardwareAvailable();
    if (result.h264Available && h264Runner.available() && !cancel.load()) {
        const CodecBenchmark::CodecResult h264 =
            CodecBenchmark::rampCodec(h264Runner, config, onStep, cancel);
        result.h264SafeFeeds = h264.safeFeeds;
        result.h264EncodeMs = h264.encodeMs;
        result.h264DecodeMs = h264.decodeMs;
        result.ceilingReached = mpeg2.ceilingReached || h264.ceilingReached;
    } else {
        result.h264SafeFeeds = -1;
        result.ceilingReached = mpeg2.ceilingReached;
    }

    // --- Recommendation ---
    result.recommended =
        recommendCodec(result.h264Available, result.h264SafeFeeds, result.mpeg2SafeFeeds);

    // --- Metadata (timestamp left for caller) ---
    result.deviceLabel = benchmarkDeviceLabel();
    result.resolution = QString::number(config.width) + QStringLiteral("x") +
                        QString::number(config.height) + QStringLiteral("@") +
                        QString::number(config.fps);

    return result;
}

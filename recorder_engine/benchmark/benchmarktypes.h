#ifndef OLR_BENCHMARKTYPES_H
#define OLR_BENCHMARKTYPES_H

#include "recorder_engine/codec/videocodecchoice.h"

#include <QString>
#include <cstdint>

struct BenchmarkConfig {
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrate = 30'000'000;
    int durationMsPerStep = 3000; // measurement window per ramp step
};

struct RampStepResult {
    int concurrency = 0;
    int framesProcessed = 0;
    int64_t framesRequired = 0;
    bool budgetMet = true;
    bool startupFailed = false; // true if any thread failed to construct its codec session
    double avgEncodeMs = 0.0;
    double avgDecodeMs = 0.0;
};

struct CodecBenchmarkResult {
    bool h264Available = false;
    // -1 = not measured / codec unavailable; 0 = measured but sustained zero feeds; >0 = safe feed count
    int  h264SafeFeeds = -1;
    // -1 = not measured / codec unavailable; 0 = measured but sustained zero feeds; >0 = safe feed count
    int  mpeg2SafeFeeds = -1;
    double h264EncodeMs = 0.0, h264DecodeMs = 0.0;
    double mpeg2EncodeMs = 0.0, mpeg2DecodeMs = 0.0;
    VideoCodecChoice recommended = VideoCodecChoice::Mpeg2Software;
    QString deviceLabel;
    QString resolution;     // e.g. "1920x1080@30"
    QString timestamp;      // ISO-8601, stamped by the caller
    bool ceilingReached = false; // true if a codec ramp hit N=32 without failing
};

#endif // OLR_BENCHMARKTYPES_H

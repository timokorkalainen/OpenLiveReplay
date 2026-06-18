#include "ndi_marker_pattern.h"

#include <Processing.NDI.Lib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> g_stop{false};

void handleSignal(int) {
    g_stop = true;
}

std::string argValue(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

int intArg(int argc, char** argv, const char* name, int fallback) {
    return std::atoi(argValue(argc, argv, name, std::to_string(fallback).c_str()).c_str());
}

double doubleArg(int argc, char** argv, const char* name, double fallback) {
    return std::atof(argValue(argc, argv, name, std::to_string(fallback).c_str()).c_str());
}

struct NdiSource {
    std::string name;
    NDIlib_send_instance_t sender = nullptr;
    std::vector<uint8_t> video;
    std::vector<float> audio;
};
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    NdiMarkerConfig config;
    config.width = intArg(argc, argv, "--width", config.width);
    config.height = intArg(argc, argv, "--height", config.height);
    config.frameRateNumerator = intArg(argc, argv, "--fps-num", config.frameRateNumerator);
    config.frameRateDenominator = intArg(argc, argv, "--fps-den", config.frameRateDenominator);
    config.skewPpm = doubleArg(argc, argv, "--skew-ppm", 0.0);
    config.startTimecode =
        QString::fromStdString(argValue(argc, argv, "--timecode", "10:00:00:00"));

    const int sources = intArg(argc, argv, "--sources", 1);
    const int seconds = intArg(argc, argv, "--seconds", 20);
    const std::string prefix = argValue(argc, argv, "--name-prefix", "OLR-FS-NDI");
    if (sources <= 0 || seconds <= 0 || config.width <= 0 || config.height <= 0 ||
        config.frameRateNumerator <= 0 || config.frameRateDenominator <= 0) {
        std::fprintf(stderr, "invalid NDI marker sender arguments\n");
        return 2;
    }

    if (!NDIlib_initialize()) {
        std::fprintf(stderr, "NDI runtime failed to initialize\n");
        return 77;
    }

    std::vector<NdiSource> ndiSources;
    ndiSources.reserve(size_t(sources));
    for (int i = 0; i < sources; ++i) {
        NdiSource source;
        source.name = prefix + "-" + std::to_string(i);
        NDIlib_send_create_t create{};
        create.p_ndi_name = source.name.c_str();
        create.clock_video = false;
        create.clock_audio = false;
        source.sender = NDIlib_send_create(&create);
        if (!source.sender) {
            std::fprintf(stderr, "failed to create NDI sender %s\n", source.name.c_str());
            for (NdiSource& existing : ndiSources) {
                NDIlib_send_destroy(existing.sender);
            }
            NDIlib_destroy();
            return 1;
        }
        std::fprintf(stderr, "[ndi-marker] source=%s\n", source.name.c_str());
        ndiSources.push_back(std::move(source));
    }

    const int64_t totalFrames =
        int64_t(seconds) * config.frameRateNumerator / config.frameRateDenominator;
    const double frameSeconds =
        double(config.frameRateDenominator) / double(config.frameRateNumerator);
    const auto start = std::chrono::steady_clock::now();

    for (int64_t frameIndex = 0; frameIndex < totalFrames && !g_stop; ++frameIndex) {
        // Keep delivery real-time; --skew-ppm is a media-clock/timestamp skew.
        const auto due =
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(double(frameIndex) * frameSeconds));
        std::this_thread::sleep_until(due);

        for (NdiSource& source : ndiSources) {
            fillNdiMarkerUyvyFrame(config, frameIndex, source.video);
            fillNdiMarkerAudioFrame(config, frameIndex, source.audio);

            NDIlib_audio_frame_v3_t audio{};
            audio.sample_rate = config.sampleRate;
            audio.no_channels = config.channels;
            audio.no_samples = ndiMarkerSamplesForFrame(config, frameIndex);
            audio.timecode = ndiMarkerTimecode100ns(config, frameIndex);
            audio.FourCC = NDIlib_FourCC_audio_type_FLTP;
            audio.p_data = reinterpret_cast<uint8_t*>(source.audio.data());
            audio.channel_stride_in_bytes = audio.no_samples * int(sizeof(float));
            audio.timestamp = ndiMarkerTimestamp100ns(config, frameIndex);
            NDIlib_send_send_audio_v3(source.sender, &audio);

            NDIlib_video_frame_v2_t video{};
            video.xres = config.width;
            video.yres = config.height;
            video.FourCC = NDIlib_FourCC_video_type_UYVY;
            video.frame_rate_N = config.frameRateNumerator;
            video.frame_rate_D = config.frameRateDenominator;
            video.picture_aspect_ratio = 0.0f;
            video.frame_format_type = NDIlib_frame_format_type_progressive;
            video.timecode = ndiMarkerTimecode100ns(config, frameIndex);
            video.p_data = source.video.data();
            video.line_stride_in_bytes = config.width * 2;
            video.timestamp = ndiMarkerTimestamp100ns(config, frameIndex);
            NDIlib_send_send_video_v2(source.sender, &video);
        }
    }

    for (NdiSource& source : ndiSources) {
        NDIlib_send_destroy(source.sender);
    }
    NDIlib_destroy();
    return 0;
}

#include "ndi_marker_pattern.h"

#include <QtCore/QStringList>

#include <algorithm>
#include <cmath>

namespace {
constexpr int64_t kHundredNsPerSecond = 10000000;

int framesPerSecond(const NdiMarkerConfig& config) {
    return std::max(1, config.frameRateNumerator / std::max(1, config.frameRateDenominator));
}
}

int ndiMarkerSamplesPerFrame(const NdiMarkerConfig& config) {
    return int(std::llround(double(config.sampleRate) * double(config.frameRateDenominator) /
                            double(config.frameRateNumerator)));
}

bool ndiMarkerIsActive(const NdiMarkerConfig& config, int64_t frameIndex) {
    const int frameInSecond = int(frameIndex % framesPerSecond(config));
    return frameInSecond >= 0 && frameInSecond < config.markerFrames;
}

int64_t ndiMarkerTimestamp100ns(const NdiMarkerConfig& config, int64_t frameIndex) {
    const double factor = 1.0 + config.skewPpm / 1000000.0;
    const double seconds = double(frameIndex) * double(config.frameRateDenominator) /
                           double(config.frameRateNumerator) * factor;
    return int64_t(std::llround(seconds * double(kHundredNsPerSecond)));
}

int64_t ndiMarkerStartTimecode100ns(const NdiMarkerConfig& config) {
    const QStringList parts = config.startTimecode.split(QLatin1Char(':'));
    if (parts.size() != 4) {
        return 0;
    }

    bool ok = true;
    const int hours = parts[0].toInt(&ok);
    if (!ok) {
        return 0;
    }
    const int minutes = parts[1].toInt(&ok);
    if (!ok) {
        return 0;
    }
    const int seconds = parts[2].toInt(&ok);
    if (!ok) {
        return 0;
    }
    const int frames = parts[3].toInt(&ok);
    if (!ok) {
        return 0;
    }

    const int64_t wholeSeconds = int64_t(hours) * 3600 + int64_t(minutes) * 60 + seconds;
    const double frameSeconds =
        double(frames) * double(config.frameRateDenominator) / double(config.frameRateNumerator);
    return wholeSeconds * kHundredNsPerSecond +
           int64_t(std::llround(frameSeconds * double(kHundredNsPerSecond)));
}

int64_t ndiMarkerTimecode100ns(const NdiMarkerConfig& config, int64_t frameIndex) {
    return ndiMarkerStartTimecode100ns(config) + ndiMarkerTimestamp100ns(config, frameIndex);
}

void fillNdiMarkerUyvyFrame(const NdiMarkerConfig& config, int64_t frameIndex,
                            std::vector<uint8_t>& pixels) {
    const uint8_t y = ndiMarkerIsActive(config, frameIndex) ? 235 : 16;
    pixels.assign(size_t(config.width * config.height * 2), 0);
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        pixels[i + 0] = 128;
        pixels[i + 1] = y;
        pixels[i + 2] = 128;
        pixels[i + 3] = y;
    }
}

void fillNdiMarkerAudioFrame(const NdiMarkerConfig& config, int64_t frameIndex,
                             std::vector<float>& samples) {
    const int perChannel = ndiMarkerSamplesPerFrame(config);
    samples.assign(size_t(config.channels * perChannel), 0.0f);
    if (!ndiMarkerIsActive(config, frameIndex)) {
        return;
    }

    const double frequency = 1000.0;
    const int64_t firstSample = frameIndex * int64_t(perChannel);
    for (int channel = 0; channel < config.channels; ++channel) {
        float* channelData = samples.data() + size_t(channel * perChannel);
        for (int i = 0; i < perChannel; ++i) {
            const double t = double(firstSample + i) / double(config.sampleRate);
            channelData[i] = float(std::sin(2.0 * M_PI * frequency * t) * 0.75);
        }
    }
}

#ifndef OLR_NDI_MARKER_PATTERN_H
#define OLR_NDI_MARKER_PATTERN_H

#include <QtCore/QString>

#include <cstdint>
#include <vector>

struct NdiMarkerConfig {
    int width = 320;
    int height = 240;
    int frameRateNumerator = 30;
    int frameRateDenominator = 1;
    int sampleRate = 48000;
    int channels = 2;
    int markerFrames = 2;
    double skewPpm = 0.0;
    QString startTimecode = QStringLiteral("10:00:00:00");
};

int ndiMarkerSamplesPerFrame(const NdiMarkerConfig& config);
bool ndiMarkerIsActive(const NdiMarkerConfig& config, int64_t frameIndex);
int64_t ndiMarkerTimestamp100ns(const NdiMarkerConfig& config, int64_t frameIndex);
int64_t ndiMarkerStartTimecode100ns(const NdiMarkerConfig& config);
int64_t ndiMarkerTimecode100ns(const NdiMarkerConfig& config, int64_t frameIndex);
void fillNdiMarkerUyvyFrame(const NdiMarkerConfig& config, int64_t frameIndex,
                            std::vector<uint8_t>& pixels);
void fillNdiMarkerAudioFrame(const NdiMarkerConfig& config, int64_t frameIndex,
                             std::vector<float>& samples);

#endif

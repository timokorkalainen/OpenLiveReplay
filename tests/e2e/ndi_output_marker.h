#ifndef OLR_NDI_OUTPUT_MARKER_H
#define OLR_NDI_OUTPUT_MARKER_H

#include <QByteArray>
#include <QtGlobal>

// A deterministic test marker designed to survive I420/NDI transport: the luma plane carries
// a block-coded frame counter (top-left) and a flash cell (top-right); marker frames also
// carry an audio tone burst. The receiver reads these back to detect dropped/duplicated/
// reordered frames and to measure audio-video sync.
struct NdiOutputMarkerConfig {
    int width = 256;
    int height = 144;
    int fpsNum = 30;
    int fpsDen = 1;
    int sampleRate = 48000;
    int channels = 2;
    int flashPeriod = 15; // a flash every Nth frame
};

int ndiMarkerSamplesPerFrame(const NdiOutputMarkerConfig& cfg);
bool ndiMarkerIsFlashFrame(const NdiOutputMarkerConfig& cfg, qint64 frameIndex);
QByteArray ndiMarkerLumaPlane(const NdiOutputMarkerConfig& cfg, qint64 frameIndex);
qint64 ndiMarkerDecodeIndex(const NdiOutputMarkerConfig& cfg, const uchar* luma, int stride);
bool ndiMarkerDecodeFlash(const NdiOutputMarkerConfig& cfg, const uchar* luma, int stride);
QByteArray ndiMarkerAudioS16(const NdiOutputMarkerConfig& cfg, qint64 frameIndex);
double ndiMarkerAudioRmsFltp(const float* plane, int samples);

#endif // OLR_NDI_OUTPUT_MARKER_H

#include "tests/e2e/ndi_output_marker.h"

#include <cmath>

namespace {
constexpr int kCell = 8; // cell size in pixels
constexpr int kCounterBits = 24;
constexpr uchar kHi = 235; // "1" / bright
constexpr uchar kLo = 16;  // "0" / dark
constexpr uchar kBg = 128; // neutral background
constexpr double kBeepHz = 1000.0;
constexpr double kBeepAmp = 0.4;

// Center pixel of cell (col,row) read with clamping to the plane.
uchar cellSample(const uchar* luma, int stride, int width, int height, int col, int row) {
    const int x = qMin(width - 1, col * kCell + kCell / 2);
    const int y = qMin(height - 1, row * kCell + kCell / 2);
    return luma[y * stride + x];
}

void fillCell(QByteArray& y, int width, int col, int row, uchar value) {
    auto* p = reinterpret_cast<uchar*>(y.data());
    for (int dy = 0; dy < kCell; ++dy) {
        for (int dx = 0; dx < kCell; ++dx) {
            p[(row * kCell + dy) * width + (col * kCell + dx)] = value;
        }
    }
}
} // namespace

int ndiMarkerSamplesPerFrame(const NdiOutputMarkerConfig& cfg) {
    if (cfg.fpsNum <= 0) return cfg.sampleRate / 30;
    return int((qint64(cfg.sampleRate) * cfg.fpsDen) / cfg.fpsNum);
}

bool ndiMarkerIsFlashFrame(const NdiOutputMarkerConfig& cfg, qint64 frameIndex) {
    const int period = qMax(1, cfg.flashPeriod);
    return (frameIndex % period) == 0;
}

QByteArray ndiMarkerLumaPlane(const NdiOutputMarkerConfig& cfg, qint64 frameIndex) {
    QByteArray y(cfg.width * cfg.height, char(kBg));
    // Counter: bit i in cell column i of the top row, MSB at column 0.
    const quint32 idx = quint32(frameIndex) & ((1u << kCounterBits) - 1u);
    for (int bit = 0; bit < kCounterBits; ++bit) {
        const bool one = (idx >> (kCounterBits - 1 - bit)) & 1u;
        fillCell(y, cfg.width, bit, 0, one ? kHi : kLo);
    }
    // Flash cell: last cell column of the SECOND row (away from the counter row).
    const int flashCol = (cfg.width / kCell) - 1;
    fillCell(y, cfg.width, flashCol, 1, ndiMarkerIsFlashFrame(cfg, frameIndex) ? kHi : kLo);
    return y;
}

qint64 ndiMarkerDecodeIndex(const NdiOutputMarkerConfig& cfg, const uchar* luma, int stride) {
    if (!luma || stride < cfg.width) return -1;
    quint32 idx = 0;
    for (int bit = 0; bit < kCounterBits; ++bit) {
        const uchar v = cellSample(luma, stride, cfg.width, cfg.height, bit, 0);
        idx = (idx << 1) | (v > 128 ? 1u : 0u);
    }
    return qint64(idx);
}

bool ndiMarkerDecodeFlash(const NdiOutputMarkerConfig& cfg, const uchar* luma, int stride) {
    if (!luma || stride < cfg.width) return false;
    const int flashCol = (cfg.width / kCell) - 1;
    return cellSample(luma, stride, cfg.width, cfg.height, flashCol, 1) > 128;
}

QByteArray ndiMarkerAudioS16(const NdiOutputMarkerConfig& cfg, qint64 frameIndex) {
    const int n = ndiMarkerSamplesPerFrame(cfg);
    QByteArray pcm(n * cfg.channels * int(sizeof(qint16)), '\0');
    if (!ndiMarkerIsFlashFrame(cfg, frameIndex)) return pcm;
    auto* s = reinterpret_cast<qint16*>(pcm.data());
    for (int k = 0; k < n; ++k) {
        const double t = double(k) / double(cfg.sampleRate);
        const double v = kBeepAmp * std::sin(2.0 * M_PI * kBeepHz * t);
        const qint16 sample = qint16(qBound(-1.0, v, 1.0) * 32767.0);
        for (int ch = 0; ch < cfg.channels; ++ch)
            s[k * cfg.channels + ch] = sample;
    }
    return pcm;
}

double ndiMarkerAudioRmsFltp(const float* plane, int samples) {
    if (!plane || samples <= 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < samples; ++i)
        sum += double(plane[i]) * double(plane[i]);
    return std::sqrt(sum / double(samples));
}

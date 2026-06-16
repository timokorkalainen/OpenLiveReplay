#ifndef FRAMERATE_H
#define FRAMERATE_H

#include <QString>

#include <cstdint>
#include <vector>

// A recording frame rate as an exact rational num/den (e.g. 29.97 = 30000/1001).
// Centralizes the integer-fps arithmetic so the whole engine is fractional-rate ready.
struct FrameRate {
    int num = 30;
    int den = 1;

    bool isValid() const { return num > 0 && den > 0; }
    double toDouble() const { return den != 0 ? double(num) / double(den) : 0.0; }
    int roundedFps() const { return den > 0 ? (num + den / 2) / den : 0; }
    // File-timeline milliseconds of frame index f:  f * 1000 * den / num.
    int64_t msForFrame(int64_t f) const { return num > 0 ? (f * 1000 * int64_t(den)) / num : 0; }
    // Frame index reached at wall-clock ms:  ms * num / (1000 * den).
    int64_t frameForMs(int64_t ms) const {
        return den > 0 ? (ms * num) / (int64_t(1000) * den) : 0;
    }
    // Audio samples per frame at sampleRate (truncated):  sampleRate * den / num.
    int64_t samplesPerFrame(int sampleRate) const {
        return num > 0 ? (int64_t(sampleRate) * den) / num : 0;
    }
};

inline bool operator==(const FrameRate& a, const FrameRate& b) {
    return a.num == b.num && a.den == b.den;
}
inline bool operator!=(const FrameRate& a, const FrameRate& b) {
    return !(a == b);
}

struct FrameRatePreset {
    const char* label;
    FrameRate rate;
};

// The standard selectable rates (fractional broadcast + integer).
const std::vector<FrameRatePreset>& frameRatePresets();
// Parse "30", "29.97", or "30000/1001" into a FrameRate (anything invalid -> {30,1}).
FrameRate parseFrameRate(const QString& s);
// Nearest preset label for a rate, else "num/den".
QString frameRateLabel(const FrameRate& r);

#endif // FRAMERATE_H

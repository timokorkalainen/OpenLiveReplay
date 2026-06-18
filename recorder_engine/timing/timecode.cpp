#include "timecode.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace olr {
namespace Timecode {

namespace {

// Rounded integer frame count per second for a rational rate. 29.97 -> 30,
// 59.94 -> 60, 23.976 -> 24, integers map to themselves. Returns at least 1.
int roundedFps(const TimecodeRate& rate) {
    if (rate.num <= 0 || rate.den <= 0) {
        return 30;
    }
    // Round-half-up of num/den.
    const long long fps = (static_cast<long long>(rate.num) + rate.den / 2) / rate.den;
    return fps < 1 ? 1 : static_cast<int>(fps);
}

// Number of frame numbers dropped at the start of each affected minute:
// 2 for 29.97, 4 for 59.94, 0 otherwise.
int dropPerMinute(const TimecodeRate& rate) {
    if (!isDropFrameRate(rate)) {
        return 0;
    }
    return roundedFps(rate) / 15; // 30/15 = 2, 60/15 = 4
}

} // namespace

bool isDropFrameRate(const TimecodeRate& rate) {
    if (rate.num <= 0 || rate.den <= 0) {
        return false;
    }
    // Drop-frame is defined only for the NTSC 1001-denominator families whose
    // nominal (rounded) rate is 30 or 60.
    if (rate.den != 1001) {
        return false;
    }
    const int fps = roundedFps(rate);
    return fps == 30 || fps == 60;
}

std::string framesToTimecode(int64_t frame, const TimecodeRate& rate, bool dropFrame) {
    if (frame < 0) {
        frame = 0;
    }

    const int fpsInt = roundedFps(rate);
    const bool df = dropFrame && isDropFrameRate(rate);
    const int drop = df ? dropPerMinute(rate) : 0;

    if (drop > 0) {
        // Standard SMPTE 12M drop-frame renumbering. `frame` counts real frames;
        // we add back the dropped frame numbers so that HH:MM:SS:FF arithmetic
        // on a plain fpsInt base reproduces the on-air labels.
        const int64_t framesPerMin = static_cast<int64_t>(fpsInt) * 60 - drop;
        const int64_t framesPer10Min = static_cast<int64_t>(fpsInt) * 600 - drop * 9;
        const int64_t tenMins = frame / framesPer10Min;
        const int64_t rem = frame % framesPer10Min;
        if (rem < drop) {
            frame += drop * 9 * tenMins;
        } else {
            frame += drop * 9 * tenMins + drop * ((rem - drop) / framesPerMin);
        }
    }

    const int ff = static_cast<int>(frame % fpsInt);
    const int64_t totalSeconds = frame / fpsInt;
    const int ss = static_cast<int>(totalSeconds % 60);
    const int mm = static_cast<int>((totalSeconds / 60) % 60);
    const int hh = static_cast<int>((totalSeconds / 3600) % 24);

    const char frameSep = df ? ';' : ':';
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "%02d:%02d:%02d%c%02d", hh, mm, ss, frameSep, ff);
    return std::string(buf.data());
}

int64_t timecodeToFrames(const std::string& tc, const TimecodeRate& rate, bool dropFrame) {
    // Parse HH:MM:SS<sep>FF where <sep> is ':' or ';'. Be strict about the field
    // count but lenient about the separator characters. Field values are NOT
    // range-checked (a frame field >= fps, or an "impossible" dropped DF label,
    // is accepted and folded into the returned index) — this is a lenient
    // left-inverse, sufficient for the engine's frames->string->frames round-trip.
    int64_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    // Accumulate in 64-bit and saturate so a pathologically long numeric field
    // cannot overflow (signed overflow is UB; see tst_timecode oversized-field).
    int64_t value = 0;
    constexpr int64_t kValueCap = 1'000'000'000'000LL; // far beyond any real TC field
    bool sawDigit = false;
    for (size_t i = 0; i <= tc.size(); ++i) {
        const char c = (i < tc.size()) ? tc[i] : '\0';
        if (c >= '0' && c <= '9') {
            if (value < kValueCap) {
                value = value * 10 + (c - '0');
            }
            sawDigit = true;
        } else if (c == ':' || c == ';' || c == '\0') {
            if (idx < 4) {
                parts[idx] = value;
            }
            ++idx;
            value = 0;
            if (c == '\0') {
                break;
            }
        } else {
            // Unexpected character -> malformed.
            return 0;
        }
    }
    if (idx != 4 || !sawDigit) {
        return 0;
    }

    const int64_t hh = parts[0];
    const int64_t mm = parts[1];
    const int64_t ss = parts[2];
    const int64_t ff = parts[3];

    const int fpsInt = roundedFps(rate);
    const bool df = dropFrame && isDropFrameRate(rate);
    const int drop = df ? dropPerMinute(rate) : 0;

    int64_t frame = (hh * 3600 + mm * 60 + ss) * fpsInt + ff;
    if (drop > 0) {
        const int64_t totalMinutes = hh * 60 + mm;
        frame -= static_cast<int64_t>(drop) * (totalMinutes - totalMinutes / 10);
    }
    return frame;
}

} // namespace Timecode
} // namespace olr

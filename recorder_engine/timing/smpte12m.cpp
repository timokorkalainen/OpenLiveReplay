#include "smpte12m.h"
#include "timecodeevidence.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>

namespace {

bool checkedTimecodeMultiplyAdd(int64_t a, int64_t b, int64_t c, int64_t* result) {
    if (a < 0 || b < 0 || c < 0) return false;
    if (a != 0 && b > (std::numeric_limits<int64_t>::max() - c) / a) return false;
    *result = a * b + c;
    return true;
}

bool normalizedRateEquals(FrameRateQ rate, int32_t numerator, int32_t denominator) {
    if (!rate.valid()) return false;
    const int32_t divisor = std::gcd(rate.num, rate.den);
    return rate.num / divisor == numerator && rate.den / divisor == denominator;
}

} // namespace

bool validateTimecodeLabel(const Smpte12mTimecode& tc, FrameRateQ rate) {
    if (!tc.valid || tc.hours < 0 || tc.hours >= 24 || tc.minutes < 0 || tc.minutes >= 60 ||
        tc.seconds < 0 || tc.seconds >= 60 || tc.frames < 0)
        return false;
    if (!rate.valid()) return false;

    int64_t minimumRate = 0;
    int64_t maximumRate = 0;
    const int64_t denominator = rate.den;
    if (!checkedTimecodeMultiplyAdd(12, denominator, 0, &minimumRate) ||
        !checkedTimecodeMultiplyAdd(240, denominator, 0, &maximumRate) || rate.num < minimumRate ||
        rate.num > maximumRate)
        return false;

    int64_t roundedNumerator = 0;
    int64_t doubledDenominator = 0;
    if (!checkedTimecodeMultiplyAdd(2, rate.num, rate.den, &roundedNumerator) ||
        !checkedTimecodeMultiplyAdd(2, rate.den, 0, &doubledDenominator))
        return false;
    const int64_t nominalLabelRate = roundedNumerator / doubledDenominator;
    if (nominalLabelRate <= 0 || tc.frames >= nominalLabelRate) return false;

    if (!tc.dropFrame) return true;
    const bool ntsc30 = normalizedRateEquals(rate, 30000, 1001);
    const bool ntsc60 = normalizedRateEquals(rate, 60000, 1001);
    if (!ntsc30 && !ntsc60) return false;

    const int droppedLabels = ntsc60 ? 4 : 2;
    return tc.seconds != 0 || tc.minutes % 10 == 0 || tc.frames >= droppedLabels;
}

namespace Smpte12m {

namespace {

// Pack a 0..99 value into two BCD nibbles: units in the low nibble, tens in the
// next. SMPTE 12M splits these across non-adjacent bit fields, so the caller
// shifts them into place.
inline int bcdUnits(int value) {
    return value % 10;
}
inline int bcdTens(int value) {
    return (value / 10) % 10;
}

// True iff drop-frame renumbering is defined for this nominal rate (30/60).
inline bool dropApplies(const Smpte12mTimecode& tc, int nominalFps) {
    return tc.dropFrame && (nominalFps == 30 || nominalFps == 60);
}
inline int dropPerMinute(int nominalFps) {
    return nominalFps == 60 ? 4 : 2;
}

} // namespace

Smpte12mTimecode fromPackedWord(uint32_t word) {
    Smpte12mTimecode tc;
    // frames: units [0..3], tens [4..5]; drop-frame flag [6].
    tc.frames = static_cast<int>(word & 0x0Fu) + static_cast<int>((word >> 4) & 0x03u) * 10;
    tc.dropFrame = ((word >> 6) & 0x01u) != 0;
    // seconds: units [8..11], tens [12..14].
    tc.seconds =
        static_cast<int>((word >> 8) & 0x0Fu) + static_cast<int>((word >> 12) & 0x07u) * 10;
    // minutes: units [16..19], tens [20..22].
    tc.minutes =
        static_cast<int>((word >> 16) & 0x0Fu) + static_cast<int>((word >> 20) & 0x07u) * 10;
    // hours: units [24..27], tens [28..29].
    tc.hours = static_cast<int>((word >> 24) & 0x0Fu) + static_cast<int>((word >> 28) & 0x03u) * 10;
    // Range-sanity the decoded fields. A SMPTE 12M word from a mis-parsed SEI
    // payload (e.g. real pic_timing/ATC syntax read as a raw word) can yield
    // impossible fields; reject rather than emit a plausible-but-wrong TC that
    // would silently mis-align downstream. (frames<=99 since the units+tens BCD
    // nibbles top out there; callers compare against the real fps separately.)
    tc.valid = tc.hours < 24 && tc.minutes < 60 && tc.seconds < 60 && tc.frames < 60;
    return tc;
}

uint32_t toPackedWord(const Smpte12mTimecode& tc) {
    uint32_t word = 0;
    // frames: units [0..3], tens [4..5]; drop-frame flag [6].
    word |= static_cast<uint32_t>(bcdUnits(tc.frames));
    word |= static_cast<uint32_t>(bcdTens(tc.frames)) << 4;
    if (tc.dropFrame) {
        word |= 1u << 6;
    }
    // seconds: units [8..11], tens [12..14].
    word |= static_cast<uint32_t>(bcdUnits(tc.seconds)) << 8;
    word |= static_cast<uint32_t>(bcdTens(tc.seconds)) << 12;
    // minutes: units [16..19], tens [20..22].
    word |= static_cast<uint32_t>(bcdUnits(tc.minutes)) << 16;
    word |= static_cast<uint32_t>(bcdTens(tc.minutes)) << 20;
    // hours: units [24..27], tens [28..29].
    word |= static_cast<uint32_t>(bcdUnits(tc.hours)) << 24;
    word |= static_cast<uint32_t>(bcdTens(tc.hours)) << 28;
    return word;
}

char* format(const Smpte12mTimecode& tc, char out[12]) {
    if (!tc.valid) {
        out[0] = '\0';
        return out;
    }
    std::snprintf(out, 12, "%02d:%02d:%02d%c%02d", tc.hours, tc.minutes, tc.seconds,
                  tc.dropFrame ? ';' : ':', tc.frames);
    return out;
}

int64_t toFrameCount(const Smpte12mTimecode& tc, int nominalFps) {
    if (nominalFps <= 0) {
        return 0;
    }
    const int64_t fps = nominalFps;
    int64_t frame =
        ((static_cast<int64_t>(tc.hours) * 60 + tc.minutes) * 60 + tc.seconds) * fps + tc.frames;
    if (dropApplies(tc, nominalFps)) {
        const int dropPerMin = dropPerMinute(nominalFps);
        const int64_t totalMinutes = static_cast<int64_t>(tc.hours) * 60 + tc.minutes;
        frame -= static_cast<int64_t>(dropPerMin) * (totalMinutes - totalMinutes / 10);
    }
    // Wrap one full day to keep the index in [0, 24h*fps).
    const int64_t framesPerDay = 24 * 60 * 60 * fps;
    if (framesPerDay > 0) {
        frame %= framesPerDay;
        if (frame < 0) {
            frame += framesPerDay;
        }
    }
    return frame;
}

int labelRate(int rateNum, int rateDen) {
    if (rateNum <= 0 || rateDen <= 0) return 0;
    constexpr int64_t kMinSupportedFps = 12;
    constexpr int64_t kMaxSupportedFps = 240;
    const int64_t numerator = rateNum;
    const int64_t denominator = rateDen;
    if (numerator < kMinSupportedFps * denominator || numerator > kMaxSupportedFps * denominator)
        return 0;
    // 64-bit intermediate: rateNum/rateDen come from attacker-controlled SPS
    // timing_info (bounded to <=2e9 by parseSpsFrameRate), so 2*rateNum can exceed
    // INT_MAX — computing in int would be signed-overflow UB.
    return int((2LL * rateNum + rateDen) / (2LL * rateDen));
}

int64_t labelFrameCount(const Smpte12mTimecode& tc, int rateNum, int rateDen) {
    if (!validateTimecodeLabel(tc, FrameRateQ{rateNum, rateDen})) return -1;
    const int rate = labelRate(rateNum, rateDen);
    return rate > 0 ? toFrameCount(tc, rate) : -1;
}

int64_t labelFrameCountFrom100ns(int64_t timecode100ns, int rateNum, int rateDen) {
    constexpr int64_t kTicksPerSecond = 10'000'000;
    constexpr int64_t kTicksPerDay = 24LL * 60 * 60 * kTicksPerSecond;
    if (timecode100ns < 0 || timecode100ns >= kTicksPerDay) return -1;
    const int rate = labelRate(rateNum, rateDen);
    if (rate <= 0) return -1;
    using I128 = __int128;
    return static_cast<int64_t>((I128(timecode100ns) * rate + kTicksPerSecond / 2) /
                                kTicksPerSecond);
}

int64_t to100ns(const Smpte12mTimecode& tc, int nominalFps) {
    if (nominalFps <= 0) {
        return 0;
    }
    return toFrameCount(tc, nominalFps) * 10'000'000 / nominalFps;
}

Smpte12mTimecode from100ns(int64_t timecode100ns, int nominalFps) {
    Smpte12mTimecode tc;
    if (nominalFps <= 0 || timecode100ns < 0) {
        return tc; // valid=false
    }
    const int64_t fps = nominalFps;
    // Round to the nearest frame: for NTSC/film rates (24/30/60) 1e7/fps is not
    // an integer, so plain truncation loses up to a whole frame and accumulates
    // with elapsed time. Rounding makes to100ns->from100ns exact for all rates.
    int64_t frame = (timecode100ns * fps + 5'000'000) / 10'000'000;
    const int64_t framesPerDay = 24 * 60 * 60 * fps;
    if (framesPerDay > 0) {
        frame %= framesPerDay;
    }

    // 100 ns timestamps carry no drop-frame flag; decode as non-drop labels.
    tc.dropFrame = false;
    tc.frames = static_cast<int>(frame % fps);
    const int64_t totalSeconds = frame / fps;
    tc.seconds = static_cast<int>(totalSeconds % 60);
    tc.minutes = static_cast<int>((totalSeconds / 60) % 60);
    tc.hours = static_cast<int>((totalSeconds / 3600) % 24);
    tc.valid = true;
    return tc;
}

Smpte12mTimecode parseTimecodeString(const char* text) {
    Smpte12mTimecode tc; // valid=false
    if (!text) {
        return tc;
    }
    // Exactly "HH:MM:SS<sep>FF": two digits per field, ':' between the first
    // three, ':' (non-drop) or ';' (drop-frame) before the frames field.
    if (std::strlen(text) != 11) {
        return tc;
    }
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    static const int kDigitPositions[8] = {0, 1, 3, 4, 6, 7, 9, 10};
    for (const int i : kDigitPositions) {
        if (!isDigit(text[i])) {
            return tc;
        }
    }
    if (text[2] != ':' || text[5] != ':') {
        return tc;
    }
    const char frameSep = text[8];
    if (frameSep != ':' && frameSep != ';') {
        return tc;
    }
    auto twoDigit = [&](int i) { return (text[i] - '0') * 10 + (text[i + 1] - '0'); };
    tc.hours = twoDigit(0);
    tc.minutes = twoDigit(3);
    tc.seconds = twoDigit(6);
    tc.frames = twoDigit(9);
    tc.dropFrame = (frameSep == ';');
    // Range-sanity (mirrors fromPackedWord): reject impossible fields rather than
    // emit a plausible-but-wrong TC that would silently mis-align downstream.
    tc.valid = tc.hours < 24 && tc.minutes < 60 && tc.seconds < 60 && tc.frames < 60;
    return tc;
}

} // namespace Smpte12m

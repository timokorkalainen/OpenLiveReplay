#include "smpte12m.h"

#include <cstdio>

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

} // namespace Smpte12m

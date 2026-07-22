#ifndef TIMECODEEVIDENCE_H
#define TIMECODEEVIDENCE_H

#include "smpte12m.h"

#include <QMetaType>

#include <cstdint>
#include <optional>

struct FrameRateQ {
    int32_t num = 0;
    int32_t den = 1;
    bool valid() const { return num > 0 && den > 0; }
    friend bool operator==(FrameRateQ a, FrameRateQ b) { return a.num == b.num && a.den == b.den; }
};

enum class TimecodeProvenance : uint8_t {
    Ndi,
    H264PicTiming,
    HevcTimeCode,
    RegisteredAtc,
    RtmpMetadata
};

struct TimecodeEvidence {
    int64_t frameOfDay = -1;
    FrameRateQ labelRate;
    uint64_t sourceGeneration = 0;
    uint64_t timingGeneration = 0;
    TimecodeProvenance provenance = TimecodeProvenance::Ndi;
    bool dropFrame = false;
    bool discontinuity = false;
    int64_t arrivalSessionFrame = -1;
    FrameRateQ sessionRate;
    int64_t quantizationBoundUs = 0;
    int64_t driftBoundUs = 0;
    bool valid() const;
};

struct AlignmentOffset {
    enum class Kind : uint8_t { Exact, Bounded, Incomparable };
    Kind kind = Kind::Incomparable;
    int64_t offsetUs = 0;
    int64_t boundUs = 0;
    uint64_t sourceGenerationA = 0;
    uint64_t sourceGenerationB = 0;
    bool comparable() const { return kind != Kind::Incomparable; }
};

bool validateTimecodeLabel(const Smpte12mTimecode& timecode, FrameRateQ rate);
std::optional<FrameRateQ> canonicalFrameRate(double framesPerSecond);

Q_DECLARE_METATYPE(TimecodeEvidence)

#endif // TIMECODEEVIDENCE_H

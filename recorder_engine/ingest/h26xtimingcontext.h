#ifndef H26XTIMINGCONTEXT_H
#define H26XTIMINGCONTEXT_H

#include "pespacket.h"
#include "recorder_engine/timing/smpte12m.h"
#include "recorder_engine/timing/timecodeevidence.h"

#include <QByteArray>
#include <QList>

#include <cstdint>

enum class H26xTimingSyntaxStatus : uint8_t { Valid, Unsupported, Malformed };

struct H264TimingSyntax {
    H26xTimingSyntaxStatus status = H26xTimingSyntaxStatus::Malformed;
    FrameRateQ frameRate;
    uint32_t numUnitsInTick = 0;
    uint32_t timeScale = 0;
    bool fixedFrameRate = false;
    bool cpbDpbDelaysPresent = false;
    uint8_t cpbRemovalDelayLength = 0;
    uint8_t dpbOutputDelayLength = 0;
    uint8_t timeOffsetLength = 0;
    bool picStructPresent = false;
};

struct HevcTimingSyntax {
    H26xTimingSyntaxStatus status = H26xTimingSyntaxStatus::Unsupported;
    FrameRateQ frameRate;
    uint32_t numUnitsInTick = 0;
    uint32_t timeScale = 0;
    uint32_t numTicksPocDiffOne = 0;
    bool timingInfoPresent = false;
    bool pocProportionalToTiming = false;
    bool fieldSeq = false;
    bool frameFieldInfoPresent = false;
    uint8_t vpsId = 0;
    uint8_t referencedVpsId = 0;
    uint8_t spsId = 0;
};

class H26xTimingContext {
public:
    bool updateParameterSets(NativeVideoCodec codec, const QList<QByteArray>& vps,
                             const QList<QByteArray>& sps);
    NativeVideoCodec codec() const;
    FrameRateQ constantFrameRate() const;
    bool fixedFrameRate() const;
    uint64_t generation() const;
    const H264TimingSyntax* h264() const;
    const HevcTimingSyntax* hevc() const;

private:
    NativeVideoCodec m_codec = NativeVideoCodec::Unknown;
    QList<QByteArray> m_vps;
    QList<QByteArray> m_sps;
    uint64_t m_generation = 0;
    H264TimingSyntax m_h264;
    HevcTimingSyntax m_hevc;
};

namespace H26xTimingDetail {

enum class TimecodeParseStatus : uint8_t { Valid, NoTimestamp, Unsupported, Malformed };

struct TimecodeParseResult {
    TimecodeParseStatus status = TimecodeParseStatus::NoTimestamp;
    Smpte12mTimecode timecode;
    FrameRateQ labelRate;
    TimecodeProvenance provenance = TimecodeProvenance::H264PicTiming;
    bool discontinuity = false;
};

struct HevcTimeCodeContinuity {
    bool haveSeconds = false;
    bool haveMinutes = false;
    bool haveHours = false;
    uint32_t seconds = 0;
    uint32_t minutes = 0;
    uint32_t hours = 0;
};

// Decode an EBSP into an RBSP while validating the NAL escape rules. A prevention
// byte must precede 0x00..0x03, and raw 00 00 00/01/02 sequences are forbidden.
bool unescapeRbsp(const QByteArray& escaped, QByteArray& rbsp);

// Internal parser seam shared by the public Annex-B extractor. It returns a
// typed status so a short/reserved payload can never leak a partially filled label.
TimecodeParseResult parseH264PicTiming(const QByteArray& payload, const H264TimingSyntax& syntax);
TimecodeParseResult parseHevcTimeCode(const QByteArray& payload, const HevcTimingSyntax& syntax,
                                      const HevcTimeCodeContinuity* previous = nullptr,
                                      HevcTimeCodeContinuity* next = nullptr,
                                      int expectedClockCount = -1);

} // namespace H26xTimingDetail

#endif // H26XTIMINGCONTEXT_H

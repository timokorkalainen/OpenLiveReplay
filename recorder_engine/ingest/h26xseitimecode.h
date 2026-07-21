#ifndef H26XSEITIMECODE_H
#define H26XSEITIMECODE_H

#include "h26xtimingcontext.h"
#include "pespacket.h" // NativeVideoCodec
#include "recorder_engine/timing/smpte12m.h"

#include <QByteArray>

#include <array>

struct H26xSeiTimecodeResult {
    H26xTimingDetail::TimecodeParseStatus status =
        H26xTimingDetail::TimecodeParseStatus::NoTimestamp;
    Smpte12mTimecode timecode;
    FrameRateQ labelRate;
    TimecodeProvenance provenance = TimecodeProvenance::H264PicTiming;
    bool discontinuity = false;
};

// Caller-owned presentation identity for output-order validation. The
// presentation key must already be unwrapped into a monotonic clock domain;
// source/timing generation, domain, and epoch prevent retained predecessors
// from crossing reopen, parameter-set, clock-domain, or wrap boundaries.
struct H26xSeiOutputOrderKey {
    uint64_t sourceGeneration = 0;
    uint64_t timingGeneration = 0;
    uint64_t domain = 0;
    uint64_t epoch = 0;
    int64_t presentationKey = 0;
};

class H26xSeiTimecodeState {
public:
    void reset();

private:
    friend H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray&, NativeVideoCodec,
                                                              const H26xTimingContext&,
                                                              H26xSeiTimecodeState&);
    friend H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray&, NativeVideoCodec,
                                                              const H26xTimingContext&,
                                                              H26xSeiTimecodeState&,
                                                              const H26xSeiOutputOrderKey&);

    struct OutputEntry {
        H26xTimingDetail::HevcTimeCodeOutput first;
        H26xTimingDetail::HevcTimeCodeOutput last;
        H26xSeiTimecodeResult result;
    };
    struct StoredOutputEntry {
        int64_t presentationKey = 0;
        OutputEntry output;
    };
    static constexpr std::size_t kMaxRetainedOutputEntries = 64;
    using OutputWindow = std::array<StoredOutputEntry, kMaxRetainedOutputEntries>;
    static_assert(sizeof(OutputWindow) == sizeof(StoredOutputEntry) * kMaxRetainedOutputEntries,
                  "HEVC output-order retention must remain fixed-capacity and inline");

    bool m_contextBound = false;
    uint64_t m_contextIdentity = 0;
    uint64_t m_contextGeneration = 0;
    NativeVideoCodec m_codec = NativeVideoCodec::Unknown;
    H26xTimingDetail::HevcTimeCodeContinuity m_hevcContinuity;
    bool m_outputOrderBound = false;
    uint64_t m_sourceGeneration = 0;
    uint64_t m_timingGeneration = 0;
    uint64_t m_outputDomain = 0;
    uint64_t m_outputEpoch = 0;
    bool m_haveEvictedOutput = false;
    int64_t m_evictedThrough = 0;
    OutputWindow m_outputEntries{};
    std::size_t m_outputEntryCount = 0;
};

// Scan an Annex-B access unit for an embedded SMPTE 12M timecode SEI. H.264
// pic_timing and HEVC time_code require the overload with an active timing
// context and are parsed as their codec-specific standard bit syntax. The
// context-free overload remains for callers that only need a safe no-evidence
// result when no parameter-set timing context is available.
// Returns {valid=false} when no timecode SEI is present (the common case).
// Pure: no Qt event loop, no FFmpeg. Bounds-checked at every step — a garbled or
// truncated SEI must return {valid=false}, never read out of bounds or crash.
Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec);
Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec,
                                        const H26xTimingContext& context);
H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray& annexB, NativeVideoCodec codec,
                                                   const H26xTimingContext& context);
H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray& annexB, NativeVideoCodec codec,
                                                   const H26xTimingContext& context,
                                                   H26xSeiTimecodeState& state);
H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray& annexB, NativeVideoCodec codec,
                                                   const H26xTimingContext& context,
                                                   H26xSeiTimecodeState& state,
                                                   const H26xSeiOutputOrderKey& outputOrder);

#endif // H26XSEITIMECODE_H

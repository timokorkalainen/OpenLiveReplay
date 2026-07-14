#ifndef H26XSEITIMECODE_H
#define H26XSEITIMECODE_H

#include "h26xtimingcontext.h"
#include "pespacket.h" // NativeVideoCodec
#include "recorder_engine/timing/smpte12m.h"

#include <QByteArray>

struct H26xSeiTimecodeResult {
    Smpte12mTimecode timecode;
    FrameRateQ labelRate;
    TimecodeProvenance provenance = TimecodeProvenance::H264PicTiming;
    bool discontinuity = false;
};

class H26xSeiTimecodeState {
public:
    void reset();

private:
    friend H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray&, NativeVideoCodec,
                                                              const H26xTimingContext&,
                                                              H26xSeiTimecodeState&);

    bool m_contextBound = false;
    uint64_t m_contextGeneration = 0;
    NativeVideoCodec m_codec = NativeVideoCodec::Unknown;
    H26xTimingDetail::HevcTimeCodeContinuity m_hevcContinuity;
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

#endif // H26XSEITIMECODE_H

#ifndef H26XSEITIMECODE_H
#define H26XSEITIMECODE_H

#include "h26xtimingcontext.h"
#include "pespacket.h" // NativeVideoCodec
#include "recorder_engine/timing/smpte12m.h"

#include <QByteArray>

// Scan an Annex-B access unit for an embedded SMPTE 12M timecode SEI. H.264
// pic_timing requires the overload with an active SPS timing context and is
// parsed as standard clock_timestamp syntax. The context-free overload is
// retained for the HEVC compatibility path until its standard parser lands.
// Returns {valid=false} when no timecode SEI is present (the common case).
// Pure: no Qt event loop, no FFmpeg. Bounds-checked at every step — a garbled or
// truncated SEI must return {valid=false}, never read out of bounds or crash.
Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec);
Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec,
                                        const H26xTimingContext& context);

#endif // H26XSEITIMECODE_H

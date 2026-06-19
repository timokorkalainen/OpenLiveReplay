#ifndef H26XSEITIMECODE_H
#define H26XSEITIMECODE_H

#include "pespacket.h" // NativeVideoCodec
#include "recorder_engine/timing/smpte12m.h"

#include <QByteArray>

// Scan an Annex-B access unit for an embedded SMPTE 12M timecode SEI:
//   - H.264: pic_timing SEI (payloadType 1) clock_timestamp, OR registered ATC
//            user_data (payloadType 4) carrying SMPTE 12M.
//   - HEVC : time_code SEI (payloadType 136) OR registered ATC user_data.
// Returns {valid=false} when no timecode SEI is present (the common case).
// Pure: no Qt event loop, no FFmpeg. Bounds-checked at every step — a garbled or
// truncated SEI must return {valid=false}, never read out of bounds or crash.
Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec);

#endif // H26XSEITIMECODE_H

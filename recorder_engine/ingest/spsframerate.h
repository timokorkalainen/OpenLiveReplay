#ifndef OLR_SPSFRAMERATE_H
#define OLR_SPSFRAMERATE_H

#include "recorder_engine/ingest/pespacket.h"

#include <QByteArray>

#include <cstdint>

// The true source frame rate recovered from a codec bitstream, as an exact
// rational (num/den frames per second). {0,0} means not recoverable.
struct SpsFrameRate {
    int32_t num = 0;
    int32_t den = 0;
    bool valid() const { return num > 0 && den > 0; }
};

// Recover a constant rate from H.264 VUI timing_info. An Annex-B prefix is
// accepted but not required. Returns {0,0} unless fixed_frame_rate_flag is set
// and the exact reduced rate is in [12,240] fps. High-profile scaling lists are
// walked rather than treated as timing failures. HEVC remains unsupported here.
SpsFrameRate parseSpsFrameRate(NativeVideoCodec codec, const QByteArray& nal);

#endif // OLR_SPSFRAMERATE_H

#ifndef OLR_SPSFRAMERATE_H
#define OLR_SPSFRAMERATE_H

#include "recorder_engine/ingest/pespacket.h"

#include <QByteArray>
#include <cstdint>

// The true source frame rate recovered from a codec bitstream, as an exact
// rational (num/den frames per second). {0,0} == "not recoverable" — the ONLY
// honest answer when the stream carries no timing_info; callers must degrade to
// Incomparable, never guess a rate.
struct SpsFrameRate {
    int32_t num = 0;
    int32_t den = 0;
    bool valid() const { return num > 0 && den > 0; }
};

// Recover the frame rate from an H.264 SPS NAL's VUI timing_info
// (fps = time_scale / (2·num_units_in_tick)). The NAL header byte is included;
// no start code or length prefix. Returns {0,0} unless a *plausible* rate
// (roughly 12..240 fps) is present — a malformed/absent/implausible VUI, a
// seq_scaling_matrix, an overrun, or any non-H.264 codec all yield {0,0} so a
// parser fault can only ever downgrade to Incomparable. Pure; Qt-free logic on a
// QByteArray. HEVC is not parsed here (returns {0,0}); those sources recover
// their rate by another route or stay Incomparable.
SpsFrameRate parseSpsFrameRate(NativeVideoCodec codec, const QByteArray& nal);

#endif // OLR_SPSFRAMERATE_H

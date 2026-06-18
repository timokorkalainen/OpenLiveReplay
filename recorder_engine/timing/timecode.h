#ifndef TIMECODE_H
#define TIMECODE_H

#include <cstdint>
#include <string>

namespace olr {

// Minimal rational frame rate for the timecode math.
//
// The project already has playback/framerate.h::FrameRate, but that type pulls
// in <QtGlobal> (qint64 / qRound / qMax) and lives in the playback/UI layer.
// The recorder_engine/timing/ unit is deliberately Qt-free and dependency-light
// (mirror driftestimator/sourceclock, which use plain <cstdint>) so it can be
// unit-tested and reused without dragging in Qt. Hence a local POD rate type.
// Callers in the Qt layer can trivially adapt: {rate.numerator, rate.denominator}.
struct TimecodeRate {
    int num = 30;
    int den = 1;
};

// SMPTE 12M timecode conversion between an absolute frame index and an
// HH:MM:SS:FF (non-drop) / HH:MM:SS;FF (drop-frame) string.
//
// Drop-frame counting (SMPTE 12M):
//   * 29.97 (30000/1001) drops frame numbers 0 and 1 at the start of every
//     minute EXCEPT minutes divisible by 10 (2 frames * 54 minutes/hour).
//   * 59.94 (60000/1001) drops 0,1,2,3 likewise (4 frames * 54 minutes/hour).
//   * Integer rates (24/25/30/50/60, and 23.976 treated as a 24-count) are
//     non-drop.
//
// DF contract: drop-frame is only defined for 29.97 and 59.94. If `dropFrame`
// is requested on any other rate, the conversion SILENTLY FALLS BACK to NDF
// (colon separator, no frame dropping). Use isDropFrameRate() to detect whether
// a rate is genuinely drop-frame capable before relying on DF semantics.
namespace Timecode {

// True iff `rate` is a valid drop-frame rate (29.97 or 59.94 family), i.e. a
// 1001-denominator rate whose rounded fps is 30 or 60.
bool isDropFrameRate(const TimecodeRate& rate);

// Convert an absolute frame index to a SMPTE 12M timecode string.
// `dropFrame` is honoured only when isDropFrameRate(rate) is true; otherwise the
// result is NDF. Negative `frame` is clamped to 0.
std::string framesToTimecode(int64_t frame, const TimecodeRate& rate, bool dropFrame);

// Inverse of framesToTimecode: parse "HH:MM:SS:FF" or "HH:MM:SS;FF" back to an
// absolute frame index. The separator before the frame field is accepted as
// either ':' or ';' regardless of `dropFrame`; `dropFrame` (gated by
// isDropFrameRate) selects whether drop-frame renumbering is undone. Returns 0
// for malformed input.
int64_t timecodeToFrames(const std::string& tc, const TimecodeRate& rate, bool dropFrame);

} // namespace Timecode

} // namespace olr

#endif // TIMECODE_H

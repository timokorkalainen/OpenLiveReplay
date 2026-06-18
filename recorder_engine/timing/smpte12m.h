#ifndef SMPTE12M_H
#define SMPTE12M_H
#include <cstdint>

// A decoded SMPTE 12M timecode. ff = frame within the second; dropFrame is the
// 29.97/59.94 NTSC drop-frame flag. valid=false means "no timecode".
struct Smpte12mTimecode {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
    bool dropFrame = false;
    bool valid = false;
};

// SMPTE 12M helpers. Pure (no Qt/FFmpeg). The 32-bit word is the BCD-packed
// representation carried in H.264/HEVC pic_timing/ATC SEI and ANC; toFrameCount
// converts a wall-clock TC to an absolute frame number at a given integer fps.
namespace Smpte12m {
// Decode the 32-bit BCD timecode word (SMPTE 12M / ATC layout) -> fields.
Smpte12mTimecode fromPackedWord(uint32_t word);
// Encode fields -> the 32-bit BCD timecode word (inverse of fromPackedWord).
uint32_t toPackedWord(const Smpte12mTimecode& tc);
// Render "HH:MM:SS:FF" (drop-frame uses ';' before the frames field).
// Empty string when !valid.
//   e.g. {10,11,12,13,false,true} -> "10:11:12:13"
// drop-frame example {1,0,0,2,true,true} -> "01:00:00;02"
char* /*caller-owned, 12 bytes*/ format(const Smpte12mTimecode& tc, char out[12]);
// Absolute frame index since 00:00:00:00 at an integer fps (non-drop arithmetic;
// drop-frame skip is applied when tc.dropFrame and nominalFps is 30 or 60).
int64_t toFrameCount(const Smpte12mTimecode& tc, int nominalFps);
// 100 ns timestamp of this TC since 00:00:00:00 (= toFrameCount * 1e7 / fps).
int64_t to100ns(const Smpte12mTimecode& tc, int nominalFps);
// Decode a 100 ns timecode (NDI delivers TC as 100 ns since midnight) -> fields.
Smpte12mTimecode from100ns(int64_t timecode100ns, int nominalFps);
// Parse "HH:MM:SS:FF" (or ';' before the frames field for drop-frame) -> fields.
// Pure and Qt-free (takes a NUL-terminated C string; Qt callers pass
// QString::toUtf8().constData()). Strictly best-effort: any malformed, null,
// out-of-range, or wrong-shape input returns {valid=false} (never crashes).
Smpte12mTimecode parseTimecodeString(const char* text);
} // namespace Smpte12m
#endif // SMPTE12M_H

// libFuzzer harness for the SMPTE 12M SEI timecode extractor. It scans an
// untrusted Annex-B access unit for pic_timing / time_code / registered-ATC SEI;
// its contract is "bounds-checked at every step — a garbled or truncated SEI must
// return {valid=false}, never read out of bounds or crash". This fuzzes that.
// The first input byte selects the codec; the remainder is the Annex-B buffer.
#include "recorder_engine/ingest/h26xseitimecode.h"

#include <QByteArray>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const NativeVideoCodec codec = (data[0] & 1) ? NativeVideoCodec::Hevc : NativeVideoCodec::H264;
    QByteArray annexB(reinterpret_cast<const char*>(data + 1), int(size - 1));

    (void) extractH26xSeiTimecode(annexB, codec);
    return 0;
}

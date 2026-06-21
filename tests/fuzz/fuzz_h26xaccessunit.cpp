// libFuzzer harness for the H.264/HEVC access-unit splitter. The PES payload it
// consumes comes straight from an untrusted transport, so NAL start-code
// scanning, parameter-set inspection (and its accumulation cap), and the
// downstream SEI-timecode scan must be crash-free on arbitrary bytes. The first
// input byte selects the codec; the remainder is the PES payload.
#include "recorder_engine/ingest/h26xaccessunit.h"
#include "recorder_engine/ingest/h26xseitimecode.h"

#include <QByteArray>
#include <QList>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const NativeVideoCodec codec = (data[0] & 1) ? NativeVideoCodec::Hevc : NativeVideoCodec::H264;
    QByteArray payload(reinterpret_cast<const char*>(data + 1), int(size - 1));

    H26xAccessUnitSplitter splitter(codec);
    const QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 90000, 90000);
    (void) splitter.parameterSets();

    // Co-fuzz the realistic PES -> split -> SEI-timecode chain on each emitted AU.
    for (const CompressedAccessUnit& unit : units)
        (void) extractH26xSeiTimecode(unit.annexB, codec);
    return 0;
}

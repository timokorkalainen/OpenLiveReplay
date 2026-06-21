// libFuzzer harness for the H.264/HEVC access-unit splitter. The PES payload it
// consumes comes straight from an untrusted transport, so NAL start-code
// scanning and parameter-set inspection must be crash-free on arbitrary bytes.
// The first input byte selects the codec; the remainder is the PES payload.
#include "recorder_engine/ingest/h26xaccessunit.h"

#include <QByteArray>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const NativeVideoCodec codec = (data[0] & 1) ? NativeVideoCodec::Hevc : NativeVideoCodec::H264;
    QByteArray payload(reinterpret_cast<const char*>(data + 1), int(size - 1));

    H26xAccessUnitSplitter splitter(codec);
    splitter.pushPesPayload(payload, /*pts90k=*/90000, /*dts90k=*/90000);
    (void) splitter.parameterSets();
    return 0;
}

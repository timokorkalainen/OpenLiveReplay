// libFuzzer harness for the MPEG-TS parser (the SRT/UDP ingest entry point).
// Feeds the fuzzer's bytes as a sequence of 188-byte transport packets through
// the full PAT -> PMT -> PES pipeline, exercising section parsing, continuity
// handling, and the PES reassembly cap. The parser must never read out of
// bounds, assert, or grow memory without limit on any input.
#include "recorder_engine/ingest/mpegtsparser.h"

#include <QByteArray>
#include <QList>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    MpegTsParser parser;
    QList<PesPacket> completed;
    for (size_t off = 0; off + 188 <= size; off += 188) {
        QByteArray packet(reinterpret_cast<const char*>(data + off), 188);
        MpegTsParser::TsPacketInfo info;
        parser.pushTsPacket(packet, &completed, &info);
        // Bound the harness-side accumulation (a stream of tiny complete PESs
        // would otherwise grow this list, not the code under test).
        if (completed.size() > 1024) completed.clear();
    }
    return 0;
}

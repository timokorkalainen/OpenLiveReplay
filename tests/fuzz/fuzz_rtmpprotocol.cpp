// libFuzzer harness for the RTMP ingest parsers. Drives the untrusted-input
// surfaces on one buffer: the stateful chunk reassembler (caps enforced), the
// FLV video/sequence-header parsers, and the AMF0 value reader. None may read
// out of bounds, assert, or allocate without limit on arbitrary input.
#include "recorder_engine/ingest/rtmpprotocol.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const QByteArray bytes(reinterpret_cast<const char*>(data), int(size));

    // Primary surface: the chunk reassembler (stateful; bounded by its caps).
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;
    parser.push(bytes, &messages, &error);

    // FLV tag / sequence-header parsers — each bounds-checks the same bytes.
    RtmpVideoPacket videoPacket;
    QString e;
    RtmpFlv::parseVideoPacket(bytes, &videoPacket, &e);
    RtmpAvcConfig avc;
    RtmpFlv::parseAvcSequenceHeader(bytes, &avc, &e);
    RtmpHevcConfig hevc;
    RtmpFlv::parseHevcSequenceHeader(bytes, &hevc, &e);
    RtmpAacConfig aac;
    RtmpFlv::parseAacSequenceHeader(bytes, &aac, &e);
    (void) RtmpFlv::avcPayloadToAnnexB(bytes, /*nalLengthSize=*/4);

    // AMF0 offset-based reader.
    int offset = 0;
    RtmpAmf0::skipValue(bytes, &offset);
    return 0;
}

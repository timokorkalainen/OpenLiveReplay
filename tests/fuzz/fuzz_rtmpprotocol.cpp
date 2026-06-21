// libFuzzer harness for the RTMP ingest parsers. Drives the untrusted-input
// surfaces on one buffer: the stateful chunk reassembler (caps enforced), the
// FLV video/sequence-header parsers, the AVCC->Annex-B conversions, and the AMF0
// readers actually used on the control path. None may read out of bounds,
// assert, or allocate without limit on arbitrary input.
#include "recorder_engine/ingest/rtmpprotocol.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const QByteArray bytes(reinterpret_cast<const char*>(data), int(size));

    // Chunk reassembler, fed in two push() calls so the cross-call buffered
    // partial-chunk reassembly (m_buffer retains an incomplete chunk between
    // calls) is exercised, not just a single complete buffer.
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;
    const int half = bytes.size() / 2;
    parser.push(bytes.left(half), &messages, &error);
    parser.push(bytes.mid(half), &messages, &error);

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

    // AVCC/length-prefixed -> Annex-B with an attacker-derived NAL length size
    // (production reads it from the sequence header, so it is not always 4).
    const int nalLengthSize = size ? ((data[size - 1] & 0x03) + 1) : 4;
    (void) RtmpFlv::avcPayloadToAnnexB(bytes, nalLengthSize);
    (void) RtmpFlv::lengthPrefixedPayloadToAnnexB(bytes, nalLengthSize);

    // AMF0 readers used on the untrusted control path (remote command name +
    // transaction id in onStatus / _result), not just skipValue.
    int offset = 0;
    QString s;
    RtmpAmf0::readString(bytes, &offset, &s);
    offset = 0;
    double number = 0;
    RtmpAmf0::readNumber(bytes, &offset, &number);
    offset = 0;
    RtmpAmf0::skipValue(bytes, &offset);
    return 0;
}

#include "h26xseitimecode.h"

#include <QList>

#include <limits>

namespace {

int startCodeSizeAt(const QByteArray& bytes, int offset) {
    if (offset + 3 <= bytes.size() && bytes[offset] == char(0) && bytes[offset + 1] == char(0) &&
        bytes[offset + 2] == char(1)) {
        return 3;
    }
    if (offset + 4 <= bytes.size() && bytes[offset] == char(0) && bytes[offset + 1] == char(0) &&
        bytes[offset + 2] == char(0) && bytes[offset + 3] == char(1)) {
        return 4;
    }
    return 0;
}

QList<int> findStartCodes(const QByteArray& bytes) {
    if (bytes.size() > std::numeric_limits<int>::max()) return {};
    const int byteCount = int(bytes.size());
    QList<int> offsets;
    for (int i = 0; i + 3 <= byteCount;) {
        const int size = startCodeSizeAt(bytes, i);
        if (size > 0) {
            offsets.append(i);
            i += size;
        } else {
            ++i;
        }
    }
    return offsets;
}

QList<QByteArray> splitAnnexBNals(const QByteArray& bytes) {
    if (bytes.size() > std::numeric_limits<int>::max()) return {};
    const int byteCount = int(bytes.size());
    QList<QByteArray> nals;
    const QList<int> starts = findStartCodes(bytes);
    for (int i = 0; i < starts.size(); ++i) {
        const int start = starts[i];
        const int prefixSize = startCodeSizeAt(bytes, start);
        const int payloadOffset = start + prefixSize;
        const int end = i + 1 < starts.size() ? starts[i + 1] : byteCount;
        if (prefixSize != 0 && end > payloadOffset)
            nals.append(bytes.mid(payloadOffset, end - payloadOffset));
    }
    return nals;
}

QByteArray rbspFromPayload(const QByteArray& bytes) {
    QByteArray rbsp;
    rbsp.reserve(bytes.size());
    int zeroCount = 0;
    for (char byte : bytes) {
        const uchar value = uchar(byte);
        if (zeroCount >= 2 && value == 0x03) {
            zeroCount = 0;
            continue;
        }
        rbsp.append(byte);
        zeroCount = value == 0 ? zeroCount + 1 : 0;
    }
    return rbsp;
}

bool isSeiNal(const QByteArray& nal, NativeVideoCodec codec) {
    if (codec == NativeVideoCodec::H264) return !nal.isEmpty() && (uchar(nal[0]) & 0x1f) == 6;
    if (codec == NativeVideoCodec::Hevc) {
        if (nal.size() < 2) return false;
        const int type = (uchar(nal[0]) >> 1) & 0x3f;
        return type == 39 || type == 40;
    }
    return false;
}

bool readSeiVarValue(const QByteArray& rbsp, int& pos, int64_t& value) {
    int64_t total = 0;
    while (true) {
        if (pos >= rbsp.size()) return false;
        const int byte = uchar(rbsp[pos++]);
        total += byte;
        if (byte != 0xff) break;
    }
    value = total;
    return true;
}

// Temporary HEVC compatibility for Task 2 only. H.264 never enters this path;
// Task 3 replaces it with standard HEVC time_code and registered-ATC parsing.
Smpte12mTimecode decodeLegacyHevcPayload(const QByteArray& rbsp, int payloadStart,
                                         int payloadSize) {
    if (payloadSize < 4 || payloadStart < 0 || payloadStart > rbsp.size() - 4) return {};
    const uint32_t word = (uint32_t(uchar(rbsp[payloadStart])) << 24) |
                          (uint32_t(uchar(rbsp[payloadStart + 1])) << 16) |
                          (uint32_t(uchar(rbsp[payloadStart + 2])) << 8) |
                          uint32_t(uchar(rbsp[payloadStart + 3]));
    return Smpte12m::fromPackedWord(word);
}

Smpte12mTimecode extractFromSeiRbsp(const QByteArray& rbsp, NativeVideoCodec codec,
                                    const H26xTimingContext* context) {
    int pos = 0;
    while (pos < rbsp.size()) {
        if (uchar(rbsp[pos]) == 0x80) break;

        int64_t payloadType = 0;
        int64_t payloadSize = 0;
        if (!readSeiVarValue(rbsp, pos, payloadType) || !readSeiVarValue(rbsp, pos, payloadSize) ||
            payloadSize > rbsp.size() - pos) {
            break;
        }

        if (codec == NativeVideoCodec::H264 && payloadType == 1 && context != nullptr &&
            context->codec() == NativeVideoCodec::H264 && context->h264() != nullptr) {
            const auto parsed = H26xTimingDetail::parseH264PicTiming(
                rbsp.mid(pos, int(payloadSize)), *context->h264());
            if (parsed.status == H26xTimingDetail::TimecodeParseStatus::Valid &&
                validateTimecodeLabel(parsed.timecode, context->constantFrameRate())) {
                return parsed.timecode;
            }
        } else if (codec == NativeVideoCodec::Hevc && (payloadType == 136 || payloadType == 4)) {
            const Smpte12mTimecode timecode = decodeLegacyHevcPayload(rbsp, pos, int(payloadSize));
            if (timecode.valid) return timecode;
        }
        pos += int(payloadSize);
    }
    return {};
}

Smpte12mTimecode extract(const QByteArray& annexB, NativeVideoCodec codec,
                         const H26xTimingContext* context) {
    if (annexB.isEmpty() || codec == NativeVideoCodec::Unknown) return {};
    for (const QByteArray& nal : splitAnnexBNals(annexB)) {
        if (!isSeiNal(nal, codec)) continue;
        const int headerBytes = codec == NativeVideoCodec::H264 ? 1 : 2;
        if (nal.size() <= headerBytes) continue;
        const Smpte12mTimecode timecode =
            extractFromSeiRbsp(rbspFromPayload(nal.mid(headerBytes)), codec, context);
        if (timecode.valid) return timecode;
    }
    return {};
}

} // namespace

Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec) {
    return extract(annexB, codec, nullptr);
}

Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec,
                                        const H26xTimingContext& context) {
    return extract(annexB, codec, &context);
}

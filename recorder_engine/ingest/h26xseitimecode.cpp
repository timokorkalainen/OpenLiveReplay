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
        int end = i + 1 < starts.size() ? starts[i + 1] : byteCount;
        while (end > payloadOffset && bytes[end - 1] == char(0))
            --end; // Annex-B trailing_zero_8bits are not part of nal_unit().
        if (prefixSize != 0 && end > payloadOffset)
            nals.append(bytes.mid(payloadOffset, end - payloadOffset));
    }
    return nals;
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

H26xTimingDetail::TimecodeParseResult extractFromSeiRbsp(const QByteArray& rbsp,
                                                         NativeVideoCodec codec,
                                                         const H26xTimingContext* context) {
    using H26xTimingDetail::TimecodeParseResult;
    using H26xTimingDetail::TimecodeParseStatus;

    TimecodeParseResult result;
    bool sawMalformed = false;
    bool sawUnsupported = false;
    bool sawTrailingBits = false;
    int pos = 0;
    while (pos < rbsp.size()) {
        if (pos == rbsp.size() - 1 && uchar(rbsp[pos]) == 0x80) {
            sawTrailingBits = true;
            ++pos;
            break;
        }

        int64_t payloadType = 0;
        int64_t payloadSize = 0;
        if (!readSeiVarValue(rbsp, pos, payloadType) || !readSeiVarValue(rbsp, pos, payloadSize) ||
            payloadSize > rbsp.size() - pos) {
            result.status = TimecodeParseStatus::Malformed;
            result.timecode = {};
            return result;
        }

        if (codec == NativeVideoCodec::H264 && payloadType == 1 && context != nullptr &&
            context->codec() == NativeVideoCodec::H264 && context->h264() != nullptr) {
            const auto parsed = H26xTimingDetail::parseH264PicTiming(
                rbsp.mid(pos, int(payloadSize)), *context->h264());
            const bool validCandidate =
                parsed.status == TimecodeParseStatus::Valid &&
                validateTimecodeLabel(parsed.timecode, context->constantFrameRate());
            if (parsed.status == TimecodeParseStatus::Malformed ||
                (parsed.status == TimecodeParseStatus::Valid && !validCandidate)) {
                sawMalformed = true;
            }
            if (parsed.status == TimecodeParseStatus::Unsupported) sawUnsupported = true;
            if (validCandidate && !result.timecode.valid) result.timecode = parsed.timecode;
        } else if (codec == NativeVideoCodec::H264 && payloadType == 1) {
            sawUnsupported = true;
        } else if (codec == NativeVideoCodec::Hevc && (payloadType == 136 || payloadType == 4)) {
            const Smpte12mTimecode timecode = decodeLegacyHevcPayload(rbsp, pos, int(payloadSize));
            if (timecode.valid && !result.timecode.valid) result.timecode = timecode;
        }
        pos += int(payloadSize);
    }
    if (sawMalformed || !sawTrailingBits || pos != rbsp.size()) {
        result.status = TimecodeParseStatus::Malformed;
        result.timecode = {};
    } else if (sawUnsupported) {
        result.status = TimecodeParseStatus::Unsupported;
        result.timecode = {};
    } else if (result.timecode.valid) {
        result.status = TimecodeParseStatus::Valid;
    }
    return result;
}

Smpte12mTimecode extract(const QByteArray& annexB, NativeVideoCodec codec,
                         const H26xTimingContext* context) {
    if (annexB.isEmpty() || codec == NativeVideoCodec::Unknown) return {};
    using H26xTimingDetail::TimecodeParseStatus;

    Smpte12mTimecode firstUsableTimestamp;
    bool sawUnsupported = false;
    bool sawMalformed = false;
    for (const QByteArray& nal : splitAnnexBNals(annexB)) {
        if (!isSeiNal(nal, codec)) continue;
        if (codec == NativeVideoCodec::H264 && (uchar(nal[0]) & 0xe0u) != 0) {
            sawMalformed = true;
            continue;
        }
        const int headerBytes = codec == NativeVideoCodec::H264 ? 1 : 2;
        if (nal.size() <= headerBytes) {
            sawMalformed = true;
            continue;
        }
        QByteArray rbsp;
        if (!H26xTimingDetail::unescapeRbsp(nal.mid(headerBytes), rbsp)) {
            sawMalformed = true;
            continue;
        }
        const auto parsed = extractFromSeiRbsp(rbsp, codec, context);
        if (parsed.status == TimecodeParseStatus::Malformed) {
            sawMalformed = true;
        } else if (parsed.status == TimecodeParseStatus::Unsupported) {
            sawUnsupported = true;
        } else if (parsed.status == TimecodeParseStatus::Valid && !firstUsableTimestamp.valid) {
            firstUsableTimestamp = parsed.timecode;
        }
    }
    if (sawMalformed || sawUnsupported) return {};
    return firstUsableTimestamp;
}

} // namespace

Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec) {
    return extract(annexB, codec, nullptr);
}

Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec,
                                        const H26xTimingContext& context) {
    return extract(annexB, codec, &context);
}

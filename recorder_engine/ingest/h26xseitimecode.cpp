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

H26xTimingDetail::TimecodeParseResult parseRegisteredT35(const QByteArray& payload) {
    using H26xTimingDetail::TimecodeParseResult;
    using H26xTimingDetail::TimecodeParseStatus;
    TimecodeParseResult result;
    result.provenance = TimecodeProvenance::RegisteredAtc;
    if (payload.isEmpty()) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }
    int pos = 0;
    const uint8_t countryCode = uint8_t(payload[pos++]);
    if (countryCode == 0xff) {
        if (pos >= payload.size()) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        ++pos; // itu_t_t35_country_code_extension_byte
    }

    // T.35 assigns the remaining syntax to the registered provider. There is
    // no published ATC profile selected by this project, so no provider-owned
    // body is decoded. For the well-known US ATSC namespace, validate the
    // provider and user identifier envelope before deliberately ignoring it.
    if (countryCode == 0xb5) {
        if (payload.size() - pos < 6) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        pos += 2; // terminal_provider_code
        pos += 4; // provider_oriented_code / user_identifier
        Q_UNUSED(pos);
    }
    return result; // syntactically valid but no explicitly supported ATC registration
}

H26xTimingDetail::TimecodeParseResult extractFromSeiRbsp(const QByteArray& rbsp,
                                                         NativeVideoCodec codec,
                                                         const H26xTimingContext* context,
                                                         bool prefixSei) {
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

        TimecodeParseResult parsed;
        bool handled = false;
        if (codec == NativeVideoCodec::H264 && payloadType == 1 && context != nullptr &&
            context->codec() == NativeVideoCodec::H264 && context->h264() != nullptr) {
            parsed = H26xTimingDetail::parseH264PicTiming(rbsp.mid(pos, int(payloadSize)),
                                                          *context->h264());
            handled = true;
        } else if (codec == NativeVideoCodec::H264 && payloadType == 1) {
            sawUnsupported = true;
        } else if (payloadType == 4 &&
                   (codec == NativeVideoCodec::H264 || codec == NativeVideoCodec::Hevc)) {
            parsed = parseRegisteredT35(rbsp.mid(pos, int(payloadSize)));
            handled = true;
        } else if (codec == NativeVideoCodec::Hevc && payloadType == 136 && prefixSei &&
                   context != nullptr && context->codec() == NativeVideoCodec::Hevc &&
                   context->hevc() != nullptr) {
            parsed = H26xTimingDetail::parseHevcTimeCode(rbsp.mid(pos, int(payloadSize)),
                                                         *context->hevc());
            handled = true;
        } else if (codec == NativeVideoCodec::Hevc && payloadType == 136 && prefixSei) {
            sawUnsupported = true;
        }
        if (handled) {
            if (parsed.status == TimecodeParseStatus::Malformed) sawMalformed = true;
            if (parsed.status == TimecodeParseStatus::Unsupported) sawUnsupported = true;
            if (parsed.status == TimecodeParseStatus::Valid && !result.timecode.valid)
                result = parsed;
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

H26xSeiTimecodeResult extractResult(const QByteArray& annexB, NativeVideoCodec codec,
                                    const H26xTimingContext* context) {
    if (annexB.isEmpty() || codec == NativeVideoCodec::Unknown) return {};
    using H26xTimingDetail::TimecodeParseStatus;

    H26xSeiTimecodeResult firstUsableTimestamp;
    bool sawUnsupported = false;
    bool sawMalformed = false;
    for (const QByteArray& nal : splitAnnexBNals(annexB)) {
        if (!isSeiNal(nal, codec)) continue;
        if (codec == NativeVideoCodec::H264 && (uchar(nal[0]) & 0xe0u) != 0) {
            sawMalformed = true;
            continue;
        }
        if (codec == NativeVideoCodec::Hevc &&
            (((uchar(nal[0]) & 0x80u) != 0) || (uchar(nal[1]) & 0x07u) == 0)) {
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
        const bool prefixSei =
            codec != NativeVideoCodec::Hevc || (((uchar(nal[0]) >> 1) & 0x3f) == 39);
        const auto parsed = extractFromSeiRbsp(rbsp, codec, context, prefixSei);
        if (parsed.status == TimecodeParseStatus::Malformed) {
            sawMalformed = true;
        } else if (parsed.status == TimecodeParseStatus::Unsupported) {
            sawUnsupported = true;
        } else if (parsed.status == TimecodeParseStatus::Valid &&
                   !firstUsableTimestamp.timecode.valid) {
            firstUsableTimestamp.timecode = parsed.timecode;
            firstUsableTimestamp.labelRate = parsed.labelRate;
            firstUsableTimestamp.provenance = parsed.provenance;
            firstUsableTimestamp.discontinuity = parsed.discontinuity;
        }
    }
    if (sawMalformed || sawUnsupported) return {};
    return firstUsableTimestamp;
}

} // namespace

Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec) {
    return extractResult(annexB, codec, nullptr).timecode;
}

Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec,
                                        const H26xTimingContext& context) {
    return extractResult(annexB, codec, &context).timecode;
}

H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray& annexB, NativeVideoCodec codec,
                                                   const H26xTimingContext& context) {
    return extractResult(annexB, codec, &context);
}

#include "h26xseitimecode.h"

#include <QList>

#include <algorithm>
#include <limits>
#include <optional>

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
    if (pos >= payload.size()) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }

    // T.35 assigns the remaining syntax to the registered provider. There is
    // no published ATC profile selected by this project, so no provider-owned
    // body is decoded. For the well-known US ATSC namespace, validate the
    // provider and user identifier envelope before deliberately ignoring it.
    if (countryCode == 0xb5) {
        if (payload.size() - pos < 2) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        const uint16_t providerCode =
            (uint16_t(uchar(payload[pos])) << 8) | uint16_t(uchar(payload[pos + 1]));
        pos += 2;
        if (providerCode == 0x0031) {
            if (payload.size() - pos < 4) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            pos += 4; // ATSC provider_oriented_code / user_identifier
        }
    }
    Q_UNUSED(pos);
    return result; // syntactically valid but no explicitly supported ATC registration
}

bool equivalentTimecodeResult(const H26xTimingDetail::TimecodeParseResult& lhs,
                              const H26xTimingDetail::TimecodeParseResult& rhs) {
    return lhs.timecode.valid == rhs.timecode.valid && lhs.timecode.hours == rhs.timecode.hours &&
           lhs.timecode.minutes == rhs.timecode.minutes &&
           lhs.timecode.seconds == rhs.timecode.seconds &&
           lhs.timecode.frames == rhs.timecode.frames &&
           lhs.timecode.dropFrame == rhs.timecode.dropFrame && lhs.labelRate == rhs.labelRate &&
           lhs.provenance == rhs.provenance && lhs.discontinuity == rhs.discontinuity;
}

bool equivalentParsedTimecodeResult(const H26xTimingDetail::TimecodeParseResult& lhs,
                                    const H26xTimingDetail::TimecodeParseResult& rhs) {
    return lhs.status == rhs.status && equivalentTimecodeResult(lhs, rhs);
}

bool equivalentContinuity(const H26xTimingDetail::HevcTimeCodeContinuity& lhs,
                          const H26xTimingDetail::HevcTimeCodeContinuity& rhs) {
    return lhs.haveSeconds == rhs.haveSeconds && lhs.haveMinutes == rhs.haveMinutes &&
           lhs.haveHours == rhs.haveHours && (!lhs.haveSeconds || lhs.seconds == rhs.seconds) &&
           (!lhs.haveMinutes || lhs.minutes == rhs.minutes) &&
           (!lhs.haveHours || lhs.hours == rhs.hours) &&
           lhs.haveFrameSemantics == rhs.haveFrameSemantics &&
           (!lhs.haveFrameSemantics ||
            (lhs.frames == rhs.frames && lhs.labelRate == rhs.labelRate &&
             lhs.countingType == rhs.countingType && lhs.dropFrame == rhs.dropFrame));
}

bool equivalentTimecodeResult(const H26xSeiTimecodeResult& lhs,
                              const H26xTimingDetail::TimecodeParseResult& rhs) {
    H26xTimingDetail::TimecodeParseResult converted;
    converted.timecode = lhs.timecode;
    converted.labelRate = lhs.labelRate;
    converted.provenance = lhs.provenance;
    converted.discontinuity = lhs.discontinuity;
    return equivalentTimecodeResult(converted, rhs);
}

bool equivalentTimecodeResult(const H26xSeiTimecodeResult& lhs, const H26xSeiTimecodeResult& rhs) {
    return lhs.status == rhs.status && lhs.timecode.valid == rhs.timecode.valid &&
           lhs.timecode.hours == rhs.timecode.hours &&
           lhs.timecode.minutes == rhs.timecode.minutes &&
           lhs.timecode.seconds == rhs.timecode.seconds &&
           lhs.timecode.frames == rhs.timecode.frames &&
           lhs.timecode.dropFrame == rhs.timecode.dropFrame && lhs.labelRate == rhs.labelRate &&
           lhs.provenance == rhs.provenance && lhs.discontinuity == rhs.discontinuity;
}

bool equivalentOutput(const H26xTimingDetail::HevcTimeCodeOutput& lhs,
                      const H26xTimingDetail::HevcTimeCodeOutput& rhs) {
    return lhs.present == rhs.present && lhs.comparable == rhs.comparable &&
           (!lhs.comparable || lhs.clockTimestamp == rhs.clockTimestamp) &&
           (!lhs.present ||
            (lhs.frames == rhs.frames && lhs.maxFps == rhs.maxFps &&
             lhs.countingType == rhs.countingType && lhs.countDropped == rhs.countDropped &&
             lhs.discontinuity == rhs.discontinuity));
}

int clockCountForHevcPicStruct(int picStruct) {
    switch (picStruct) {
    case 0:
    case 1:
    case 2:
    case 9:
    case 10:
    case 11:
    case 12:
        return 1;
    case 3:
    case 4:
    case 7:
        return 2;
    case 5:
    case 6:
    case 8:
        return 3;
    default:
        return -1;
    }
}

enum class HevcPicStructScanStatus : uint8_t { Valid, Unsupported, Malformed };

HevcPicStructScanStatus scanHevcAuSeiContext(const QList<QByteArray>& prefixRbsps,
                                             const HevcTimingSyntax& syntax, bool& sawTimeCode,
                                             int& expectedClockCount) {
    std::optional<int> picStruct;
    for (const QByteArray& rbsp : prefixRbsps) {
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
            if (!readSeiVarValue(rbsp, pos, payloadType) ||
                !readSeiVarValue(rbsp, pos, payloadSize) || payloadSize > rbsp.size() - pos) {
                return HevcPicStructScanStatus::Malformed;
            }
            if (payloadType == 136) sawTimeCode = true;
            if (payloadType == 1) {
                const auto parsed = H26xTimingDetail::parseHevcPictureTiming(
                    rbsp.mid(pos, int(payloadSize)), syntax);
                if (parsed.status == H26xTimingDetail::TimecodeParseStatus::Malformed)
                    return HevcPicStructScanStatus::Malformed;
                if (parsed.status == H26xTimingDetail::TimecodeParseStatus::Unsupported)
                    return HevcPicStructScanStatus::Unsupported;
                if (parsed.picStruct < 0) return HevcPicStructScanStatus::Unsupported;
                if (picStruct.has_value() && *picStruct != parsed.picStruct)
                    return HevcPicStructScanStatus::Unsupported;
                picStruct = parsed.picStruct;
            }
            pos += int(payloadSize);
        }
        if (!sawTrailingBits || pos != rbsp.size()) return HevcPicStructScanStatus::Malformed;
    }
    if (!sawTimeCode) return HevcPicStructScanStatus::Valid;
    if (!picStruct.has_value()) return HevcPicStructScanStatus::Unsupported;
    expectedClockCount = clockCountForHevcPicStruct(*picStruct);
    return expectedClockCount > 0 ? HevcPicStructScanStatus::Valid
                                  : HevcPicStructScanStatus::Unsupported;
}

H26xTimingDetail::TimecodeParseResult extractFromSeiRbsp(
    const QByteArray& rbsp, NativeVideoCodec codec, const H26xTimingContext* context,
    bool prefixSei, const H26xTimingDetail::HevcTimeCodeContinuity* hevcPreviousContinuity,
    H26xTimingDetail::HevcTimeCodeContinuity* hevcNextContinuity, bool* hasHevcNextContinuity,
    bool* hasHevcTimeCodeMessage, int expectedHevcClockCount,
    const H26xTimingDetail::HevcTimeCodeOutput* hevcPreviousOutput,
    H26xTimingDetail::HevcTimeCodeOutput* hevcFirstOutput,
    H26xTimingDetail::HevcTimeCodeOutput* hevcLastOutput) {
    using H26xTimingDetail::TimecodeParseResult;
    using H26xTimingDetail::TimecodeParseStatus;

    TimecodeParseResult result;
    bool sawMalformed = false;
    bool sawUnsupported = false;
    bool sawTrailingBits = false;
    std::optional<TimecodeParseResult> firstHevcTimeCodeMessage;
    std::optional<H26xTimingDetail::HevcTimeCodeContinuity> firstHevcContinuityProposal;
    std::optional<H26xTimingDetail::HevcTimeCodeOutput> firstHevcFirstOutput;
    std::optional<H26xTimingDetail::HevcTimeCodeOutput> firstHevcLastOutput;
    if (hasHevcNextContinuity != nullptr) *hasHevcNextContinuity = false;
    if (hasHevcTimeCodeMessage != nullptr) *hasHevcTimeCodeMessage = false;
    if (hevcFirstOutput != nullptr) *hevcFirstOutput = {};
    if (hevcLastOutput != nullptr) *hevcLastOutput = {};
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
            // Distinct SEI payload cases legitimately share the "unsupported" outcome; they
            // cannot merge because the specific handled cases must precede these fallbacks.
            // NOLINTNEXTLINE(bugprone-branch-clone)
        } else if (codec == NativeVideoCodec::H264 && payloadType == 1) {
            sawUnsupported = true;
        } else if (payloadType == 4 &&
                   (codec == NativeVideoCodec::H264 || codec == NativeVideoCodec::Hevc)) {
            parsed = parseRegisteredT35(rbsp.mid(pos, int(payloadSize)));
            handled = true;
        } else if (codec == NativeVideoCodec::Hevc && payloadType == 136 && prefixSei &&
                   context != nullptr && context->codec() == NativeVideoCodec::Hevc &&
                   context->hevc() != nullptr) {
            H26xTimingDetail::HevcTimeCodeContinuity updatedContinuity;
            H26xTimingDetail::HevcTimeCodeOutput messageFirstOutput;
            H26xTimingDetail::HevcTimeCodeOutput messageLastOutput;
            parsed = H26xTimingDetail::parseHevcTimeCode(
                rbsp.mid(pos, int(payloadSize)), *context->hevc(), hevcPreviousContinuity,
                &updatedContinuity, expectedHevcClockCount, hevcPreviousOutput, &messageFirstOutput,
                &messageLastOutput);
            if (hasHevcTimeCodeMessage != nullptr) *hasHevcTimeCodeMessage = true;
            if (!firstHevcTimeCodeMessage.has_value()) {
                firstHevcTimeCodeMessage = parsed;
            } else if (!equivalentParsedTimecodeResult(*firstHevcTimeCodeMessage, parsed)) {
                sawMalformed = true;
            }
            if (parsed.status != TimecodeParseStatus::Malformed &&
                parsed.status != TimecodeParseStatus::Unsupported) {
                if (!firstHevcContinuityProposal.has_value()) {
                    firstHevcContinuityProposal = updatedContinuity;
                } else if (!equivalentContinuity(*firstHevcContinuityProposal, updatedContinuity)) {
                    sawMalformed = true;
                }
            }
            if (messageFirstOutput.present) {
                if (!firstHevcFirstOutput.has_value()) {
                    firstHevcFirstOutput = messageFirstOutput;
                    firstHevcLastOutput = messageLastOutput;
                } else if (!equivalentOutput(*firstHevcFirstOutput, messageFirstOutput) ||
                           !equivalentOutput(*firstHevcLastOutput, messageLastOutput)) {
                    sawMalformed = true;
                }
            }
            handled = true;
        } else if (codec == NativeVideoCodec::Hevc && payloadType == 136 && prefixSei) {
            sawUnsupported = true;
        }
        if (handled) {
            if (parsed.status == TimecodeParseStatus::Malformed) sawMalformed = true;
            if (parsed.status == TimecodeParseStatus::Unsupported) sawUnsupported = true;
            if (parsed.status == TimecodeParseStatus::Valid) {
                if (!result.timecode.valid) {
                    result = parsed;
                } else if (codec == NativeVideoCodec::Hevc &&
                           !equivalentTimecodeResult(result, parsed)) {
                    sawMalformed = true;
                }
            }
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
    if (result.status != TimecodeParseStatus::Malformed &&
        result.status != TimecodeParseStatus::Unsupported &&
        firstHevcContinuityProposal.has_value()) {
        if (hevcNextContinuity != nullptr) *hevcNextContinuity = *firstHevcContinuityProposal;
        if (hasHevcNextContinuity != nullptr) *hasHevcNextContinuity = true;
    }
    if (firstHevcFirstOutput.has_value()) {
        if (hevcFirstOutput != nullptr) *hevcFirstOutput = *firstHevcFirstOutput;
        if (hevcLastOutput != nullptr) *hevcLastOutput = *firstHevcLastOutput;
    }
    return result;
}

H26xSeiTimecodeResult
extractResult(const QByteArray& annexB, NativeVideoCodec codec, const H26xTimingContext* context,
              H26xTimingDetail::HevcTimeCodeContinuity* hevcContinuity = nullptr,
              bool* accepted = nullptr,
              const H26xTimingDetail::HevcTimeCodeOutput* hevcPreviousOutput = nullptr,
              H26xTimingDetail::HevcTimeCodeOutput* hevcFirstOutput = nullptr,
              H26xTimingDetail::HevcTimeCodeOutput* hevcLastOutput = nullptr) {
    if (accepted != nullptr) *accepted = true;
    if (annexB.isEmpty() || codec == NativeVideoCodec::Unknown) return {};
    using H26xTimingDetail::TimecodeParseStatus;

    struct SeiNalData {
        QByteArray rbsp;
        bool prefix = false;
    };

    H26xSeiTimecodeResult firstUsableTimestamp;
    bool sawUnsupported = false;
    bool sawMalformed = false;
    std::optional<H26xTimingDetail::TimecodeParseResult> firstHevcTimeCodeMessage;
    std::optional<H26xTimingDetail::HevcTimeCodeContinuity> firstHevcContinuityProposal;
    std::optional<H26xTimingDetail::HevcTimeCodeOutput> firstHevcFirstOutput;
    std::optional<H26xTimingDetail::HevcTimeCodeOutput> firstHevcLastOutput;
    QList<SeiNalData> seiNals;
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
        if (codec == NativeVideoCodec::Hevc) {
            const int layerId =
                static_cast<int>(((uchar(nal[0]) & 0x01u) << 5) | (uchar(nal[1]) >> 3));
            if (layerId != 0) {
                sawUnsupported = true;
                continue;
            }
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
        seiNals.append({rbsp, prefixSei});
    }

    int expectedHevcClockCount = -1;
    if (codec == NativeVideoCodec::Hevc && context != nullptr && context->hevc() != nullptr &&
        context->hevc()->frameFieldInfoPresent) {
        QList<QByteArray> prefixRbsps;
        for (const SeiNalData& nal : seiNals) {
            if (nal.prefix) prefixRbsps.append(nal.rbsp);
        }
        bool sawTimeCode = false;
        const HevcPicStructScanStatus status = scanHevcAuSeiContext(
            prefixRbsps, *context->hevc(), sawTimeCode, expectedHevcClockCount);
        if (status == HevcPicStructScanStatus::Malformed) sawMalformed = true;
        if (status == HevcPicStructScanStatus::Unsupported) sawUnsupported = true;
    }

    for (const SeiNalData& nal : seiNals) {
        H26xTimingDetail::HevcTimeCodeContinuity proposedContinuity;
        bool hasContinuityProposal = false;
        bool hasTimeCodeMessage = false;
        H26xTimingDetail::HevcTimeCodeOutput proposedFirstOutput;
        H26xTimingDetail::HevcTimeCodeOutput proposedLastOutput;
        const auto parsed = extractFromSeiRbsp(
            nal.rbsp, codec, context, nal.prefix, hevcContinuity, &proposedContinuity,
            &hasContinuityProposal, &hasTimeCodeMessage, expectedHevcClockCount, hevcPreviousOutput,
            &proposedFirstOutput, &proposedLastOutput);
        if (hasTimeCodeMessage) {
            if (!firstHevcTimeCodeMessage.has_value()) {
                firstHevcTimeCodeMessage = parsed;
            } else if (!equivalentParsedTimecodeResult(*firstHevcTimeCodeMessage, parsed)) {
                sawMalformed = true;
            }
        }
        if (hasContinuityProposal) {
            if (!firstHevcContinuityProposal.has_value()) {
                firstHevcContinuityProposal = proposedContinuity;
            } else if (!equivalentContinuity(*firstHevcContinuityProposal, proposedContinuity)) {
                sawMalformed = true;
            }
        }
        if (proposedFirstOutput.present) {
            if (!firstHevcFirstOutput.has_value()) {
                firstHevcFirstOutput = proposedFirstOutput;
                firstHevcLastOutput = proposedLastOutput;
            } else if (!equivalentOutput(*firstHevcFirstOutput, proposedFirstOutput) ||
                       !equivalentOutput(*firstHevcLastOutput, proposedLastOutput)) {
                sawMalformed = true;
            }
        }
        // The Malformed and the Hevc-mismatch cases share the sawMalformed outcome but are
        // distinct, order-dependent status checks that cannot be merged.
        // NOLINTNEXTLINE(bugprone-branch-clone)
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
        } else if (parsed.status == TimecodeParseStatus::Valid && codec == NativeVideoCodec::Hevc &&
                   !equivalentTimecodeResult(firstUsableTimestamp, parsed)) {
            sawMalformed = true;
        }
    }
    if (sawMalformed || sawUnsupported) {
        if (accepted != nullptr) *accepted = false;
        H26xSeiTimecodeResult rejected;
        rejected.status =
            sawMalformed ? TimecodeParseStatus::Malformed : TimecodeParseStatus::Unsupported;
        return rejected;
    }
    if (hevcContinuity != nullptr && firstHevcContinuityProposal.has_value())
        *hevcContinuity = *firstHevcContinuityProposal;
    if (firstHevcFirstOutput.has_value()) {
        if (hevcFirstOutput != nullptr) *hevcFirstOutput = *firstHevcFirstOutput;
        if (hevcLastOutput != nullptr) *hevcLastOutput = *firstHevcLastOutput;
    }
    firstUsableTimestamp.status = firstUsableTimestamp.timecode.valid
                                      ? TimecodeParseStatus::Valid
                                      : TimecodeParseStatus::NoTimestamp;
    return firstUsableTimestamp;
}

} // namespace

void H26xSeiTimecodeState::reset() {
    m_contextBound = false;
    m_contextIdentity = 0;
    m_contextGeneration = 0;
    m_codec = NativeVideoCodec::Unknown;
    m_hevcContinuity = {};
    m_outputOrderBound = false;
    m_sourceGeneration = 0;
    m_timingGeneration = 0;
    m_outputDomain = 0;
    m_outputEpoch = 0;
    m_haveEvictedOutput = false;
    m_evictedThrough = 0;
    m_outputEntryCount = 0;
}

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

H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray& annexB, NativeVideoCodec codec,
                                                   const H26xTimingContext& context,
                                                   H26xSeiTimecodeState& state) {
    if (!state.m_contextBound || state.m_contextIdentity != context.identity() ||
        state.m_contextGeneration != context.generation() || state.m_codec != codec) {
        state.reset();
        state.m_contextBound = true;
        state.m_contextIdentity = context.identity();
        state.m_contextGeneration = context.generation();
        state.m_codec = codec;
    }

    // Without a caller-supplied presentation identity the current AU can be
    // parsed in isolation, but no decoding/output predecessor is retained or
    // consumed. Any cross-AU inference therefore fails closed instead of
    // treating decode-call order as output order.
    H26xTimingDetail::HevcTimeCodeOutput firstOutput;
    H26xTimingDetail::HevcTimeCodeOutput lastOutput;
    H26xSeiTimecodeResult result = extractResult(annexB, codec, &context, nullptr, nullptr, nullptr,
                                                 &firstOutput, &lastOutput);
    if (firstOutput.present && firstOutput.countDropped && !firstOutput.discontinuity) {
        result = {};
        result.status = H26xTimingDetail::TimecodeParseStatus::Unsupported;
    }
    return result;
}

H26xSeiTimecodeResult extractH26xSeiTimecodeResult(const QByteArray& annexB, NativeVideoCodec codec,
                                                   const H26xTimingContext& context,
                                                   H26xSeiTimecodeState& state,
                                                   const H26xSeiOutputOrderKey& outputOrder) {
    using H26xTimingDetail::TimecodeParseStatus;
    const auto rejected = [](TimecodeParseStatus status) {
        H26xSeiTimecodeResult result;
        result.status = status;
        return result;
    };

    if (!state.m_contextBound || state.m_contextIdentity != context.identity() ||
        state.m_contextGeneration != context.generation() || state.m_codec != codec) {
        state.reset();
        state.m_contextBound = true;
        state.m_contextIdentity = context.identity();
        state.m_contextGeneration = context.generation();
        state.m_codec = codec;
    }
    if (outputOrder.timingGeneration != context.generation())
        return rejected(TimecodeParseStatus::Unsupported);

    if (!state.m_outputOrderBound) {
        state.m_outputOrderBound = true;
        state.m_sourceGeneration = outputOrder.sourceGeneration;
        state.m_timingGeneration = outputOrder.timingGeneration;
        state.m_outputDomain = outputOrder.domain;
        state.m_outputEpoch = outputOrder.epoch;
    } else if (outputOrder.sourceGeneration < state.m_sourceGeneration) {
        return rejected(TimecodeParseStatus::Unsupported);
    } else if (outputOrder.sourceGeneration > state.m_sourceGeneration) {
        state.m_sourceGeneration = outputOrder.sourceGeneration;
        state.m_timingGeneration = outputOrder.timingGeneration;
        state.m_outputDomain = outputOrder.domain;
        state.m_outputEpoch = outputOrder.epoch;
        state.m_hevcContinuity = {};
        state.m_outputEntryCount = 0;
        state.m_haveEvictedOutput = false;
    } else {
        if (outputOrder.domain != state.m_outputDomain ||
            outputOrder.timingGeneration != state.m_timingGeneration ||
            outputOrder.epoch < state.m_outputEpoch) {
            return rejected(TimecodeParseStatus::Unsupported);
        }
        if (outputOrder.epoch > state.m_outputEpoch) {
            state.m_outputEpoch = outputOrder.epoch;
            state.m_hevcContinuity = {};
            state.m_outputEntryCount = 0;
            state.m_haveEvictedOutput = false;
        }
    }

    const auto entriesBegin = state.m_outputEntries.begin();
    const auto entriesEnd = entriesBegin + std::ptrdiff_t(state.m_outputEntryCount);
    const auto successor =
        std::lower_bound(entriesBegin, entriesEnd, outputOrder.presentationKey,
                         [](const H26xSeiTimecodeState::StoredOutputEntry& entry, int64_t key) {
                             return entry.presentationKey < key;
                         });
    const bool duplicate =
        successor != entriesEnd && successor->presentationKey == outputOrder.presentationKey;
    if (!duplicate && state.m_outputEntryCount == H26xSeiTimecodeState::kMaxRetainedOutputEntries &&
        outputOrder.presentationKey < state.m_outputEntries.front().presentationKey) {
        return rejected(TimecodeParseStatus::Unsupported);
    }
    if (state.m_haveEvictedOutput &&
        (outputOrder.presentationKey <= state.m_evictedThrough ||
         (state.m_outputEntryCount != 0 &&
          outputOrder.presentationKey < state.m_outputEntries.front().presentationKey))) {
        return rejected(TimecodeParseStatus::Unsupported);
    }
    const H26xTimingDetail::HevcTimeCodeOutput* previousOutput = nullptr;
    if (successor != entriesBegin) {
        previousOutput = &std::prev(successor)->output.last;
    }

    H26xTimingDetail::HevcTimeCodeContinuity workingContinuity = state.m_hevcContinuity;
    H26xTimingDetail::HevcTimeCodeOutput firstOutput;
    H26xTimingDetail::HevcTimeCodeOutput lastOutput;
    bool accepted = false;
    H26xSeiTimecodeResult result =
        extractResult(annexB, codec, &context, &workingContinuity, &accepted, previousOutput,
                      &firstOutput, &lastOutput);
    if (!accepted) {
        state.m_hevcContinuity = {};
        return result;
    }
    if (!firstOutput.present) {
        state.m_hevcContinuity = workingContinuity;
        return result;
    }

    if (duplicate) {
        const H26xSeiTimecodeState::OutputEntry& retained = successor->output;
        if (!equivalentOutput(retained.first, firstOutput) ||
            !equivalentOutput(retained.last, lastOutput) ||
            !equivalentTimecodeResult(retained.result, result)) {
            state.m_hevcContinuity = {};
            return rejected(TimecodeParseStatus::Malformed);
        }
        return retained.result;
    }

    if (successor != entriesEnd) {
        const TimecodeParseStatus successorStatus =
            H26xTimingDetail::validateHevcOutputTransition(&lastOutput, successor->output.first);
        if (successorStatus == TimecodeParseStatus::Malformed) {
            state.m_hevcContinuity = {};
            return rejected(successorStatus);
        }
        if (successorStatus == TimecodeParseStatus::Unsupported) {
            state.m_hevcContinuity = {};
            return rejected(successorStatus);
        }
    }

    state.m_hevcContinuity = workingContinuity;
    if (state.m_outputEntryCount == H26xSeiTimecodeState::kMaxRetainedOutputEntries) {
        state.m_haveEvictedOutput = true;
        state.m_evictedThrough = state.m_outputEntries.front().presentationKey;
        std::move(state.m_outputEntries.begin() + 1, state.m_outputEntries.end(),
                  state.m_outputEntries.begin());
        --state.m_outputEntryCount;
    }
    const auto insertEnd = state.m_outputEntries.begin() + std::ptrdiff_t(state.m_outputEntryCount);
    const auto insertAt =
        std::lower_bound(state.m_outputEntries.begin(), insertEnd, outputOrder.presentationKey,
                         [](const H26xSeiTimecodeState::StoredOutputEntry& entry, int64_t key) {
                             return entry.presentationKey < key;
                         });
    std::move_backward(insertAt, insertEnd, insertEnd + 1);
    *insertAt = {outputOrder.presentationKey,
                 H26xSeiTimecodeState::OutputEntry{firstOutput, lastOutput, result}};
    ++state.m_outputEntryCount;
    return result;
}

#include "h26xtimingcontext.h"

#include <limits>
#include <numeric>

namespace {

class BitReader {
public:
    explicit BitReader(const QByteArray& bytes) : m_bytes(bytes) {}

    bool bit(bool& value) {
        uint32_t raw = 0;
        if (!bits(1, raw)) return false;
        value = raw != 0;
        return true;
    }

    bool bits(int count, uint32_t& value) {
        if (count < 0 || count > 32 || m_bitOffset > int64_t(m_bytes.size()) * 8 - count) {
            m_malformed = true;
            value = 0;
            return false;
        }
        uint32_t out = 0;
        for (int i = 0; i < count; ++i) {
            const int64_t byteOffset = m_bitOffset / 8;
            const int bitInByte = int(m_bitOffset % 8);
            out = (out << 1) | ((uchar(m_bytes[int(byteOffset)]) >> (7 - bitInByte)) & 1u);
            ++m_bitOffset;
        }
        value = out;
        return true;
    }

    bool ue(uint32_t& value) {
        int leadingZeroBits = 0;
        bool current = false;
        while (true) {
            if (!bit(current)) return false;
            if (current) break;
            if (++leadingZeroBits > 31) {
                m_malformed = true;
                return false;
            }
        }
        uint32_t suffix = 0;
        if (!bits(leadingZeroBits, suffix)) return false;
        const uint64_t decoded = (uint64_t(1) << leadingZeroBits) - 1u + suffix;
        if (decoded > std::numeric_limits<uint32_t>::max()) {
            m_malformed = true;
            return false;
        }
        value = uint32_t(decoded);
        return true;
    }

    bool se(int32_t& value) {
        uint32_t codeNum = 0;
        if (!ue(codeNum)) return false;
        if ((codeNum & 1u) != 0) {
            const uint64_t positive = (uint64_t(codeNum) + 1u) / 2u;
            if (positive > uint64_t(std::numeric_limits<int32_t>::max())) {
                m_malformed = true;
                return false;
            }
            value = int32_t(positive);
        } else {
            value = -int32_t(codeNum / 2u);
        }
        return true;
    }

    bool signedBits(int count, int32_t& value) {
        uint32_t raw = 0;
        if (!bits(count, raw)) return false;
        if (count == 0) {
            value = 0;
            return true;
        }
        const uint32_t sign = uint32_t(1) << (count - 1);
        if ((raw & sign) == 0) {
            value = int32_t(raw);
        } else {
            value = int32_t(int64_t(raw) - (int64_t(1) << count));
        }
        return true;
    }

    bool rbspTrailingBits() {
        bool stopBit = false;
        if (!bit(stopBit) || !stopBit) return false;
        const int64_t bitCount = int64_t(m_bytes.size()) * 8;
        while (m_bitOffset < bitCount) {
            bool paddingBit = false;
            if (!bit(paddingBit) || paddingBit) return false;
        }
        return true;
    }

    bool seiPayloadAlignmentBits() {
        const int64_t bitCount = int64_t(m_bytes.size()) * 8;
        // Annex D adds bit_equal_to_one/zero only when the payload syntax is
        // not already byte aligned. At an aligned position, extra bits are data.
        if (m_bitOffset == bitCount) return true;
        if ((m_bitOffset & 7) == 0) return false;

        bool marker = false;
        if (!bit(marker) || !marker) return false;
        while ((m_bitOffset & 7) != 0) {
            bool padding = false;
            if (!bit(padding) || padding) return false;
        }
        return m_bitOffset == bitCount;
    }

private:
    const QByteArray& m_bytes;
    int64_t m_bitOffset = 0;
    bool m_malformed = false;
};

bool highProfileHasExtendedSyntax(uint32_t profileIdc) {
    switch (profileIdc) {
    case 44:
    case 83:
    case 86:
    case 100:
    case 110:
    case 118:
    case 122:
    case 128:
    case 134:
    case 135:
    case 138:
    case 139:
    case 244:
        return true;
    default:
        return false;
    }
}

QByteArray removeAnnexBPrefix(const QByteArray& bytes) {
    if (bytes.startsWith(QByteArray::fromHex("00000001"))) return bytes.mid(4);
    if (bytes.startsWith(QByteArray::fromHex("000001"))) return bytes.mid(3);
    return bytes;
}

bool skipScalingList(BitReader& reader, int size) {
    int lastScale = 8;
    int nextScale = 8;
    for (int i = 0; i < size; ++i) {
        if (nextScale != 0) {
            int32_t deltaScale = 0;
            if (!reader.se(deltaScale)) return false;
            int64_t reduced = (int64_t(lastScale) + deltaScale) % 256;
            if (reduced < 0) reduced += 256;
            nextScale = int(reduced);
        }
        if (nextScale != 0) lastScale = nextScale;
    }
    return true;
}

struct HrdSyntax {
    uint8_t cpbRemovalDelayLength = 0;
    uint8_t dpbOutputDelayLength = 0;
    uint8_t timeOffsetLength = 0;
};

bool parseHrd(BitReader& reader, HrdSyntax& syntax) {
    uint32_t cpbCountMinus1 = 0;
    uint32_t ignored = 0;
    if (!reader.ue(cpbCountMinus1) || cpbCountMinus1 > 31 || !reader.bits(4, ignored) ||
        !reader.bits(4, ignored)) {
        return false;
    }
    for (uint32_t i = 0; i <= cpbCountMinus1; ++i) {
        bool cbrFlag = false;
        if (!reader.ue(ignored) || !reader.ue(ignored) || !reader.bit(cbrFlag)) return false;
    }
    uint32_t initialDelayLengthMinus1 = 0;
    uint32_t cpbDelayLengthMinus1 = 0;
    uint32_t dpbDelayLengthMinus1 = 0;
    uint32_t timeOffsetLength = 0;
    if (!reader.bits(5, initialDelayLengthMinus1) || !reader.bits(5, cpbDelayLengthMinus1) ||
        !reader.bits(5, dpbDelayLengthMinus1) || !reader.bits(5, timeOffsetLength)) {
        return false;
    }
    syntax.cpbRemovalDelayLength = uint8_t(cpbDelayLengthMinus1 + 1);
    syntax.dpbOutputDelayLength = uint8_t(dpbDelayLengthMinus1 + 1);
    syntax.timeOffsetLength = uint8_t(timeOffsetLength);
    return true;
}

bool sameHrdSyntax(const HrdSyntax& lhs, const HrdSyntax& rhs) {
    return lhs.cpbRemovalDelayLength == rhs.cpbRemovalDelayLength &&
           lhs.dpbOutputDelayLength == rhs.dpbOutputDelayLength &&
           lhs.timeOffsetLength == rhs.timeOffsetLength;
}

FrameRateQ frameRateFromVui(uint32_t numUnitsInTick, uint32_t timeScale) {
    if (numUnitsInTick == 0 || timeScale == 0) return {};
    uint64_t num = timeScale;
    uint64_t den = uint64_t(numUnitsInTick) * 2u;
    const uint64_t divisor = std::gcd(num, den);
    num /= divisor;
    den /= divisor;
    if (num > uint64_t(std::numeric_limits<int32_t>::max()) ||
        den > uint64_t(std::numeric_limits<int32_t>::max()) || num * 1u < den * 12u ||
        num > den * 240u) {
        return {};
    }
    return FrameRateQ{int32_t(num), int32_t(den)};
}

H264TimingSyntax malformedH264() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Malformed;
    return syntax;
}

H264TimingSyntax unsupportedH264() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Unsupported;
    return syntax;
}

H264TimingSyntax parseH264Sps(const QByteArray& parameterSet) {
    const QByteArray nal = removeAnnexBPrefix(parameterSet);
    if (nal.size() < 4 || (uchar(nal[0]) & 0x80u) != 0 || (uchar(nal[0]) & 0x1fu) != 7)
        return malformedH264();

    QByteArray rbsp;
    if (!H26xTimingDetail::unescapeRbsp(nal.mid(1), rbsp)) return malformedH264();
    BitReader reader(rbsp);
    uint32_t profileIdc = 0;
    uint32_t constraintFlags = 0;
    uint32_t ignored = 0;
    uint32_t chromaFormatIdc = 1;
    if (!reader.bits(8, profileIdc) || !reader.bits(8, constraintFlags) ||
        (constraintFlags & 0x03u) != 0 || !reader.bits(8, ignored) || !reader.ue(ignored)) {
        return malformedH264();
    }

    if (profileIdc != 66 && profileIdc != 77 && profileIdc != 88 &&
        !highProfileHasExtendedSyntax(profileIdc)) {
        return unsupportedH264();
    }

    if (highProfileHasExtendedSyntax(profileIdc)) {
        if (!reader.ue(chromaFormatIdc) || chromaFormatIdc > 3) return unsupportedH264();
        bool flag = false;
        if ((chromaFormatIdc == 3 && !reader.bit(flag)) || !reader.ue(ignored) ||
            !reader.ue(ignored) || !reader.bit(flag) || !reader.bit(flag)) {
            return malformedH264();
        }
        if (flag) {
            const int scalingListCount = chromaFormatIdc == 3 ? 12 : 8;
            for (int i = 0; i < scalingListCount; ++i) {
                bool present = false;
                if (!reader.bit(present)) return malformedH264();
                if (present && !skipScalingList(reader, i < 6 ? 16 : 64)) return malformedH264();
            }
        }
    }

    uint32_t picOrderCntType = 0;
    if (!reader.ue(ignored) || !reader.ue(picOrderCntType) || picOrderCntType > 2)
        return malformedH264();
    if (picOrderCntType == 0) {
        if (!reader.ue(ignored)) return malformedH264();
    } else if (picOrderCntType == 1) {
        bool flag = false;
        int32_t signedValue = 0;
        uint32_t cycleCount = 0;
        if (!reader.bit(flag) || !reader.se(signedValue) || !reader.se(signedValue) ||
            !reader.ue(cycleCount) || cycleCount > 255) {
            return malformedH264();
        }
        for (uint32_t i = 0; i < cycleCount; ++i) {
            if (!reader.se(signedValue)) return malformedH264();
        }
    }

    bool flag = false;
    bool frameMbsOnly = false;
    if (!reader.ue(ignored) || !reader.bit(flag) || !reader.ue(ignored) || !reader.ue(ignored) ||
        !reader.bit(frameMbsOnly) || (!frameMbsOnly && !reader.bit(flag)) || !reader.bit(flag) ||
        !reader.bit(flag)) {
        return malformedH264();
    }
    if (flag) {
        for (int i = 0; i < 4; ++i) {
            if (!reader.ue(ignored)) return malformedH264();
        }
    }

    bool vuiPresent = false;
    if (!reader.bit(vuiPresent)) return malformedH264();
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    if (!vuiPresent) return syntax;

    bool present = false;
    if (!reader.bit(present)) return malformedH264();
    if (present) {
        uint32_t aspectRatioIdc = 0;
        if (!reader.bits(8, aspectRatioIdc)) return malformedH264();
        if (aspectRatioIdc == 255 && (!reader.bits(16, ignored) || !reader.bits(16, ignored))) {
            return malformedH264();
        }
    }
    if (!reader.bit(present) || (present && !reader.bit(flag)) || !reader.bit(present))
        return malformedH264();
    if (present) {
        if (!reader.bits(3, ignored) || !reader.bit(flag) || !reader.bit(flag))
            return malformedH264();
        if (flag &&
            (!reader.bits(8, ignored) || !reader.bits(8, ignored) || !reader.bits(8, ignored))) {
            return malformedH264();
        }
    }
    if (!reader.bit(present)) return malformedH264();
    if (present && (!reader.ue(ignored) || !reader.ue(ignored))) return malformedH264();

    bool timingInfoPresent = false;
    uint32_t numUnitsInTick = 0;
    uint32_t timeScale = 0;
    if (!reader.bit(timingInfoPresent)) return malformedH264();
    if (timingInfoPresent) {
        if (!reader.bits(32, numUnitsInTick) || !reader.bits(32, timeScale) ||
            !reader.bit(syntax.fixedFrameRate)) {
            return malformedH264();
        }
        syntax.numUnitsInTick = numUnitsInTick;
        syntax.timeScale = timeScale;
        syntax.frameRate = frameRateFromVui(numUnitsInTick, timeScale);
    }

    bool nalHrdPresent = false;
    bool vclHrdPresent = false;
    HrdSyntax nalHrd;
    HrdSyntax vclHrd;
    if (!reader.bit(nalHrdPresent) || (nalHrdPresent && !parseHrd(reader, nalHrd)) ||
        !reader.bit(vclHrdPresent) || (vclHrdPresent && !parseHrd(reader, vclHrd))) {
        return malformedH264();
    }
    if (nalHrdPresent && vclHrdPresent && !sameHrdSyntax(nalHrd, vclHrd)) return unsupportedH264();
    if (nalHrdPresent || vclHrdPresent) {
        if (!reader.bit(flag)) // low_delay_hrd_flag
            return malformedH264();
        const HrdSyntax& hrd = nalHrdPresent ? nalHrd : vclHrd;
        syntax.cpbDpbDelaysPresent = true;
        syntax.cpbRemovalDelayLength = hrd.cpbRemovalDelayLength;
        syntax.dpbOutputDelayLength = hrd.dpbOutputDelayLength;
        syntax.timeOffsetLength = hrd.timeOffsetLength;
    }
    if (!reader.bit(syntax.picStructPresent)) return malformedH264();
    bool bitstreamRestriction = false;
    if (!reader.bit(bitstreamRestriction)) return malformedH264();
    if (bitstreamRestriction) {
        if (!reader.bit(flag)) return malformedH264();
        for (int i = 0; i < 6; ++i) {
            if (!reader.ue(ignored)) return malformedH264();
        }
    }
    if (!reader.rbspTrailingBits()) return malformedH264();
    return syntax;
}

bool equivalentTiming(const H264TimingSyntax& lhs, const H264TimingSyntax& rhs) {
    // Equal normalized rates preserve Equation D-1 ordering under any positive
    // common scale when no signed offset is present. With time_offset syntax,
    // the raw tick scale is semantically significant and must also match.
    const bool clockScaleEquivalent =
        (lhs.timeOffsetLength == 0 && rhs.timeOffsetLength == 0) ||
        (lhs.numUnitsInTick == rhs.numUnitsInTick && lhs.timeScale == rhs.timeScale);
    return lhs.status == rhs.status && lhs.frameRate == rhs.frameRate && clockScaleEquivalent &&
           lhs.fixedFrameRate == rhs.fixedFrameRate &&
           lhs.cpbDpbDelaysPresent == rhs.cpbDpbDelaysPresent &&
           lhs.cpbRemovalDelayLength == rhs.cpbRemovalDelayLength &&
           lhs.dpbOutputDelayLength == rhs.dpbOutputDelayLength &&
           lhs.timeOffsetLength == rhs.timeOffsetLength &&
           lhs.picStructPresent == rhs.picStructPresent;
}

int clockTimestampCount(uint32_t picStruct) {
    static constexpr int counts[] = {1, 1, 1, 2, 2, 3, 3, 2, 3};
    return picStruct < std::size(counts) ? counts[picStruct] : 0;
}

bool normalizedRateEquals(FrameRateQ rate, int32_t numerator, int32_t denominator) {
    return rate.valid() && int64_t(rate.num) * denominator == int64_t(rate.den) * numerator;
}

bool validH264FrameCount(uint32_t frames, FrameRateQ rate) {
    if (!rate.valid()) return false;
    const int64_t maxFps = (int64_t(rate.num) + rate.den - 1) / rate.den;
    return int64_t(frames) < maxFps;
}

bool checkedAdd(int64_t lhs, int64_t rhs, int64_t& result) {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool checkedMultiplyNonnegative(int64_t lhs, int64_t rhs, int64_t& result) {
    if (lhs < 0 || rhs < 0 || (rhs != 0 && lhs > std::numeric_limits<int64_t>::max() / rhs)) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool h264ClockTimestamp(uint32_t hours, uint32_t minutes, uint32_t seconds, uint32_t frames,
                        bool nuitFieldBased, int32_t timeOffset, const H264TimingSyntax& syntax,
                        int64_t& result) {
    if (syntax.numUnitsInTick == 0 || syntax.timeScale == 0) return false;

    int64_t clockTimestamp = 0;
    if (!checkedMultiplyNonnegative(hours, 60, clockTimestamp) ||
        !checkedAdd(clockTimestamp, minutes, clockTimestamp) ||
        !checkedMultiplyNonnegative(clockTimestamp, 60, clockTimestamp) ||
        !checkedAdd(clockTimestamp, seconds, clockTimestamp) ||
        !checkedMultiplyNonnegative(clockTimestamp, syntax.timeScale, clockTimestamp)) {
        return false;
    }

    int64_t frameTicks = 0;
    if (!checkedMultiplyNonnegative(syntax.numUnitsInTick, 1 + int64_t(nuitFieldBased),
                                    frameTicks) ||
        !checkedMultiplyNonnegative(frames, frameTicks, frameTicks) ||
        !checkedAdd(clockTimestamp, frameTicks, clockTimestamp) ||
        !checkedAdd(clockTimestamp, timeOffset, clockTimestamp)) {
        return false;
    }
    result = clockTimestamp;
    return true;
}

} // namespace

bool H26xTimingDetail::unescapeRbsp(const QByteArray& escaped, QByteArray& rbsp) {
    QByteArray decoded;
    decoded.reserve(escaped.size());
    int zeroCount = 0;
    for (qsizetype i = 0; i < escaped.size(); ++i) {
        const uchar value = uchar(escaped[i]);
        if (zeroCount >= 2 && value == 0x03) {
            if (i + 1 >= escaped.size() || uchar(escaped[i + 1]) > 0x03) {
                rbsp.clear();
                return false;
            }
            zeroCount = 0;
            continue;
        }
        if (zeroCount >= 2 && value <= 0x02) {
            rbsp.clear();
            return false;
        }
        decoded.append(escaped[i]);
        zeroCount = value == 0 ? zeroCount + 1 : 0;
    }
    rbsp = decoded;
    return true;
}

bool H26xTimingContext::updateParameterSets(NativeVideoCodec codec, const QList<QByteArray>& vps,
                                            const QList<QByteArray>& sps) {
    const bool relevantParameterSetsUnchanged =
        codec == m_codec && sps == m_sps && (codec == NativeVideoCodec::H264 || vps == m_vps);
    if (relevantParameterSetsUnchanged) {
        if (codec == NativeVideoCodec::H264) return m_h264.status == H26xTimingSyntaxStatus::Valid;
        return false;
    }

    m_codec = codec;
    m_vps = codec == NativeVideoCodec::H264 ? QList<QByteArray>{} : vps;
    m_sps = sps;
    if (m_generation != std::numeric_limits<uint64_t>::max()) ++m_generation;
    m_h264 = H264TimingSyntax{};
    m_hevc = HevcTimingSyntax{};

    if (codec == NativeVideoCodec::Hevc) return false;
    if (codec != NativeVideoCodec::H264 || sps.isEmpty()) return false;

    H264TimingSyntax active;
    bool haveValid = false;
    bool sawMalformed = false;
    bool sawUnsupported = false;
    bool sawConflictingValid = false;
    for (const QByteArray& parameterSet : sps) {
        const H264TimingSyntax candidate = parseH264Sps(parameterSet);
        if (candidate.status == H26xTimingSyntaxStatus::Malformed) {
            sawMalformed = true;
        } else if (candidate.status == H26xTimingSyntaxStatus::Unsupported) {
            sawUnsupported = true;
        } else if (!haveValid) {
            active = candidate;
            haveValid = true;
        } else if (!equivalentTiming(active, candidate)) {
            sawConflictingValid = true;
        }
    }
    if (sawMalformed) {
        m_h264 = malformedH264();
        return false;
    }
    if (sawUnsupported || sawConflictingValid || !haveValid) {
        m_h264 = unsupportedH264();
        return false;
    }
    m_h264 = active;
    return true;
}

NativeVideoCodec H26xTimingContext::codec() const {
    return m_codec;
}

FrameRateQ H26xTimingContext::constantFrameRate() const {
    if (!fixedFrameRate()) return {};
    return m_h264.frameRate;
}

bool H26xTimingContext::fixedFrameRate() const {
    return m_codec == NativeVideoCodec::H264 && m_h264.status == H26xTimingSyntaxStatus::Valid &&
           m_h264.fixedFrameRate && m_h264.frameRate.valid();
}

uint64_t H26xTimingContext::generation() const {
    return m_generation;
}

const H264TimingSyntax* H26xTimingContext::h264() const {
    return m_codec == NativeVideoCodec::H264 ? &m_h264 : nullptr;
}

const HevcTimingSyntax* H26xTimingContext::hevc() const {
    return m_codec == NativeVideoCodec::Hevc ? &m_hevc : nullptr;
}

H26xTimingDetail::TimecodeParseResult
H26xTimingDetail::parseH264PicTiming(const QByteArray& payload, const H264TimingSyntax& syntax) {
    TimecodeParseResult result;
    if (syntax.status != H26xTimingSyntaxStatus::Valid) {
        result.status = syntax.status == H26xTimingSyntaxStatus::Malformed
                            ? TimecodeParseStatus::Malformed
                            : TimecodeParseStatus::Unsupported;
        return result;
    }

    BitReader reader(payload);
    uint32_t ignored = 0;
    if (syntax.cpbDpbDelaysPresent && (!reader.bits(syntax.cpbRemovalDelayLength, ignored) ||
                                       !reader.bits(syntax.dpbOutputDelayLength, ignored))) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }
    if (!syntax.picStructPresent) {
        if (!reader.seiPayloadAlignmentBits()) result.status = TimecodeParseStatus::Malformed;
        return result;
    }

    uint32_t picStruct = 0;
    if (!reader.bits(4, picStruct)) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }
    const int timestampCount = clockTimestampCount(picStruct);
    if (timestampCount == 0) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }

    Smpte12mTimecode firstUsableTimestamp;
    bool unsupportedMapping = false;
    bool orderingUnavailable = false;
    bool sawPresentTimestamp = false;
    bool previousTimestampComparable = false;
    int64_t previousClockTimestamp = 0;
    bool havePreviousSeconds = false;
    bool havePreviousMinutes = false;
    bool havePreviousHours = false;
    uint32_t previousSeconds = 0;
    uint32_t previousMinutes = 0;
    uint32_t previousHours = 0;
    for (int i = 0; i < timestampCount; ++i) {
        bool timestampFlag = false;
        if (!reader.bit(timestampFlag)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (!timestampFlag) continue;

        uint32_t ctType = 0;
        bool nuitFieldBased = false;
        uint32_t countingType = 0;
        bool fullTimestamp = false;
        bool discontinuity = false;
        bool countDropped = false;
        uint32_t frames = 0;
        if (!reader.bits(2, ctType) || !reader.bit(nuitFieldBased) ||
            !reader.bits(5, countingType) || !reader.bit(fullTimestamp) ||
            !reader.bit(discontinuity) || !reader.bit(countDropped) || !reader.bits(8, frames)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (ctType == 3 || countingType > 6) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }

        bool haveSeconds = false;
        bool haveMinutes = false;
        bool haveHours = false;
        uint32_t seconds = 0;
        uint32_t minutes = 0;
        uint32_t hours = 0;
        if (fullTimestamp) {
            haveSeconds = haveMinutes = haveHours = true;
            if (!reader.bits(6, seconds) || !reader.bits(6, minutes) || !reader.bits(5, hours)) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
        } else {
            if (!reader.bit(haveSeconds)) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            if (haveSeconds) {
                if (!reader.bits(6, seconds) || !reader.bit(haveMinutes)) {
                    result.status = TimecodeParseStatus::Malformed;
                    return result;
                }
                if (haveMinutes && (!reader.bits(6, minutes) || !reader.bit(haveHours))) {
                    result.status = TimecodeParseStatus::Malformed;
                    return result;
                }
                if (haveHours && !reader.bits(5, hours)) {
                    result.status = TimecodeParseStatus::Malformed;
                    return result;
                }
            }
        }

        int32_t timeOffset = 0;
        if (!reader.signedBits(syntax.timeOffsetLength, timeOffset)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        Q_UNUSED(ctType);

        if ((haveSeconds && seconds >= 60) || (haveMinutes && minutes >= 60) ||
            (haveHours && hours >= 24) || !validH264FrameCount(frames, syntax.frameRate)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }

        const bool effectiveSecondsPresent = haveSeconds || havePreviousSeconds;
        const bool effectiveMinutesPresent = haveMinutes || havePreviousMinutes;
        const bool effectiveHoursPresent = haveHours || havePreviousHours;
        const uint32_t effectiveSeconds = haveSeconds ? seconds : previousSeconds;
        const uint32_t effectiveMinutes = haveMinutes ? minutes : previousMinutes;
        const uint32_t effectiveHours = haveHours ? hours : previousHours;
        const bool effectiveTimestampComplete =
            effectiveSecondsPresent && effectiveMinutesPresent && effectiveHoursPresent;

        if (haveSeconds) {
            previousSeconds = seconds;
            havePreviousSeconds = true;
        }
        if (haveMinutes) {
            previousMinutes = minutes;
            havePreviousMinutes = true;
        }
        if (haveHours) {
            previousHours = hours;
            havePreviousHours = true;
        }

        bool currentTimestampComparable = false;
        int64_t currentClockTimestamp = 0;
        if (effectiveTimestampComplete && syntax.numUnitsInTick != 0 && syntax.timeScale != 0) {
            // Equation D-1 suppresses time_offset only for counting_type 0.
            // Types 1 through 6 all apply the signed offset even when their
            // label mapping is not representable by Smpte12mTimecode.
            const int32_t orderingTimeOffset = countingType == 0 ? 0 : timeOffset;
            if (!h264ClockTimestamp(effectiveHours, effectiveMinutes, effectiveSeconds, frames,
                                    nuitFieldBased, orderingTimeOffset, syntax,
                                    currentClockTimestamp)) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            currentTimestampComparable = true;
        }
        // Annex D constrains a clock_timestamp against its predecessor only
        // when the current entry does not declare a discontinuity. The current
        // value still becomes the baseline for a following continuous entry.
        if (sawPresentTimestamp && !discontinuity) {
            if (!previousTimestampComparable || !currentTimestampComparable) {
                orderingUnavailable = true;
            } else if (currentClockTimestamp < previousClockTimestamp) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
        }
        sawPresentTimestamp = true;
        previousTimestampComparable = currentTimestampComparable;
        if (currentTimestampComparable) previousClockTimestamp = currentClockTimestamp;

        bool mappingRepresentable = nuitFieldBased;
        bool dropFrameScheme = false;
        switch (countingType) {
        case 0:
        case 1:
            if (countDropped) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            break;
        case 2:
            if (countDropped && frames != 1) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            mappingRepresentable = false;
            break;
        case 3:
            if (countDropped && frames != 0) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            mappingRepresentable = false;
            break;
        case 4:
            dropFrameScheme = true;
            if (countDropped) {
                if (!effectiveSecondsPresent || !effectiveMinutesPresent) {
                    mappingRepresentable = false;
                } else if (frames != 2 || effectiveSeconds != 0 || effectiveMinutes % 10 == 0) {
                    result.status = TimecodeParseStatus::Malformed;
                    return result;
                }
            }
            // Table D-3's two-label skip maps exactly to SMPTE 29.97 DF only.
            mappingRepresentable =
                mappingRepresentable && normalizedRateEquals(syntax.frameRate, 30000, 1001);
            break;
        case 5:
        case 6:
            mappingRepresentable = false;
            break;
        }
        if (!mappingRepresentable) unsupportedMapping = true;
        if (!effectiveTimestampComplete) continue;
        if (!mappingRepresentable) continue;

        const Smpte12mTimecode timestamp{int(effectiveHours),   int(effectiveMinutes),
                                         int(effectiveSeconds), int(frames),
                                         dropFrameScheme,       true};
        if (!validateTimecodeLabel(timestamp, syntax.frameRate)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (!syntax.fixedFrameRate) {
            unsupportedMapping = true;
            continue;
        }
        if (!firstUsableTimestamp.valid) firstUsableTimestamp = timestamp;
    }
    if (!reader.seiPayloadAlignmentBits()) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }
    if (unsupportedMapping || (orderingUnavailable && firstUsableTimestamp.valid)) {
        result.status = TimecodeParseStatus::Unsupported;
        return result;
    }
    if (firstUsableTimestamp.valid) {
        result.timecode = firstUsableTimestamp;
        result.status = TimecodeParseStatus::Valid;
    }
    return result;
}

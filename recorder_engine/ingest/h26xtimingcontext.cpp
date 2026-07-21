#include "h26xtimingcontext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <numeric>
#include <optional>

namespace {

uint64_t nextTimingContextIdentity() {
    static std::atomic<uint64_t> next{1};
    uint64_t identity = next.fetch_add(1, std::memory_order_relaxed);
    if (identity == 0) identity = next.fetch_add(1, std::memory_order_relaxed);
    return identity;
}

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
        while ((m_bitOffset & 7) != 0) {
            bool paddingBit = false;
            if (!bit(paddingBit) || paddingBit) return false;
        }
        return m_bitOffset == bitCount;
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
    if (nal.size() < 4 || (uchar(nal[0]) & 0x80u) != 0 || (uchar(nal[0]) & 0x60u) == 0 ||
        (uchar(nal[0]) & 0x1fu) != 7) {
        return malformedH264();
    }

    QByteArray rbsp;
    if (!H26xTimingDetail::unescapeRbsp(nal.mid(1), rbsp)) return malformedH264();
    BitReader reader(rbsp);
    uint32_t profileIdc = 0;
    uint32_t constraintFlags = 0;
    uint32_t ignored = 0;
    uint32_t chromaFormatIdc = 1;
    uint32_t sequenceParameterSetId = 0;
    if (!reader.bits(8, profileIdc) || !reader.bits(8, constraintFlags) ||
        (constraintFlags & 0x03u) != 0 || !reader.bits(8, ignored) ||
        !reader.ue(sequenceParameterSetId) || sequenceParameterSetId > 31) {
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

    uint32_t log2MaxFrameNumMinus4 = 0;
    uint32_t picOrderCntType = 0;
    if (!reader.ue(log2MaxFrameNumMinus4) || log2MaxFrameNumMinus4 > 12 ||
        !reader.ue(picOrderCntType) || picOrderCntType > 2) {
        return malformedH264();
    }
    if (picOrderCntType == 0) {
        uint32_t log2MaxPicOrderCntLsbMinus4 = 0;
        if (!reader.ue(log2MaxPicOrderCntLsbMinus4) || log2MaxPicOrderCntLsbMinus4 > 12)
            return malformedH264();
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
    if (!vuiPresent) {
        if (!reader.rbspTrailingBits()) return malformedH264();
        return syntax;
    }

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

HevcTimingSyntax malformedHevc() {
    HevcTimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Malformed;
    return syntax;
}

HevcTimingSyntax unsupportedHevc() {
    HevcTimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Unsupported;
    return syntax;
}

struct HevcProfileTierLevel {
    bool progressiveSource = false;
    bool interlacedSource = false;
};

bool parseHevcProfileTierLevel(BitReader& reader, uint32_t maxSubLayersMinus1,
                               HevcProfileTierLevel& syntax) {
    uint32_t ignored = 0;
    bool flag = false;
    if (!reader.bits(2, ignored) || !reader.bit(flag) || !reader.bits(5, ignored) ||
        !reader.bits(32, ignored) || !reader.bit(syntax.progressiveSource) ||
        !reader.bit(syntax.interlacedSource) || !reader.bit(flag) || !reader.bit(flag) ||
        !reader.bits(32, ignored) || !reader.bits(12, ignored) || !reader.bits(8, ignored)) {
        return false;
    }
    bool profilePresent[7]{};
    bool levelPresent[7]{};
    for (uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
        if (!reader.bit(profilePresent[i]) || !reader.bit(levelPresent[i])) return false;
    }
    if (maxSubLayersMinus1 > 0) {
        for (uint32_t i = maxSubLayersMinus1; i < 8; ++i) {
            if (!reader.bits(2, ignored) || ignored != 0) return false;
        }
    }
    for (uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
        if (profilePresent[i] &&
            (!reader.bits(2, ignored) || !reader.bit(flag) || !reader.bits(5, ignored) ||
             !reader.bits(32, ignored) || !reader.bits(4, ignored) || !reader.bits(32, ignored) ||
             !reader.bits(12, ignored))) {
            return false;
        }
        if (levelPresent[i] && !reader.bits(8, ignored)) return false;
    }
    return true;
}

FrameRateQ reducedRate(uint64_t numerator, uint64_t denominator) {
    if (numerator == 0 || denominator == 0) return {};
    const uint64_t divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (numerator > uint64_t(std::numeric_limits<int32_t>::max()) ||
        denominator > uint64_t(std::numeric_limits<int32_t>::max()) ||
        numerator < denominator * 12u || numerator > denominator * 240u) {
        return {};
    }
    return FrameRateQ{int32_t(numerator), int32_t(denominator)};
}

bool skipHevcSubLayerHrd(BitReader& reader, uint32_t cpbCountMinus1, bool subPicPresent) {
    bool flag = false;
    uint32_t ignored = 0;
    for (uint32_t i = 0; i <= cpbCountMinus1; ++i) {
        if (!reader.ue(ignored) || !reader.ue(ignored) ||
            (subPicPresent && (!reader.ue(ignored) || !reader.ue(ignored))) || !reader.bit(flag)) {
            return false;
        }
    }
    return true;
}

struct HevcHrdSyntax {
    bool nalHrdPresent = false;
    bool vclHrdPresent = false;
    bool subPicPresent = false;
    bool subPicCpbParamsInPicTimingSei = false;
    bool fixedPicRateWithinCvsKnown = false;
    bool fixedPicRateWithinCvs = true;
    uint8_t auCpbRemovalDelayLength = 0;
    uint8_t dpbOutputDelayLength = 0;
    uint8_t dpbOutputDelayDuLength = 0;
    uint8_t duCpbRemovalDelayIncrementLength = 0;
};

void applyHevcHrdSyntax(HevcTimingSyntax& syntax, const HevcHrdSyntax& hrd) {
    syntax.cpbDpbDelaysPresent = hrd.nalHrdPresent || hrd.vclHrdPresent;
    syntax.subPicHrdParamsPresent = hrd.subPicPresent;
    syntax.subPicCpbParamsInPicTimingSei = hrd.subPicCpbParamsInPicTimingSei;
    syntax.fixedPicRateWithinCvsKnown = hrd.fixedPicRateWithinCvsKnown;
    syntax.fixedPicRateWithinCvs = hrd.fixedPicRateWithinCvs;
    syntax.auCpbRemovalDelayLength = hrd.auCpbRemovalDelayLength;
    syntax.dpbOutputDelayLength = hrd.dpbOutputDelayLength;
    syntax.dpbOutputDelayDuLength = hrd.dpbOutputDelayDuLength;
    syntax.duCpbRemovalDelayIncrementLength = hrd.duCpbRemovalDelayIncrementLength;
}

bool skipHevcHrd(BitReader& reader, bool commonInformationPresent, uint32_t maxSubLayersMinus1,
                 HevcHrdSyntax& syntax) {
    uint32_t ignored = 0;
    if (commonInformationPresent) {
        syntax = {};
        if (!reader.bit(syntax.nalHrdPresent) || !reader.bit(syntax.vclHrdPresent)) return false;
        if (syntax.nalHrdPresent || syntax.vclHrdPresent) {
            if (!reader.bit(syntax.subPicPresent)) return false;
            if (syntax.subPicPresent) {
                uint32_t delayIncrementLengthMinus1 = 0;
                uint32_t outputDuLengthMinus1 = 0;
                if (!reader.bits(8, ignored) || !reader.bits(5, delayIncrementLengthMinus1) ||
                    !reader.bit(syntax.subPicCpbParamsInPicTimingSei) ||
                    !reader.bits(5, outputDuLengthMinus1)) {
                    return false;
                }
                syntax.duCpbRemovalDelayIncrementLength = uint8_t(delayIncrementLengthMinus1 + 1);
                syntax.dpbOutputDelayDuLength = uint8_t(outputDuLengthMinus1 + 1);
            }
            uint32_t auDelayLengthMinus1 = 0;
            uint32_t dpbDelayLengthMinus1 = 0;
            if (!reader.bits(4, ignored) || !reader.bits(4, ignored) ||
                (syntax.subPicPresent && !reader.bits(4, ignored)) || !reader.bits(5, ignored) ||
                !reader.bits(5, auDelayLengthMinus1) || !reader.bits(5, dpbDelayLengthMinus1)) {
                return false;
            }
            syntax.auCpbRemovalDelayLength = uint8_t(auDelayLengthMinus1 + 1);
            syntax.dpbOutputDelayLength = uint8_t(dpbDelayLengthMinus1 + 1);
        }
    }
    syntax.fixedPicRateWithinCvsKnown = true;
    for (uint32_t i = 0; i <= maxSubLayersMinus1; ++i) {
        bool fixedGeneral = false;
        bool fixedWithin = true;
        bool lowDelay = false;
        if (!reader.bit(fixedGeneral)) return false;
        if (!fixedGeneral && !reader.bit(fixedWithin)) return false;
        syntax.fixedPicRateWithinCvs = syntax.fixedPicRateWithinCvs && fixedWithin;
        if (fixedWithin) {
            if (!reader.ue(ignored)) return false;
        } else if (!reader.bit(lowDelay)) {
            return false;
        }
        uint32_t cpbCountMinus1 = 0;
        if (!lowDelay && (!reader.ue(cpbCountMinus1) || cpbCountMinus1 > 31)) return false;
        if (syntax.nalHrdPresent &&
            !skipHevcSubLayerHrd(reader, cpbCountMinus1, syntax.subPicPresent))
            return false;
        if (syntax.vclHrdPresent &&
            !skipHevcSubLayerHrd(reader, cpbCountMinus1, syntax.subPicPresent))
            return false;
    }
    return true;
}

HevcTimingSyntax parseHevcVps(const QByteArray& parameterSet) {
    const QByteArray nal = removeAnnexBPrefix(parameterSet);
    if (nal.size() < 4 || (uchar(nal[0]) & 0x80u) != 0 || ((uchar(nal[0]) >> 1) & 0x3fu) != 32 ||
        (((uchar(nal[0]) & 0x01u) << 5) | (uchar(nal[1]) >> 3)) != 0 ||
        (uchar(nal[1]) & 0x07u) != 1) {
        return malformedHevc();
    }
    QByteArray rbsp;
    if (!H26xTimingDetail::unescapeRbsp(nal.mid(2), rbsp)) return malformedHevc();
    BitReader reader(rbsp);
    uint32_t ignored = 0;
    uint32_t vpsId = 0;
    uint32_t maxLayersMinus1 = 0;
    uint32_t maxSubLayersMinus1 = 0;
    bool baseLayerInternal = false;
    bool baseLayerAvailable = false;
    bool temporalIdNesting = false;
    HevcProfileTierLevel profileTierLevel;
    if (!reader.bits(4, vpsId) || !reader.bit(baseLayerInternal) ||
        !reader.bit(baseLayerAvailable) || !reader.bits(6, maxLayersMinus1) ||
        !reader.bits(3, maxSubLayersMinus1) || maxSubLayersMinus1 > 6 ||
        !reader.bit(temporalIdNesting) || (!baseLayerInternal && maxLayersMinus1 == 0) ||
        (maxSubLayersMinus1 == 0 && !temporalIdNesting) || !reader.bits(16, ignored) ||
        ignored != 0xffffu ||
        !parseHevcProfileTierLevel(reader, maxSubLayersMinus1, profileTierLevel)) {
        return malformedHevc();
    }
    // Multilayer VPS extensions change parameter-set inference rules. Do not
    // guess at them in the base-layer ingest parser.
    if (maxLayersMinus1 != 0) return unsupportedHevc();
    // This parser consumes only the base layer. A conforming VPS may describe
    // an unavailable base layer, but such timing cannot authorize base-layer
    // ingest decisions.
    if (!baseLayerAvailable) return unsupportedHevc();

    bool subLayerOrderingInfoPresent = false;
    if (!reader.bit(subLayerOrderingInfoPresent)) return malformedHevc();
    const uint32_t firstLayer = subLayerOrderingInfoPresent ? 0 : maxSubLayersMinus1;
    for (uint32_t i = firstLayer; i <= maxSubLayersMinus1; ++i) {
        uint32_t buffering = 0;
        uint32_t reorder = 0;
        uint32_t latency = 0;
        if (!reader.ue(buffering) || !reader.ue(reorder) || !reader.ue(latency) || buffering > 16 ||
            reorder > buffering) {
            return malformedHevc();
        }
    }
    uint32_t maxLayerId = 0;
    uint32_t layerSetCountMinus1 = 0;
    if (!reader.bits(6, maxLayerId) || maxLayerId != 0 || !reader.ue(layerSetCountMinus1))
        return malformedHevc();
    if (layerSetCountMinus1 != 0) return unsupportedHevc();

    HevcTimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.vpsId = uint8_t(vpsId);
    syntax.maxSubLayersMinus1 = uint8_t(maxSubLayersMinus1);
    syntax.temporalIdNesting = temporalIdNesting;
    syntax.generalProgressiveSource = profileTierLevel.progressiveSource;
    syntax.generalInterlacedSource = profileTierLevel.interlacedSource;
    if (!reader.bit(syntax.timingInfoPresent)) return malformedHevc();
    if (syntax.timingInfoPresent) {
        if (!reader.bits(32, syntax.numUnitsInTick) || !reader.bits(32, syntax.timeScale) ||
            syntax.numUnitsInTick == 0 || syntax.timeScale == 0 ||
            !reader.bit(syntax.pocProportionalToTiming)) {
            return malformedHevc();
        }
        if (syntax.pocProportionalToTiming) {
            uint32_t ticksMinus1 = 0;
            if (!reader.ue(ticksMinus1) || ticksMinus1 == std::numeric_limits<uint32_t>::max())
                return malformedHevc();
            syntax.numTicksPocDiffOne = ticksMinus1 + 1;
            syntax.frameRate = reducedRate(syntax.timeScale, uint64_t(syntax.numUnitsInTick) *
                                                                 syntax.numTicksPocDiffOne);
        }
        uint32_t hrdCount = 0;
        if (!reader.ue(hrdCount)) return malformedHevc();
        if (hrdCount > uint64_t(layerSetCountMinus1) + 1) return malformedHevc();
        HevcHrdSyntax hrdSyntax;
        bool sawBaseLayerSetHrd = false;
        for (uint32_t i = 0; i < hrdCount; ++i) {
            bool commonInformationPresent = i == 0;
            uint32_t hrdLayerSetIndex = 0;
            if (!reader.ue(hrdLayerSetIndex) || hrdLayerSetIndex > layerSetCountMinus1 ||
                (i == 0 && hrdLayerSetIndex != 0) ||
                (hrdLayerSetIndex == 0 && sawBaseLayerSetHrd) ||
                (i > 0 && !reader.bit(commonInformationPresent)) ||
                !skipHevcHrd(reader, commonInformationPresent, maxSubLayersMinus1, hrdSyntax)) {
                return malformedHevc();
            }
            if (hrdLayerSetIndex == 0) sawBaseLayerSetHrd = true;
        }
        if (hrdCount != 0) applyHevcHrdSyntax(syntax, hrdSyntax);
    }
    bool extensionFlag = false;
    if (!reader.bit(extensionFlag)) return malformedHevc();
    if (extensionFlag) return unsupportedHevc();
    if (!reader.rbspTrailingBits()) return malformedHevc();
    return syntax;
}

bool skipHevcScalingList(BitReader& reader) {
    for (int sizeId = 0; sizeId < 4; ++sizeId) {
        for (int matrixId = 0; matrixId < 6; matrixId += sizeId == 3 ? 3 : 1) {
            bool predMode = false;
            if (!reader.bit(predMode)) return false;
            if (!predMode) {
                uint32_t delta = 0;
                if (!reader.ue(delta)) return false;
            } else {
                const int coefficientCount = std::min(64, 1 << (4 + (sizeId << 1)));
                int32_t ignored = 0;
                if (sizeId > 1 && !reader.se(ignored)) return false;
                for (int i = 0; i < coefficientCount; ++i) {
                    if (!reader.se(ignored)) return false;
                }
            }
        }
    }
    return true;
}

bool skipHevcShortTermRefPicSets(BitReader& reader, uint32_t setCount) {
    if (setCount > 64) return false;
    uint32_t deltaPocCount[64]{};
    for (uint32_t setIndex = 0; setIndex < setCount; ++setIndex) {
        bool predicted = false;
        if (setIndex != 0 && !reader.bit(predicted)) return false;
        if (predicted) {
            bool flag = false;
            uint32_t ignored = 0;
            if (!reader.bit(flag) || !reader.ue(ignored)) return false;
            const uint32_t referenceCount = deltaPocCount[setIndex - 1];
            uint32_t included = 0;
            for (uint32_t j = 0; j <= referenceCount; ++j) {
                bool used = false;
                bool useDelta = true;
                if (!reader.bit(used) || (!used && !reader.bit(useDelta))) return false;
                if (used || useDelta) ++included;
            }
            deltaPocCount[setIndex] = included;
        } else {
            uint32_t negative = 0;
            uint32_t positive = 0;
            if (!reader.ue(negative) || !reader.ue(positive) || negative > 16 || positive > 16)
                return false;
            bool flag = false;
            uint32_t ignored = 0;
            for (uint32_t j = 0; j < negative + positive; ++j) {
                if (!reader.ue(ignored) || !reader.bit(flag)) return false;
            }
            deltaPocCount[setIndex] = negative + positive;
        }
    }
    return true;
}

HevcTimingSyntax parseHevcSps(const QByteArray& parameterSet) {
    const QByteArray nal = removeAnnexBPrefix(parameterSet);
    if (nal.size() < 4 || (uchar(nal[0]) & 0x80u) != 0 || ((uchar(nal[0]) >> 1) & 0x3fu) != 33 ||
        (((uchar(nal[0]) & 0x01u) << 5) | (uchar(nal[1]) >> 3)) != 0 ||
        (uchar(nal[1]) & 0x07u) != 1) {
        return malformedHevc();
    }
    QByteArray rbsp;
    if (!H26xTimingDetail::unescapeRbsp(nal.mid(2), rbsp)) return malformedHevc();
    BitReader reader(rbsp);
    uint32_t ignored = 0;
    uint32_t referencedVpsId = 0;
    uint32_t maxSubLayersMinus1 = 0;
    bool temporalIdNesting = false;
    bool flag = false;
    HevcProfileTierLevel profileTierLevel;
    if (!reader.bits(4, referencedVpsId) || !reader.bits(3, maxSubLayersMinus1) ||
        maxSubLayersMinus1 > 6 || !reader.bit(temporalIdNesting) ||
        (maxSubLayersMinus1 == 0 && !temporalIdNesting) ||
        !parseHevcProfileTierLevel(reader, maxSubLayersMinus1, profileTierLevel)) {
        return malformedHevc();
    }
    uint32_t spsId = 0;
    uint32_t chromaFormat = 0;
    if (!reader.ue(spsId) || spsId > 15 || !reader.ue(chromaFormat) || chromaFormat > 3)
        return malformedHevc();
    if (chromaFormat == 3 && !reader.bit(flag)) return malformedHevc();
    uint32_t width = 0;
    uint32_t height = 0;
    if (!reader.ue(width) || !reader.ue(height) || width == 0 || height == 0 || !reader.bit(flag))
        return malformedHevc();
    if (flag) {
        for (int i = 0; i < 4; ++i) {
            if (!reader.ue(ignored)) return malformedHevc();
        }
    }
    uint32_t bitDepthLuma = 0;
    uint32_t bitDepthChroma = 0;
    uint32_t log2MaxPocMinus4 = 0;
    if (!reader.ue(bitDepthLuma) || !reader.ue(bitDepthChroma) || bitDepthLuma > 8 ||
        bitDepthChroma > 8 || !reader.ue(log2MaxPocMinus4) || log2MaxPocMinus4 > 12)
        return malformedHevc();

    bool orderingPresent = false;
    if (!reader.bit(orderingPresent)) return malformedHevc();
    const uint32_t firstLayer = orderingPresent ? 0 : maxSubLayersMinus1;
    for (uint32_t i = firstLayer; i <= maxSubLayersMinus1; ++i) {
        uint32_t buffering = 0;
        uint32_t reorder = 0;
        uint32_t latency = 0;
        if (!reader.ue(buffering) || !reader.ue(reorder) || !reader.ue(latency) || buffering > 16 ||
            reorder > buffering) {
            return malformedHevc();
        }
    }
    const int ueFieldCount = 6;
    for (int i = 0; i < ueFieldCount; ++i) {
        if (!reader.ue(ignored)) return malformedHevc();
    }
    bool scalingListEnabled = false;
    bool scalingListPresent = false;
    if (!reader.bit(scalingListEnabled) ||
        (scalingListEnabled && (!reader.bit(scalingListPresent) ||
                                (scalingListPresent && !skipHevcScalingList(reader))))) {
        return malformedHevc();
    }
    bool pcmEnabled = false;
    if (!reader.bit(flag) || !reader.bit(flag) || !reader.bit(pcmEnabled)) return malformedHevc();
    if (pcmEnabled) {
        if (!reader.bits(4, ignored) || !reader.bits(4, ignored) || !reader.ue(ignored) ||
            !reader.ue(ignored) || !reader.bit(flag)) {
            return malformedHevc();
        }
    }
    uint32_t shortTermSetCount = 0;
    if (!reader.ue(shortTermSetCount) || !skipHevcShortTermRefPicSets(reader, shortTermSetCount)) {
        return malformedHevc();
    }
    bool longTermPresent = false;
    if (!reader.bit(longTermPresent)) return malformedHevc();
    if (longTermPresent) {
        uint32_t longTermCount = 0;
        if (!reader.ue(longTermCount) || longTermCount > 32) return malformedHevc();
        for (uint32_t i = 0; i < longTermCount; ++i) {
            if (!reader.bits(int(log2MaxPocMinus4 + 4), ignored) || !reader.bit(flag))
                return malformedHevc();
        }
    }
    if (!reader.bit(flag) || !reader.bit(flag)) return malformedHevc();

    bool vuiPresent = false;
    if (!reader.bit(vuiPresent)) return malformedHevc();
    HevcTimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.generalProgressiveSource = profileTierLevel.progressiveSource;
    syntax.generalInterlacedSource = profileTierLevel.interlacedSource;
    syntax.referencedVpsId = uint8_t(referencedVpsId);
    syntax.spsId = uint8_t(spsId);
    syntax.maxSubLayersMinus1 = uint8_t(maxSubLayersMinus1);
    syntax.temporalIdNesting = temporalIdNesting;
    if (!vuiPresent) {
        syntax.frameFieldInfoPresent =
            syntax.generalProgressiveSource && syntax.generalInterlacedSource;
        bool extensionPresent = false;
        if (!reader.bit(extensionPresent)) return malformedHevc();
        if (extensionPresent) return unsupportedHevc();
        if (!reader.rbspTrailingBits()) return malformedHevc();
        return syntax;
    }

    bool present = false;
    if (!reader.bit(present)) return malformedHevc();
    if (present) {
        uint32_t aspectRatioIdc = 0;
        if (!reader.bits(8, aspectRatioIdc) ||
            (aspectRatioIdc == 255 && (!reader.bits(16, ignored) || !reader.bits(16, ignored)))) {
            return malformedHevc();
        }
    }
    if (!reader.bit(present) || (present && !reader.bit(flag)) || !reader.bit(present))
        return malformedHevc();
    if (present) {
        bool colourDescription = false;
        if (!reader.bits(3, ignored) || !reader.bit(flag) || !reader.bit(colourDescription))
            return malformedHevc();
        if (colourDescription &&
            (!reader.bits(8, ignored) || !reader.bits(8, ignored) || !reader.bits(8, ignored))) {
            return malformedHevc();
        }
    }
    if (!reader.bit(present)) return malformedHevc();
    if (present && (!reader.ue(ignored) || !reader.ue(ignored))) return malformedHevc();
    if (!reader.bit(flag) || !reader.bit(syntax.fieldSeq) ||
        !reader.bit(syntax.frameFieldInfoPresent) || !reader.bit(present)) {
        return malformedHevc();
    }
    if (!syntax.frameFieldInfoPresent &&
        (syntax.fieldSeq || (syntax.generalProgressiveSource && syntax.generalInterlacedSource))) {
        return malformedHevc();
    }
    if (present) {
        for (int i = 0; i < 4; ++i) {
            if (!reader.ue(ignored)) return malformedHevc();
        }
    }
    if (!reader.bit(syntax.timingInfoPresent)) return malformedHevc();
    if (syntax.timingInfoPresent) {
        if (!reader.bits(32, syntax.numUnitsInTick) || !reader.bits(32, syntax.timeScale) ||
            syntax.numUnitsInTick == 0 || syntax.timeScale == 0 ||
            !reader.bit(syntax.pocProportionalToTiming)) {
            return malformedHevc();
        }
        if (syntax.pocProportionalToTiming) {
            uint32_t ticksMinus1 = 0;
            if (!reader.ue(ticksMinus1) || ticksMinus1 == std::numeric_limits<uint32_t>::max())
                return malformedHevc();
            syntax.numTicksPocDiffOne = ticksMinus1 + 1;
            syntax.frameRate = reducedRate(syntax.timeScale, uint64_t(syntax.numUnitsInTick) *
                                                                 syntax.numTicksPocDiffOne);
        }
        bool hrdPresent = false;
        if (!reader.bit(hrdPresent)) return malformedHevc();
        if (hrdPresent) {
            HevcHrdSyntax hrdSyntax;
            if (!skipHevcHrd(reader, true, maxSubLayersMinus1, hrdSyntax)) return malformedHevc();
            applyHevcHrdSyntax(syntax, hrdSyntax);
        }
    }
    bool bitstreamRestriction = false;
    if (!reader.bit(bitstreamRestriction)) return malformedHevc();
    if (bitstreamRestriction) {
        if (!reader.bit(flag) || !reader.bit(flag) || !reader.bit(flag) || !reader.ue(ignored) ||
            !reader.ue(ignored) || !reader.ue(ignored) || !reader.ue(ignored) ||
            !reader.ue(ignored)) {
            return malformedHevc();
        }
    }
    bool extensionPresent = false;
    if (!reader.bit(extensionPresent)) return malformedHevc();
    if (extensionPresent) return unsupportedHevc();
    if (!reader.rbspTrailingBits()) return malformedHevc();
    return syntax;
}

bool mergeHevcTiming(HevcTimingSyntax& base, const HevcTimingSyntax& sps) {
    if (sps.status != H26xTimingSyntaxStatus::Valid) return false;
    base.fieldSeq = sps.fieldSeq;
    base.frameFieldInfoPresent = sps.frameFieldInfoPresent;
    // Picture-level source_scan_type is constrained by the profile-tier-level
    // that applies through the active SPS (H.265 D.3.3).
    base.generalProgressiveSource = sps.generalProgressiveSource;
    base.generalInterlacedSource = sps.generalInterlacedSource;
    if (sps.fixedPicRateWithinCvsKnown) {
        base.cpbDpbDelaysPresent = sps.cpbDpbDelaysPresent;
        base.subPicHrdParamsPresent = sps.subPicHrdParamsPresent;
        base.subPicCpbParamsInPicTimingSei = sps.subPicCpbParamsInPicTimingSei;
        base.fixedPicRateWithinCvsKnown = true;
        base.fixedPicRateWithinCvs = sps.fixedPicRateWithinCvs;
        base.auCpbRemovalDelayLength = sps.auCpbRemovalDelayLength;
        base.dpbOutputDelayLength = sps.dpbOutputDelayLength;
        base.dpbOutputDelayDuLength = sps.dpbOutputDelayDuLength;
        base.duCpbRemovalDelayIncrementLength = sps.duCpbRemovalDelayIncrementLength;
    }
    if (!sps.timingInfoPresent) return true;
    if (base.timingInfoPresent &&
        (base.numUnitsInTick != sps.numUnitsInTick || base.timeScale != sps.timeScale ||
         base.pocProportionalToTiming != sps.pocProportionalToTiming ||
         base.numTicksPocDiffOne != sps.numTicksPocDiffOne)) {
        return false;
    }
    base.timingInfoPresent = true;
    base.numUnitsInTick = sps.numUnitsInTick;
    base.timeScale = sps.timeScale;
    base.pocProportionalToTiming = sps.pocProportionalToTiming;
    base.numTicksPocDiffOne = sps.numTicksPocDiffOne;
    base.frameRate = sps.frameRate;
    return true;
}

bool equivalentTiming(const HevcTimingSyntax& lhs, const HevcTimingSyntax& rhs) {
    return lhs.status == rhs.status && lhs.frameRate == rhs.frameRate &&
           lhs.numUnitsInTick == rhs.numUnitsInTick && lhs.timeScale == rhs.timeScale &&
           lhs.numTicksPocDiffOne == rhs.numTicksPocDiffOne &&
           lhs.timingInfoPresent == rhs.timingInfoPresent &&
           lhs.pocProportionalToTiming == rhs.pocProportionalToTiming &&
           lhs.fieldSeq == rhs.fieldSeq && lhs.frameFieldInfoPresent == rhs.frameFieldInfoPresent &&
           lhs.generalProgressiveSource == rhs.generalProgressiveSource &&
           lhs.generalInterlacedSource == rhs.generalInterlacedSource &&
           lhs.cpbDpbDelaysPresent == rhs.cpbDpbDelaysPresent &&
           lhs.subPicHrdParamsPresent == rhs.subPicHrdParamsPresent &&
           lhs.subPicCpbParamsInPicTimingSei == rhs.subPicCpbParamsInPicTimingSei &&
           lhs.fixedPicRateWithinCvsKnown == rhs.fixedPicRateWithinCvsKnown &&
           lhs.fixedPicRateWithinCvs == rhs.fixedPicRateWithinCvs &&
           lhs.auCpbRemovalDelayLength == rhs.auCpbRemovalDelayLength &&
           lhs.dpbOutputDelayLength == rhs.dpbOutputDelayLength &&
           lhs.dpbOutputDelayDuLength == rhs.dpbOutputDelayDuLength &&
           lhs.duCpbRemovalDelayIncrementLength == rhs.duCpbRemovalDelayIncrementLength &&
           lhs.maxSubLayersMinus1 == rhs.maxSubLayersMinus1 &&
           lhs.temporalIdNesting == rhs.temporalIdNesting;
}

FrameRateQ hevcLabelRate(const HevcTimingSyntax& syntax, bool unitsFieldBased) {
    if (!syntax.timingInfoPresent || syntax.numUnitsInTick == 0 || syntax.timeScale == 0) return {};
    uint64_t numerator = syntax.timeScale;
    uint64_t denominator = uint64_t(syntax.numUnitsInTick) * (1u + uint64_t(unitsFieldBased));
    const uint64_t divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (numerator > uint64_t(std::numeric_limits<int32_t>::max()) ||
        denominator > uint64_t(std::numeric_limits<int32_t>::max()) ||
        numerator < denominator * 12u || numerator > denominator * 240u) {
        return {};
    }
    return FrameRateQ{int32_t(numerator), int32_t(denominator)};
}

bool validHevcFrameCount(uint32_t frames, FrameRateQ rate) {
    if (!rate.valid()) return false;
    return int64_t(frames) < (int64_t(rate.num) + rate.den - 1) / rate.den;
}

bool hevcClockTimestamp(uint32_t hours, uint32_t minutes, uint32_t seconds, uint32_t frames,
                        bool unitsFieldBased, int32_t timeOffset, const HevcTimingSyntax& syntax,
                        int64_t& result) {
    if (!syntax.timingInfoPresent || syntax.numUnitsInTick == 0 || syntax.timeScale == 0)
        return false;
    int64_t clockTimestamp = 0;
    if (!checkedMultiplyNonnegative(hours, 60, clockTimestamp) ||
        !checkedAdd(clockTimestamp, minutes, clockTimestamp) ||
        !checkedMultiplyNonnegative(clockTimestamp, 60, clockTimestamp) ||
        !checkedAdd(clockTimestamp, seconds, clockTimestamp) ||
        !checkedMultiplyNonnegative(clockTimestamp, syntax.timeScale, clockTimestamp)) {
        return false;
    }
    int64_t frameTicks = 0;
    if (!checkedMultiplyNonnegative(syntax.numUnitsInTick, 1 + int64_t(unitsFieldBased),
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

H26xTimingContext::H26xTimingContext() : m_identity(nextTimingContextIdentity()) {}

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
        if (codec == NativeVideoCodec::Hevc) return m_hevc.status == H26xTimingSyntaxStatus::Valid;
        return false;
    }

    m_codec = codec;
    m_vps = codec == NativeVideoCodec::H264 ? QList<QByteArray>{} : vps;
    m_sps = sps;
    if (m_generation != std::numeric_limits<uint64_t>::max()) ++m_generation;
    m_h264 = H264TimingSyntax{};
    m_hevc = HevcTimingSyntax{};

    if (codec == NativeVideoCodec::Hevc) {
        if (vps.isEmpty()) return false;
        std::array<std::optional<HevcTimingSyntax>, 16> vpsById;
        bool sawMalformed = false;
        bool sawUnsupported = false;
        bool sawConflictingId = false;
        for (const QByteArray& parameterSet : vps) {
            const HevcTimingSyntax candidate = parseHevcVps(parameterSet);
            if (candidate.status == H26xTimingSyntaxStatus::Malformed) {
                sawMalformed = true;
            } else if (candidate.status == H26xTimingSyntaxStatus::Unsupported) {
                sawUnsupported = true;
            } else {
                auto& retained = vpsById[candidate.vpsId];
                if (!retained.has_value()) {
                    retained = candidate;
                } else if (!equivalentTiming(*retained, candidate)) {
                    sawConflictingId = true;
                }
            }
        }
        if (sawMalformed) {
            m_hevc = malformedHevc();
            return false;
        }
        if (sawUnsupported || sawConflictingId) {
            m_hevc = unsupportedHevc();
            return false;
        }

        HevcTimingSyntax merged;
        bool haveSps = false;
        HevcTimingSyntax firstMerged;
        std::array<std::optional<uint8_t>, 16> spsReferences;
        QList<HevcTimingSyntax> validSps;
        bool sawMalformedSps = false;
        bool sawUnsupportedSps = false;
        for (const QByteArray& parameterSet : sps) {
            const HevcTimingSyntax candidate = parseHevcSps(parameterSet);
            if (candidate.status == H26xTimingSyntaxStatus::Malformed) {
                sawMalformedSps = true;
            } else if (candidate.status == H26xTimingSyntaxStatus::Unsupported) {
                sawUnsupportedSps = true;
            } else {
                validSps.append(candidate);
            }
        }
        if (sawMalformedSps) {
            m_hevc = malformedHevc();
            return false;
        }
        if (sawUnsupportedSps) {
            m_hevc = unsupportedHevc();
            return false;
        }
        for (const HevcTimingSyntax& candidate : validSps) {
            const auto& referencedVps = vpsById[candidate.referencedVpsId];
            if (!referencedVps.has_value()) {
                m_hevc = unsupportedHevc();
                return false;
            }
            if (candidate.maxSubLayersMinus1 > referencedVps->maxSubLayersMinus1 ||
                (referencedVps->temporalIdNesting && !candidate.temporalIdNesting)) {
                m_hevc = malformedHevc();
                return false;
            }
            auto& retainedReference = spsReferences[candidate.spsId];
            if (retainedReference.has_value() && *retainedReference != candidate.referencedVpsId) {
                m_hevc = unsupportedHevc();
                return false;
            }
            retainedReference = candidate.referencedVpsId;

            HevcTimingSyntax candidateMerged = *referencedVps;
            if (!mergeHevcTiming(candidateMerged, candidate)) {
                m_hevc = unsupportedHevc();
                return false;
            }
            candidateMerged.referencedVpsId = candidate.referencedVpsId;
            candidateMerged.spsId = candidate.spsId;
            if (!haveSps) {
                firstMerged = candidateMerged;
                haveSps = true;
            } else if (!equivalentTiming(firstMerged, candidateMerged)) {
                m_hevc = unsupportedHevc();
                return false;
            }
        }
        if (haveSps) {
            merged = firstMerged;
        } else {
            const HevcTimingSyntax* soleVps = nullptr;
            for (const auto& candidate : vpsById) {
                if (!candidate.has_value()) continue;
                if (soleVps != nullptr) {
                    m_hevc = unsupportedHevc();
                    return false;
                }
                soleVps = &*candidate;
            }
            if (soleVps == nullptr) {
                m_hevc = unsupportedHevc();
                return false;
            }
            merged = *soleVps;
        }
        m_hevc = merged;
        return true;
    }
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
    return m_codec == NativeVideoCodec::H264 ? m_h264.frameRate : m_hevc.frameRate;
}

bool H26xTimingContext::fixedFrameRate() const {
    if (m_codec == NativeVideoCodec::H264)
        return m_h264.status == H26xTimingSyntaxStatus::Valid && m_h264.fixedFrameRate &&
               m_h264.frameRate.valid();
    return m_codec == NativeVideoCodec::Hevc && m_hevc.status == H26xTimingSyntaxStatus::Valid &&
           m_hevc.pocProportionalToTiming && m_hevc.frameRate.valid();
}

uint64_t H26xTimingContext::generation() const {
    return m_generation;
}

uint64_t H26xTimingContext::identity() const {
    return m_identity;
}

const H264TimingSyntax* H26xTimingContext::h264() const {
    return m_codec == NativeVideoCodec::H264 ? &m_h264 : nullptr;
}

const HevcTimingSyntax* H26xTimingContext::hevc() const {
    return m_codec == NativeVideoCodec::Hevc ? &m_hevc : nullptr;
}

H26xTimingDetail::HevcPictureTimingParseResult
H26xTimingDetail::parseHevcPictureTiming(const QByteArray& payload,
                                         const HevcTimingSyntax& syntax) {
    HevcPictureTimingParseResult result;
    if (syntax.status != H26xTimingSyntaxStatus::Valid) {
        result.status = syntax.status == H26xTimingSyntaxStatus::Malformed
                            ? TimecodeParseStatus::Malformed
                            : TimecodeParseStatus::Unsupported;
        return result;
    }

    BitReader reader(payload);
    uint32_t ignored = 0;
    if (syntax.frameFieldInfoPresent) {
        uint32_t picStruct = 0;
        uint32_t sourceScanType = 0;
        bool duplicate = false;
        if (!reader.bits(4, picStruct) || !reader.bits(2, sourceScanType) ||
            !reader.bit(duplicate) || picStruct > 12 || sourceScanType == 3) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        Q_UNUSED(duplicate);
        const bool sourceScanMatchesProfile =
            (syntax.generalProgressiveSource && syntax.generalInterlacedSource) ||
            (syntax.generalProgressiveSource && sourceScanType == 1) ||
            (syntax.generalInterlacedSource && sourceScanType == 0) ||
            (!syntax.generalProgressiveSource && !syntax.generalInterlacedSource &&
             sourceScanType == 2);
        if (!sourceScanMatchesProfile) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        const bool fieldPicture = picStruct == 1 || picStruct == 2 || picStruct >= 9;
        if (syntax.fieldSeq != fieldPicture) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (picStruct == 7 || picStruct == 8) {
            if (!syntax.fixedPicRateWithinCvsKnown) {
                result.status = TimecodeParseStatus::Unsupported;
                return result;
            }
            if (!syntax.fixedPicRateWithinCvs) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
        }
        result.picStruct = int(picStruct);
        result.sourceScanType = int(sourceScanType);
    }

    if (syntax.cpbDpbDelaysPresent) {
        if (syntax.auCpbRemovalDelayLength == 0 || syntax.dpbOutputDelayLength == 0 ||
            !reader.bits(syntax.auCpbRemovalDelayLength, ignored) ||
            !reader.bits(syntax.dpbOutputDelayLength, ignored)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (syntax.subPicHrdParamsPresent &&
            (syntax.dpbOutputDelayDuLength == 0 ||
             !reader.bits(syntax.dpbOutputDelayDuLength, ignored))) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (syntax.subPicHrdParamsPresent && syntax.subPicCpbParamsInPicTimingSei) {
            uint32_t decodingUnitsMinus1 = 0;
            bool commonDelay = false;
            if (!reader.ue(decodingUnitsMinus1) || decodingUnitsMinus1 > 65535 ||
                !reader.bit(commonDelay)) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            if (commonDelay && (syntax.duCpbRemovalDelayIncrementLength == 0 ||
                                !reader.bits(syntax.duCpbRemovalDelayIncrementLength, ignored))) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            for (uint32_t i = 0; i <= decodingUnitsMinus1; ++i) {
                if (!reader.ue(ignored) ||
                    (!commonDelay && i < decodingUnitsMinus1 &&
                     (syntax.duCpbRemovalDelayIncrementLength == 0 ||
                      !reader.bits(syntax.duCpbRemovalDelayIncrementLength, ignored)))) {
                    result.status = TimecodeParseStatus::Malformed;
                    return result;
                }
            }
        }
    }
    if (!reader.seiPayloadAlignmentBits()) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }
    result.status = TimecodeParseStatus::Valid;
    return result;
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
    bool firstUsableDiscontinuity = false;
    bool unsupportedMapping = false;
    bool orderingUnavailable = false;
    bool sawPresentTimestamp = false;
    bool previousTimestampComparable = false;
    int64_t previousClockTimestamp = 0;
    int previousTimestampIndex = -1;
    uint32_t previousCtType = 0;
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
        // discontinuity_flag governs comparison with the preceding output-order
        // timestamp; it does not relax ordering among entries in this message.

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
        if (sawPresentTimestamp) {
            if (!previousTimestampComparable || !currentTimestampComparable) {
                orderingUnavailable = true;
            } else if (currentClockTimestamp < previousClockTimestamp ||
                       (picStruct >= 3 && picStruct <= 6 &&
                        currentClockTimestamp == previousClockTimestamp &&
                        i == previousTimestampIndex + 1 && (ctType == 1 || previousCtType == 1))) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
        }
        sawPresentTimestamp = true;
        previousTimestampComparable = currentTimestampComparable;
        previousTimestampIndex = i;
        previousCtType = ctType;
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
        default:
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
        if (!firstUsableTimestamp.valid) {
            firstUsableTimestamp = timestamp;
            firstUsableDiscontinuity = discontinuity;
        }
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
        result.labelRate = syntax.frameRate;
        result.provenance = TimecodeProvenance::H264PicTiming;
        result.discontinuity = firstUsableDiscontinuity;
        result.status = TimecodeParseStatus::Valid;
    }
    return result;
}

H26xTimingDetail::TimecodeParseStatus
H26xTimingDetail::validateHevcOutputTransition(const HevcTimeCodeOutput* previous,
                                               const HevcTimeCodeOutput& current) {
    if (!current.present) return TimecodeParseStatus::NoTimestamp;
    if (current.discontinuity) return TimecodeParseStatus::Valid;

    if (previous != nullptr && previous->present) {
        if (!previous->comparable || !current.comparable) return TimecodeParseStatus::Unsupported;
        if (current.clockTimestamp < previous->clockTimestamp)
            return TimecodeParseStatus::Malformed;
    }

    if (!current.countDropped) return TimecodeParseStatus::Valid;
    // D.3.27 qualifies each predecessor restriction with "when present". A
    // first set can therefore signal a conforming drop; only a known previous
    // set can make the transition malformed.
    if (previous == nullptr || !previous->present) return TimecodeParseStatus::Valid;
    switch (current.countingType) {
    case 2:
        return previous->frames == 0 ? TimecodeParseStatus::Malformed : TimecodeParseStatus::Valid;
    case 3:
        return current.maxFps != 0 && previous->frames == current.maxFps - 1
                   ? TimecodeParseStatus::Malformed
                   : TimecodeParseStatus::Valid;
    case 4:
        return previous->frames == 0 || previous->frames == 1 ? TimecodeParseStatus::Malformed
                                                              : TimecodeParseStatus::Valid;
    default:
        return TimecodeParseStatus::Valid;
    }
}

H26xTimingDetail::TimecodeParseResult H26xTimingDetail::parseHevcTimeCode(
    const QByteArray& payload, const HevcTimingSyntax& syntax,
    const HevcTimeCodeContinuity* previous, HevcTimeCodeContinuity* next, int expectedClockCount,
    const HevcTimeCodeOutput* previousOutput, HevcTimeCodeOutput* firstOutput,
    HevcTimeCodeOutput* lastOutput) {
    TimecodeParseResult result;
    result.provenance = TimecodeProvenance::HevcTimeCode;
    if (firstOutput != nullptr) *firstOutput = {};
    if (lastOutput != nullptr) *lastOutput = {};
    if (syntax.status != H26xTimingSyntaxStatus::Valid) {
        result.status = syntax.status == H26xTimingSyntaxStatus::Malformed
                            ? TimecodeParseStatus::Malformed
                            : TimecodeParseStatus::Unsupported;
        return result;
    }

    BitReader reader(payload);
    uint32_t clockCount = 0;
    if (!reader.bits(2, clockCount) || clockCount == 0) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }
    if (syntax.fieldSeq && clockCount != 1) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }
    if (syntax.frameFieldInfoPresent) {
        if (expectedClockCount < 0) {
            result.status = TimecodeParseStatus::Unsupported;
            return result;
        }
        if (clockCount != uint32_t(expectedClockCount)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
    }

    Smpte12mTimecode firstUsable;
    FrameRateQ firstRate;
    bool firstDiscontinuity = false;
    bool unsupportedMapping = false;
    HevcTimeCodeOutput localPreviousOutput;
    const HevcTimeCodeOutput* outputPredecessor = previousOutput;
    bool havePreviousSeconds = previous != nullptr && previous->haveSeconds;
    bool havePreviousMinutes = previous != nullptr && previous->haveMinutes;
    bool havePreviousHours = previous != nullptr && previous->haveHours;
    uint32_t previousSeconds = havePreviousSeconds ? previous->seconds : 0;
    uint32_t previousMinutes = havePreviousMinutes ? previous->minutes : 0;
    uint32_t previousHours = havePreviousHours ? previous->hours : 0;
    bool havePreviousFrameSemantics = previous != nullptr && previous->haveFrameSemantics;
    uint32_t previousFrames = havePreviousFrameSemantics ? previous->frames : 0;
    FrameRateQ previousRate = havePreviousFrameSemantics ? previous->labelRate : FrameRateQ{};
    uint8_t previousCountingType = havePreviousFrameSemantics ? previous->countingType : uint8_t(0);
    bool previousDropFrame = havePreviousFrameSemantics && previous->dropFrame;

    for (uint32_t i = 0; i < clockCount; ++i) {
        bool clockTimestampFlag = false;
        if (!reader.bit(clockTimestampFlag)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (!clockTimestampFlag) {
            havePreviousSeconds = false;
            havePreviousMinutes = false;
            havePreviousHours = false;
            havePreviousFrameSemantics = false;
            outputPredecessor = nullptr;
            continue;
        }

        bool unitsFieldBased = false;
        uint32_t countingType = 0;
        bool fullTimestamp = false;
        bool discontinuity = false;
        bool countDropped = false;
        uint32_t frames = 0;
        if (!reader.bit(unitsFieldBased) || !reader.bits(5, countingType) ||
            !reader.bit(fullTimestamp) || !reader.bit(discontinuity) || !reader.bit(countDropped) ||
            !reader.bits(9, frames)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (countingType > 6) {
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

        uint32_t timeOffsetLength = 0;
        int32_t timeOffset = 0;
        if (!reader.bits(5, timeOffsetLength) ||
            !reader.signedBits(int(timeOffsetLength), timeOffset)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (countingType == 0 && timeOffsetLength != 0) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if ((haveSeconds && seconds >= 60) || (haveMinutes && minutes >= 60) ||
            (haveHours && hours >= 24)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }

        const bool effectiveSecondsPresent = haveSeconds || havePreviousSeconds;
        const bool effectiveMinutesPresent = haveMinutes || havePreviousMinutes;
        const bool effectiveHoursPresent = haveHours || havePreviousHours;
        const uint32_t effectiveSeconds = haveSeconds ? seconds : previousSeconds;
        const uint32_t effectiveMinutes = haveMinutes ? minutes : previousMinutes;
        const uint32_t effectiveHours = haveHours ? hours : previousHours;
        const bool complete =
            effectiveSecondsPresent && effectiveMinutesPresent && effectiveHoursPresent;
        if (!complete) unsupportedMapping = true;
        if (haveSeconds) {
            havePreviousSeconds = true;
            previousSeconds = seconds;
        }
        if (haveMinutes) {
            havePreviousMinutes = true;
            previousMinutes = minutes;
        }
        if (haveHours) {
            havePreviousHours = true;
            previousHours = hours;
        }

        const FrameRateQ rate = hevcLabelRate(syntax, unitsFieldBased);
        if (rate.valid() && !validHevcFrameCount(frames, rate)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }

        bool comparable = false;
        int64_t currentClock = 0;
        if (complete && rate.valid()) {
            const int32_t orderingOffset = countingType == 0 ? 0 : timeOffset;
            if (!hevcClockTimestamp(effectiveHours, effectiveMinutes, effectiveSeconds, frames,
                                    unitsFieldBased, orderingOffset, syntax, currentClock)) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            comparable = true;
        }

        bool representable = rate.valid();
        bool dropFrame = false;
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
            representable = false;
            break;
        case 3:
            if (countDropped && frames != 0) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            representable = false;
            break;
        case 4:
            dropFrame = true;
            if (countDropped &&
                (!effectiveSecondsPresent || !effectiveMinutesPresent || frames != 2 ||
                 effectiveSeconds != 0 || effectiveMinutes % 10 == 0)) {
                result.status = TimecodeParseStatus::Malformed;
                return result;
            }
            representable = representable && normalizedRateEquals(rate, 30000, 1001);
            break;
        case 5:
        case 6:
            representable = false;
            break;
        default:
            break;
        }
        HevcTimeCodeOutput currentOutput;
        currentOutput.present = true;
        currentOutput.comparable = comparable;
        currentOutput.clockTimestamp = currentClock;
        currentOutput.frames = frames;
        if (rate.valid())
            currentOutput.maxFps = uint32_t((int64_t(rate.num) + rate.den - 1) / rate.den);
        currentOutput.countingType = uint8_t(countingType);
        currentOutput.countDropped = countDropped;
        currentOutput.discontinuity = discontinuity;
        const TimecodeParseStatus transitionStatus =
            validateHevcOutputTransition(outputPredecessor, currentOutput);
        if (transitionStatus == TimecodeParseStatus::Malformed) {
            result.status = transitionStatus;
            return result;
        }
        if (transitionStatus == TimecodeParseStatus::Unsupported) unsupportedMapping = true;
        if (firstOutput != nullptr && !firstOutput->present) *firstOutput = currentOutput;
        if (lastOutput != nullptr) *lastOutput = currentOutput;
        localPreviousOutput = currentOutput;
        outputPredecessor = &localPreviousOutput;
        if (!representable) unsupportedMapping = true;
        havePreviousFrameSemantics = complete && rate.valid() && representable;
        if (havePreviousFrameSemantics) {
            previousFrames = frames;
            previousRate = rate;
            previousCountingType = uint8_t(countingType);
            previousDropFrame = dropFrame;
        }
        if (!complete || !representable) continue;

        const Smpte12mTimecode timestamp{int(effectiveHours),
                                         int(effectiveMinutes),
                                         int(effectiveSeconds),
                                         int(frames),
                                         dropFrame,
                                         true};
        if (!validateTimecodeLabel(timestamp, rate)) {
            result.status = TimecodeParseStatus::Malformed;
            return result;
        }
        if (!firstUsable.valid) {
            firstUsable = timestamp;
            firstRate = rate;
        }
        firstDiscontinuity = firstDiscontinuity || discontinuity;
    }

    if (!reader.seiPayloadAlignmentBits()) {
        result.status = TimecodeParseStatus::Malformed;
        return result;
    }
    if (unsupportedMapping) {
        result.status = TimecodeParseStatus::Unsupported;
        return result;
    }
    if (next != nullptr) {
        next->haveSeconds = havePreviousSeconds;
        next->haveMinutes = havePreviousMinutes;
        next->haveHours = havePreviousHours;
        next->seconds = previousSeconds;
        next->minutes = previousMinutes;
        next->hours = previousHours;
        next->haveFrameSemantics = havePreviousFrameSemantics;
        next->frames = previousFrames;
        next->labelRate = previousRate;
        next->countingType = previousCountingType;
        next->dropFrame = previousDropFrame;
    }
    if (firstUsable.valid) {
        result.status = TimecodeParseStatus::Valid;
        result.timecode = firstUsable;
        result.labelRate = firstRate;
        result.discontinuity = firstDiscontinuity;
    }
    return result;
}

#include "h26xseitimecode.h"

#include <QList>

namespace {

// --- Start-code scan: identical convention to H26xAccessUnitSplitter ---------
// (00 00 01 = 3-byte, 00 00 00 01 = 4-byte start codes).

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
    QList<int> offsets;
    for (int i = 0; i + 3 <= bytes.size();) {
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

// Each NAL payload spans from just after its start code to the next start code.
QList<QByteArray> splitAnnexBNals(const QByteArray& bytes) {
    QList<QByteArray> nals;
    const QList<int> starts = findStartCodes(bytes);
    for (int i = 0; i < starts.size(); ++i) {
        const int start = starts[i];
        const int prefixSize = startCodeSizeAt(bytes, start);
        const int payloadOffset = start + prefixSize;
        const int end = (i + 1 < starts.size()) ? starts[i + 1] : bytes.size();
        if (prefixSize == 0 || end <= payloadOffset) {
            continue;
        }
        nals.append(bytes.mid(payloadOffset, end - payloadOffset));
    }
    return nals;
}

// Strip emulation-prevention bytes (00 00 03 -> 00 00): same logic the splitter
// uses to recover an RBSP from a NAL payload.
QByteArray rbspFromPayload(const QByteArray& bytes) {
    QByteArray rbsp;
    rbsp.reserve(bytes.size());
    int zeroCount = 0;
    for (int i = 0; i < bytes.size(); ++i) {
        const uchar value = uchar(bytes[i]);
        if (zeroCount >= 2 && value == 0x03) {
            zeroCount = 0;
            continue;
        }
        rbsp.append(char(value));
        if (value == 0) {
            ++zeroCount;
        } else {
            zeroCount = 0;
        }
    }
    return rbsp;
}

bool isSeiNal(const QByteArray& nal, NativeVideoCodec codec) {
    if (codec == NativeVideoCodec::H264) {
        if (nal.isEmpty()) {
            return false;
        }
        return (uchar(nal[0]) & 0x1f) == 6;
    }
    if (codec == NativeVideoCodec::Hevc) {
        if (nal.size() < 2) {
            return false;
        }
        const int type = (uchar(nal[0]) >> 1) & 0x3f;
        return type == 39 || type == 40; // PREFIX_SEI / SUFFIX_SEI
    }
    return false;
}

// True if this payloadType carries a SMPTE 12M timecode word we can decode.
//   H.264: 1 = pic_timing, 4 = user_data_registered_itu_t_t35 (ATC).
//   HEVC : 136 = time_code, 4 = user_data_registered (ATC).
bool isTimecodePayloadType(int64_t payloadType, NativeVideoCodec codec) {
    if (codec == NativeVideoCodec::H264) {
        return payloadType == 1 || payloadType == 4;
    }
    if (codec == NativeVideoCodec::Hevc) {
        return payloadType == 136 || payloadType == 4;
    }
    return false;
}

// Read a SEI payloadType/payloadSize value: a run of 0xFF bytes plus a final
// byte < 0xFF. Returns false (and leaves pos unchanged-ish) on any short read so
// a truncated continuation run never reads past the RBSP end.
bool readSeiVarValue(const QByteArray& rbsp, int& pos, int64_t& value) {
    int64_t total = 0;
    while (true) {
        if (pos >= rbsp.size()) {
            return false; // ran off the end with no terminating byte
        }
        const int byte = uchar(rbsp[pos]);
        ++pos;
        // 64-bit accumulation: the run length is bounded by rbsp.size() (pos
        // advances each iteration), so total <= 255*size can never overflow
        // int64. payloadType may legitimately exceed the buffer size (e.g. 136 =
        // HEVC time_code in a tiny RBSP), so do NOT cap on size here; payloadSize
        // is bounds-checked against the remaining buffer at the call site.
        total += byte;
        if (byte != 0xFF) {
            break;
        }
    }
    value = total;
    return true;
}

// Decode the big-endian 32-bit SMPTE 12M word from the front of a SEI payload.
Smpte12mTimecode decodePayloadTimecode(const QByteArray& rbsp, int payloadStart, int payloadSize) {
    // Need 4 bytes for the packed word, and they must lie within the payload.
    if (payloadSize < 4) {
        return Smpte12mTimecode{};
    }
    if (payloadStart < 0 || payloadStart + 4 > rbsp.size()) {
        return Smpte12mTimecode{};
    }
    const uint32_t word = (static_cast<uint32_t>(uchar(rbsp[payloadStart])) << 24) |
                          (static_cast<uint32_t>(uchar(rbsp[payloadStart + 1])) << 16) |
                          (static_cast<uint32_t>(uchar(rbsp[payloadStart + 2])) << 8) |
                          static_cast<uint32_t>(uchar(rbsp[payloadStart + 3]));
    return Smpte12m::fromPackedWord(word);
}

// Walk one SEI RBSP's messages; return the first recognised timecode.
Smpte12mTimecode extractFromSeiRbsp(const QByteArray& rbsp, NativeVideoCodec codec) {
    int pos = 0;
    while (pos < rbsp.size()) {
        // RBSP trailing bits (0x80) or padding terminate the message list.
        if (uchar(rbsp[pos]) == 0x80) {
            break;
        }

        int64_t payloadType = 0;
        if (!readSeiVarValue(rbsp, pos, payloadType)) {
            break; // truncated payloadType run -> stop, no timecode
        }
        int64_t payloadSize = 0;
        if (!readSeiVarValue(rbsp, pos, payloadSize)) {
            break; // truncated payloadSize run -> stop, no timecode
        }

        // The declared payload must fit entirely within the remaining RBSP.
        // payloadSize is >=0 and <= rbsp.size() (readSeiVarValue caps it), and
        // pos <= rbsp.size(), so rbsp.size() - pos is a safe non-negative bound.
        if (payloadSize > rbsp.size() - pos) {
            break; // truncated payload -> never read OOB
        }

        if (isTimecodePayloadType(payloadType, codec)) {
            const Smpte12mTimecode tc =
                decodePayloadTimecode(rbsp, pos, static_cast<int>(payloadSize));
            if (tc.valid) {
                return tc;
            }
        }

        pos += static_cast<int>(payloadSize); // advance past this message to the next
    }
    return Smpte12mTimecode{};
}

} // namespace

Smpte12mTimecode extractH26xSeiTimecode(const QByteArray& annexB, NativeVideoCodec codec) {
    if (annexB.isEmpty() || codec == NativeVideoCodec::Unknown) {
        return Smpte12mTimecode{};
    }

    const QList<QByteArray> nals = splitAnnexBNals(annexB);
    for (const QByteArray& nal : nals) {
        if (!isSeiNal(nal, codec)) {
            continue;
        }
        // SEI RBSP begins after the NAL header (1 byte H.264, 2 bytes HEVC).
        const int headerBytes = (codec == NativeVideoCodec::H264) ? 1 : 2;
        if (nal.size() <= headerBytes) {
            continue;
        }
        const QByteArray rbsp = rbspFromPayload(nal.mid(headerBytes));
        const Smpte12mTimecode tc = extractFromSeiRbsp(rbsp, codec);
        if (tc.valid) {
            return tc;
        }
    }
    return Smpte12mTimecode{};
}

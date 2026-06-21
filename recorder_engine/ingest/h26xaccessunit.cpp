#include "h26xaccessunit.h"

#include <QtGlobal>

namespace {

struct AnnexBNal {
    int startOffset = 0;
    int payloadOffset = 0;
    int endOffset = 0;
    QByteArray nal;
};

struct PendingAccessUnit {
    int startOffset = -1;
    int endOffset = -1;
    bool hasVcl = false;
};

int startCodeSizeAt(const QByteArray& bytes, int offset) {
    if (offset + 3 <= bytes.size()
        && bytes[offset] == char(0)
        && bytes[offset + 1] == char(0)
        && bytes[offset + 2] == char(1)) {
        return 3;
    }
    if (offset + 4 <= bytes.size()
        && bytes[offset] == char(0)
        && bytes[offset + 1] == char(0)
        && bytes[offset + 2] == char(0)
        && bytes[offset + 3] == char(1)) {
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

QList<AnnexBNal> splitAnnexBNals(const QByteArray& bytes) {
    QList<AnnexBNal> nals;
    const QList<int> starts = findStartCodes(bytes);
    for (int i = 0; i < starts.size(); ++i) {
        const int start = starts[i];
        const int prefixSize = startCodeSizeAt(bytes, start);
        const int payloadOffset = start + prefixSize;
        const int end = (i + 1 < starts.size()) ? starts[i + 1] : bytes.size();
        if (prefixSize == 0 || end <= payloadOffset) {
            continue;
        }

        AnnexBNal nal;
        nal.startOffset = start;
        nal.payloadOffset = payloadOffset;
        nal.endOffset = end;
        nal.nal = bytes.mid(payloadOffset, end - payloadOffset);
        nals.append(nal);
    }
    return nals;
}

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

class BitReader {
public:
    explicit BitReader(const QByteArray& bytes)
        : m_bytes(bytes) {}

    bool readBit(bool* bit) {
        if (!bit || m_bitOffset >= m_bytes.size() * 8) {
            return false;
        }
        const int byteOffset = m_bitOffset / 8;
        const int bitInByte = 7 - (m_bitOffset % 8);
        *bit = ((uchar(m_bytes[byteOffset]) >> bitInByte) & 0x01) != 0;
        ++m_bitOffset;
        return true;
    }

    bool readUnsignedExpGolomb(quint32* value) {
        if (!value) {
            return false;
        }

        int leadingZeroes = 0;
        bool bit = false;
        while (readBit(&bit)) {
            if (bit) {
                break;
            }
            ++leadingZeroes;
            if (leadingZeroes > 31) {
                return false;
            }
        }
        if (!bit) {
            return false;
        }

        quint32 suffix = 0;
        for (int i = 0; i < leadingZeroes; ++i) {
            if (!readBit(&bit)) {
                return false;
            }
            suffix = (suffix << 1) | (bit ? 1u : 0u);
        }

        *value = ((1u << leadingZeroes) - 1u) + suffix;
        return true;
    }

private:
    QByteArray m_bytes;
    int m_bitOffset = 0;
};

bool h264IsVcl(int type) {
    return type >= 1 && type <= 5;
}

bool h264StartsNewPicture(const QByteArray& nal) {
    if (nal.size() < 2) {
        return false;
    }

    const QByteArray rbsp = rbspFromPayload(nal.mid(1));
    BitReader reader(rbsp);
    quint32 firstMbInSlice = 0;
    return reader.readUnsignedExpGolomb(&firstMbInSlice) && firstMbInSlice == 0;
}

bool hevcIsVcl(int type) {
    return type >= 0 && type <= 31;
}

bool hevcStartsNewPicture(const QByteArray& nal) {
    if (nal.size() < 3) {
        return false;
    }

    const QByteArray rbsp = rbspFromPayload(nal.mid(2));
    return !rbsp.isEmpty() && ((uchar(rbsp[0]) & 0x80) != 0);
}

bool isH264ParameterSet(int type) {
    return type == 7 || type == 8;
}

bool isHevcParameterSet(int type) {
    return type == 32 || type == 33 || type == 34;
}

// A real SPS/PPS/VPS is tiny (tens to a few hundred bytes). Cap both the per-NAL
// size and the count of tracked parameter sets: m_parameterSets is copied into
// every emitted access unit and is reset only per connection, so a hostile
// stream that floods slightly-varied parameter-set NALs (dedup by exact equality
// is trivially evaded) would otherwise grow it without bound — a memory + O(n)-
// copy DoS. Evict the oldest on overflow so legitimate mid-stream parameter-set
// updates still take effect, and ignore implausibly large parameter-set NALs.
constexpr int kMaxParameterSetsPerList = 16;
constexpr int kMaxParameterSetBytes = 8 * 1024;

template <typename T>
void appendUnique(QList<T>* values, const T& value) {
    if (!values || value.size() > kMaxParameterSetBytes || values->contains(value)) {
        return;
    }
    if (values->size() >= kMaxParameterSetsPerList) {
        values->removeFirst(); // bounded recent window; oldest evicted
    }
    values->append(value);
}

} // namespace

H26xAccessUnitSplitter::H26xAccessUnitSplitter(NativeVideoCodec codec)
    : m_codec(codec) {}

QList<CompressedAccessUnit> H26xAccessUnitSplitter::pushPesPayload(const QByteArray& payload,
                                                                   qint64 pts90k,
                                                                   qint64 dts90k) {
    QList<CompressedAccessUnit> units;
    if (payload.isEmpty() || m_codec == NativeVideoCodec::Unknown) {
        return units;
    }

    const QList<AnnexBNal> nals = splitAnnexBNals(payload);
    if (nals.isEmpty()) {
        return units;
    }

    PendingAccessUnit pending;

    const auto flushPending = [&]() {
        if (pending.startOffset < 0 || pending.endOffset <= pending.startOffset) {
            return;
        }

        CompressedAccessUnit unit;
        unit.codec = m_codec;
        unit.pts90k = pts90k;
        unit.dts90k = dts90k;
        unit.annexB = payload.mid(pending.startOffset, pending.endOffset - pending.startOffset);
        unit.parameterSets = m_parameterSets;
        units.append(unit);

        pending = PendingAccessUnit();
    };

    for (const AnnexBNal& nal : nals) {
        if (nal.nal.isEmpty()) {
            continue;
        }

        const int type = (m_codec == NativeVideoCodec::H264)
            ? (uchar(nal.nal[0]) & 0x1f)
            : ((uchar(nal.nal[0]) >> 1) & 0x3f);
        const bool isParameterSet = (m_codec == NativeVideoCodec::H264)
            ? isH264ParameterSet(type)
            : isHevcParameterSet(type);
        const bool isAud = (m_codec == NativeVideoCodec::H264) ? (type == 9) : (type == 35);
        const bool isVcl = (m_codec == NativeVideoCodec::H264) ? h264IsVcl(type) : hevcIsVcl(type);
        const bool startsNewPicture = isVcl && ((m_codec == NativeVideoCodec::H264)
            ? h264StartsNewPicture(nal.nal)
            : hevcStartsNewPicture(nal.nal));

        if (pending.startOffset >= 0
            && (isAud || (pending.hasVcl && (isParameterSet || startsNewPicture)))) {
            flushPending();
        }

        if (pending.startOffset < 0) {
            pending.startOffset = nal.startOffset;
        }
        pending.endOffset = nal.endOffset;
        pending.hasVcl = pending.hasVcl || isVcl;

        inspectNal(nal.nal);
    }

    flushPending();
    return units;
}

void H26xAccessUnitSplitter::inspectNal(const QByteArray& nal) {
    if (nal.isEmpty()) {
        return;
    }

    if (m_codec == NativeVideoCodec::H264) {
        const int type = uchar(nal[0]) & 0x1f;
        if (type == 7) {
            appendUnique(&m_parameterSets.h264Sps, nal);
        } else if (type == 8) {
            appendUnique(&m_parameterSets.h264Pps, nal);
        }
    } else if (m_codec == NativeVideoCodec::Hevc && nal.size() >= 2) {
        const int type = (uchar(nal[0]) >> 1) & 0x3f;
        if (type == 32) {
            appendUnique(&m_parameterSets.hevcVps, nal);
        } else if (type == 33) {
            appendUnique(&m_parameterSets.hevcSps, nal);
        } else if (type == 34) {
            appendUnique(&m_parameterSets.hevcPps, nal);
        }
    }
}

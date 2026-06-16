#include "rtmpprotocol.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {
const int kAacSampleRates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};
constexpr int kMaxAmf0SkipDepth = 64;

void appendU24(QByteArray* bytes, int value) {
    bytes->append(char((value >> 16) & 0xff));
    bytes->append(char((value >> 8) & 0xff));
    bytes->append(char(value & 0xff));
}

int readU24(const char* data) {
    return (int(uchar(data[0])) << 16) | (int(uchar(data[1])) << 8) | int(uchar(data[2]));
}

quint32 readU32Be(const char* data) {
    return (quint32(uchar(data[0])) << 24) | (quint32(uchar(data[1])) << 16) |
           (quint32(uchar(data[2])) << 8) | quint32(uchar(data[3]));
}

quint32 readU32Le(const char* data) {
    return quint32(uchar(data[0])) | (quint32(uchar(data[1])) << 8) |
           (quint32(uchar(data[2])) << 16) | (quint32(uchar(data[3])) << 24);
}

bool needMore(const QByteArray& bytes, int offset, int size) {
    return offset + size > bytes.size();
}

bool skipAmf0Value(const QByteArray& data, int* offset, int depth) {
    if (!offset || depth > kMaxAmf0SkipDepth || *offset < 0 || *offset >= data.size()) {
        return false;
    }

    int cursor = *offset;
    const int type = uchar(data[cursor]);
    ++cursor;
    if (type == 0x00) {
        if (needMore(data, cursor, 8)) return false;
        cursor += 8;
        *offset = cursor;
        return true;
    }
    if (type == 0x01) {
        if (needMore(data, cursor, 1)) return false;
        cursor += 1;
        *offset = cursor;
        return true;
    }
    if (type == 0x02) {
        if (needMore(data, cursor, 2)) return false;
        const int size = (int(uchar(data[cursor])) << 8) | int(uchar(data[cursor + 1]));
        if (needMore(data, cursor + 2, size)) return false;
        cursor += 2 + size;
        *offset = cursor;
        return true;
    }
    if (type == 0x03) {
        while (cursor + 3 <= data.size()) {
            if (uchar(data[cursor]) == 0 && uchar(data[cursor + 1]) == 0 &&
                uchar(data[cursor + 2]) == 0x09) {
                cursor += 3;
                *offset = cursor;
                return true;
            }
            const int keySize = (int(uchar(data[cursor])) << 8) | int(uchar(data[cursor + 1]));
            if (needMore(data, cursor + 2, keySize)) return false;
            cursor += 2 + keySize;
            if (!skipAmf0Value(data, &cursor, depth + 1)) return false;
        }
        return false;
    }
    if (type == 0x08) {
        if (needMore(data, cursor, 4)) return false;
        cursor += 4;
        while (cursor + 3 <= data.size()) {
            if (uchar(data[cursor]) == 0 && uchar(data[cursor + 1]) == 0 &&
                uchar(data[cursor + 2]) == 0x09) {
                cursor += 3;
                *offset = cursor;
                return true;
            }
            const int keySize = (int(uchar(data[cursor])) << 8) | int(uchar(data[cursor + 1]));
            if (needMore(data, cursor + 2, keySize)) return false;
            cursor += 2 + keySize;
            if (!skipAmf0Value(data, &cursor, depth + 1)) return false;
        }
        return false;
    }
    if (type == 0x0a) {
        if (needMore(data, cursor, 4)) return false;
        const quint32 count = readU32Be(data.constData() + cursor);
        cursor += 4;
        for (quint32 i = 0; i < count; ++i) {
            if (!skipAmf0Value(data, &cursor, depth + 1)) return false;
        }
        *offset = cursor;
        return true;
    }
    if (type == 0x05 || type == 0x06) {
        *offset = cursor;
        return true;
    }
    return false;
}

} // namespace

QByteArray RtmpAmf0::number(double value) {
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double size");
    memcpy(&bits, &value, sizeof(bits));
    QByteArray out;
    out.append(char(0x00));
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.append(char((bits >> shift) & 0xff));
    }
    return out;
}

QByteArray RtmpAmf0::boolean(bool value) {
    QByteArray out;
    out.append(char(0x01));
    out.append(value ? char(1) : char(0));
    return out;
}

QByteArray RtmpAmf0::string(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    QByteArray out;
    out.append(char(0x02));
    out.append(char((utf8.size() >> 8) & 0xff));
    out.append(char(utf8.size() & 0xff));
    out.append(utf8);
    return out;
}

QByteArray RtmpAmf0::nullValue() {
    return QByteArray(1, char(0x05));
}

QByteArray RtmpAmf0::object(const QList<QPair<QString, QByteArray>>& values) {
    QByteArray out;
    out.append(char(0x03));
    for (const auto& value : values) {
        const QByteArray utf8 = value.first.toUtf8();
        out.append(char((utf8.size() >> 8) & 0xff));
        out.append(char(utf8.size() & 0xff));
        out.append(utf8);
        out.append(value.second);
    }
    out.append("\0\0\x09", 3);
    return out;
}

QByteArray RtmpAmf0::strictArray(const QList<QByteArray>& values) {
    QByteArray out;
    out.append(char(0x0a));
    const quint32 count = quint32(values.size());
    out.append(char((count >> 24) & 0xff));
    out.append(char((count >> 16) & 0xff));
    out.append(char((count >> 8) & 0xff));
    out.append(char(count & 0xff));
    for (const QByteArray& value : values) {
        out.append(value);
    }
    return out;
}

bool RtmpAmf0::readString(const QByteArray& data, int* offset, QString* value) {
    if (!offset || !value || needMore(data, *offset, 3) || uchar(data[*offset]) != 0x02) {
        return false;
    }
    const int size = (int(uchar(data[*offset + 1])) << 8) | int(uchar(data[*offset + 2]));
    *offset += 3;
    if (needMore(data, *offset, size)) {
        return false;
    }
    *value = QString::fromUtf8(data.constData() + *offset, size);
    *offset += size;
    return true;
}

bool RtmpAmf0::readNumber(const QByteArray& data, int* offset, double* value) {
    if (!offset || !value || needMore(data, *offset, 9) || uchar(data[*offset]) != 0x00) {
        return false;
    }
    quint64 bits = 0;
    for (int i = 1; i <= 8; ++i) {
        bits = (bits << 8) | quint64(uchar(data[*offset + i]));
    }
    memcpy(value, &bits, sizeof(bits));
    *offset += 9;
    return true;
}

bool RtmpAmf0::skipValue(const QByteArray& data, int* offset) {
    if (!offset) {
        return false;
    }
    int cursor = *offset;
    if (!skipAmf0Value(data, &cursor, 0)) return false;
    *offset = cursor;
    return true;
}

QByteArray RtmpChunkWriter::message(int chunkStreamId, int messageType, int messageStreamId,
                                    qint64 timestampMs, const QByteArray& payload, int chunkSize) {
    QByteArray out;
    out.append(char(chunkStreamId & 0x3f));
    appendU24(&out, int(qMin<qint64>(timestampMs, 0xffffff)));
    appendU24(&out, payload.size());
    out.append(char(messageType));
    const quint32 streamId = quint32(messageStreamId);
    out.append(char(streamId & 0xff));
    out.append(char((streamId >> 8) & 0xff));
    out.append(char((streamId >> 16) & 0xff));
    out.append(char((streamId >> 24) & 0xff));
    if (timestampMs >= 0xffffff) {
        const quint32 ts = quint32(timestampMs);
        out.append(char((ts >> 24) & 0xff));
        out.append(char((ts >> 16) & 0xff));
        out.append(char((ts >> 8) & 0xff));
        out.append(char(ts & 0xff));
    }

    const int boundedChunkSize = qMax(1, chunkSize);
    int offset = 0;
    bool first = true;
    while (offset < payload.size() || (first && payload.isEmpty())) {
        const int size = qMin(boundedChunkSize, payload.size() - offset);
        if (!first) {
            out.append(char((3 << 6) | (chunkStreamId & 0x3f)));
            if (timestampMs >= 0xffffff) {
                const quint32 ts = quint32(timestampMs);
                out.append(char((ts >> 24) & 0xff));
                out.append(char((ts >> 16) & 0xff));
                out.append(char((ts >> 8) & 0xff));
                out.append(char(ts & 0xff));
            }
        }
        if (size > 0) out.append(payload.constData() + offset, size);
        offset += size;
        first = false;
    }
    return out;
}

void RtmpChunkParser::reset() {
    m_buffer.clear();
    m_inputChunkSize = 128;
    m_previousHeaders.clear();
    m_assemblies.clear();
    m_abortedChunkStreams.clear();
}

int RtmpChunkParser::assemblyPayloadBytes() const {
    int total = 0;
    for (auto it = m_assemblies.cbegin(); it != m_assemblies.cend(); ++it) {
        total += it.value().payload.size();
    }
    return total;
}

bool RtmpChunkParser::tryParseFragment(ParsedChunkFragment* fragment, QString* error) {
    if (!fragment || m_buffer.isEmpty()) {
        return false;
    }

    ParsedChunkFragment parsed;
    int offset = 0;
    const int first = uchar(m_buffer[offset++]);
    const int fmt = (first >> 6) & 0x03;
    int csid = first & 0x3f;
    if (csid == 0) {
        if (needMore(m_buffer, offset, 1)) return false;
        csid = 64 + uchar(m_buffer[offset++]);
    } else if (csid == 1) {
        if (needMore(m_buffer, offset, 2)) return false;
        csid = 64 + uchar(m_buffer[offset]) + 256 * uchar(m_buffer[offset + 1]);
        offset += 2;
    }

    const bool hasPreviousHeader = m_previousHeaders.contains(csid);
    const bool hasActiveAssembly = m_assemblies.contains(csid);
    const bool isAborted = m_abortedChunkStreams.contains(csid);
    if (fmt != 0 && !hasPreviousHeader) {
        if (error) *error = QStringLiteral("RTMP chunk header required a previous header.");
        return false;
    }
    if (isAborted && fmt == 3) {
        ChunkHeader previousHeader = m_previousHeaders.value(csid);
        if (previousHeader.usesExtendedTimestamp) {
            if (needMore(m_buffer, offset, 4)) return false;
            offset += 4;
        }

        const int remaining = m_abortedChunkStreams.value(csid);
        if (remaining <= 0) {
            if (error) {
                *error = QStringLiteral("RTMP aborted chunk stream received stale continuation.");
            }
            return false;
        }

        const int toRead = qMin(m_inputChunkSize, remaining);
        if (needMore(m_buffer, offset, toRead)) return false;

        parsed.consumed = offset + toRead;
        parsed.csid = csid;
        parsed.header = previousHeader;
        parsed.discarded = true;
        parsed.discardedPayloadBytes = toRead;
        *fragment = std::move(parsed);
        return true;
    }
    if (fmt != 3 && hasActiveAssembly) {
        if (error) {
            *error = QStringLiteral(
                "RTMP new chunk header arrived before incomplete message assembly completed.");
        }
        return false;
    }

    const ChunkHeader previousHeader = m_previousHeaders.value(csid);
    ChunkHeader header = previousHeader;
    parsed.startsMessage = fmt != 3 || !hasActiveAssembly;

    if (fmt == 0) {
        if (needMore(m_buffer, offset, 11)) return false;
        const int timestamp = readU24(m_buffer.constData() + offset);
        header.timestampMs = timestamp;
        header.timestampDeltaMs = 0;
        header.messageLength = readU24(m_buffer.constData() + offset + 3);
        header.messageType = uchar(m_buffer[offset + 6]);
        header.messageStreamId = int(readU32Le(m_buffer.constData() + offset + 7));
        header.timestampIsDelta = false;
        header.usesExtendedTimestamp = timestamp == 0xffffff;
        offset += 11;
    } else if (fmt == 1) {
        if (needMore(m_buffer, offset, 7)) return false;
        const int timestampDelta = readU24(m_buffer.constData() + offset);
        header.timestampMs = previousHeader.timestampMs;
        header.timestampDeltaMs = timestampDelta;
        header.messageLength = readU24(m_buffer.constData() + offset + 3);
        header.messageType = uchar(m_buffer[offset + 6]);
        header.timestampIsDelta = true;
        header.usesExtendedTimestamp = timestampDelta == 0xffffff;
        if (!header.usesExtendedTimestamp) {
            header.timestampMs += header.timestampDeltaMs;
        }
        offset += 7;
    } else if (fmt == 2) {
        if (needMore(m_buffer, offset, 3)) return false;
        const int timestampDelta = readU24(m_buffer.constData() + offset);
        header.timestampMs = previousHeader.timestampMs;
        header.timestampDeltaMs = timestampDelta;
        header.timestampIsDelta = true;
        header.usesExtendedTimestamp = timestampDelta == 0xffffff;
        if (!header.usesExtendedTimestamp) {
            header.timestampMs += header.timestampDeltaMs;
        }
        offset += 3;
    } else {
        if (!hasActiveAssembly && header.timestampIsDelta) {
            header.timestampMs += header.timestampDeltaMs;
        }
        header.usesExtendedTimestamp = previousHeader.usesExtendedTimestamp;
    }

    if (header.usesExtendedTimestamp) {
        if (needMore(m_buffer, offset, 4)) return false;
        const quint32 timestamp = readU32Be(m_buffer.constData() + offset);
        if (fmt == 0) {
            header.timestampMs = timestamp;
        } else if (fmt == 1 || fmt == 2) {
            header.timestampDeltaMs = timestamp;
            header.timestampMs = previousHeader.timestampMs + header.timestampDeltaMs;
        }
        offset += 4;
    }

    if (header.messageLength > m_maxMessageSize) {
        if (error) {
            *error = QStringLiteral("RTMP message length %1 exceeds limit %2.")
                         .arg(header.messageLength)
                         .arg(m_maxMessageSize);
        }
        return false;
    }

    ChunkAssembly assembly = m_assemblies.value(csid);
    if (parsed.startsMessage || assembly.payload.isEmpty()) {
        assembly.header = header;
    }
    const int remaining = assembly.header.messageLength - assembly.payload.size();
    if (remaining < 0) {
        if (error) *error = QStringLiteral("RTMP chunk overflow.");
        return false;
    }
    const int toRead = qMin(m_inputChunkSize, remaining);
    if (needMore(m_buffer, offset, toRead)) return false;
    if (toRead > 0) {
        parsed.fragment = m_buffer.mid(offset, toRead);
        offset += toRead;
    }

    parsed.consumed = offset;
    parsed.csid = csid;
    parsed.header = header;
    if (isAborted && fmt != 3) {
        m_abortedChunkStreams.remove(csid);
    }
    *fragment = std::move(parsed);
    return true;
}

bool RtmpChunkParser::push(const QByteArray& bytes, QList<RtmpMessage>* messages, QString* error) {
    if (!messages) {
        return false;
    }
    messages->clear();
    if (m_buffer.size() + bytes.size() > m_maxBufferedBytes) {
        if (error) *error = QStringLiteral("RTMP buffered bytes exceed limit.");
        return false;
    }
    m_buffer.append(bytes);

    while (!m_buffer.isEmpty()) {
        ParsedChunkFragment fragment;
        QString parseError;
        if (!tryParseFragment(&fragment, &parseError)) {
            if (!parseError.isEmpty()) {
                if (error) *error = parseError;
                return false;
            }
            return true;
        }

        if (fragment.discarded) {
            const int remaining =
                qMax(0, m_abortedChunkStreams.value(fragment.csid) - fragment.discardedPayloadBytes);
            if (remaining > 0) {
                m_abortedChunkStreams.insert(fragment.csid, remaining);
            } else {
                m_abortedChunkStreams.remove(fragment.csid);
            }
            m_buffer.remove(0, fragment.consumed);
            continue;
        }

        ChunkAssembly assembly = m_assemblies.value(fragment.csid);
        if (fragment.startsMessage || assembly.payload.isEmpty()) {
            assembly.header = fragment.header;
        }
        const int existingAssemblyBytes = m_assemblies.value(fragment.csid).payload.size();
        const int projectedPayloadSize = assembly.payload.size() + fragment.fragment.size();
        if (projectedPayloadSize < assembly.header.messageLength &&
            assemblyPayloadBytes() - existingAssemblyBytes + projectedPayloadSize >
                m_maxAssemblyBytes) {
            if (error) *error = QStringLiteral("RTMP assembly bytes exceed limit.");
            return false;
        }

        assembly.payload.append(fragment.fragment);
        if (assembly.payload.size() > assembly.header.messageLength) {
            if (error) *error = QStringLiteral("RTMP chunk overflow.");
            return false;
        }

        m_previousHeaders.insert(fragment.csid, fragment.header);
        m_buffer.remove(0, fragment.consumed);

        if (assembly.payload.size() < assembly.header.messageLength) {
            m_assemblies.insert(fragment.csid, assembly);
            continue;
        }
        m_assemblies.remove(fragment.csid);

        RtmpMessage message;
        message.type = assembly.header.messageType;
        message.streamId = assembly.header.messageStreamId;
        message.timestampMs = assembly.header.timestampMs;
        message.payload = std::move(assembly.payload);
        if (message.type == 2) {
            if (message.payload.size() != 4) {
                if (error) *error = QStringLiteral("RTMP abort payload was malformed.");
                return false;
            }
            const int abortCsid = int(readU32Be(message.payload.constData()));
            if (m_assemblies.contains(abortCsid)) {
                const ChunkAssembly abortedAssembly = m_assemblies.take(abortCsid);
                const int remaining =
                    qMax(0, abortedAssembly.header.messageLength - abortedAssembly.payload.size());
                if (remaining > 0) {
                    m_abortedChunkStreams.insert(abortCsid, remaining);
                } else {
                    m_abortedChunkStreams.remove(abortCsid);
                }
            }
        }
        if (message.type == 1) {
            if (message.payload.size() != 4) {
                if (error) *error = QStringLiteral("RTMP set chunk size payload was malformed.");
                return false;
            }
            const quint32 chunkSize = readU32Be(message.payload.constData());
            if (chunkSize == 0 || chunkSize > 0x7fffffff) {
                if (error) *error = QStringLiteral("RTMP set chunk size was invalid.");
                return false;
            }
            m_inputChunkSize = int(chunkSize);
        }
        messages->append(std::move(message));
    }
    return true;
}

qint32 RtmpFlv::readS24(const char* data) {
    qint32 value = readU24(data);
    if (value & 0x800000) {
        value |= ~0xffffff;
    }
    return value;
}

bool RtmpFlv::parseAvcSequenceHeader(const QByteArray& payload, RtmpAvcConfig* config,
                                     QString* error) {
    if (!config || payload.size() < 7 || uchar(payload[0]) != 1) {
        if (error) *error = QStringLiteral("RTMP AVC sequence header is malformed.");
        return false;
    }
    RtmpAvcConfig parsed;
    parsed.nalLengthSize = (uchar(payload[4]) & 0x03) + 1;
    int offset = 5;
    const int spsCount = uchar(payload[offset++]) & 0x1f;
    for (int i = 0; i < spsCount; ++i) {
        if (needMore(payload, offset, 2)) return false;
        const int size = (int(uchar(payload[offset])) << 8) | int(uchar(payload[offset + 1]));
        offset += 2;
        if (size <= 0 || needMore(payload, offset, size)) return false;
        parsed.parameterSets.h264Sps.append(payload.mid(offset, size));
        offset += size;
    }
    if (offset >= payload.size()) return false;
    const int ppsCount = uchar(payload[offset++]);
    for (int i = 0; i < ppsCount; ++i) {
        if (needMore(payload, offset, 2)) return false;
        const int size = (int(uchar(payload[offset])) << 8) | int(uchar(payload[offset + 1]));
        offset += 2;
        if (size <= 0 || needMore(payload, offset, size)) return false;
        parsed.parameterSets.h264Pps.append(payload.mid(offset, size));
        offset += size;
    }
    if (parsed.parameterSets.h264Sps.isEmpty() || parsed.parameterSets.h264Pps.isEmpty()) {
        if (error) *error = QStringLiteral("RTMP AVC sequence header lacks SPS/PPS.");
        return false;
    }
    *config = std::move(parsed);
    return true;
}

QByteArray RtmpFlv::avcPayloadToAnnexB(const QByteArray& payload, int nalLengthSize) {
    QByteArray annexB;
    int offset = 0;
    while (offset + nalLengthSize <= payload.size()) {
        int nalSize = 0;
        for (int i = 0; i < nalLengthSize; ++i) {
            nalSize = (nalSize << 8) | int(uchar(payload[offset + i]));
        }
        offset += nalLengthSize;
        if (nalSize <= 0 || offset + nalSize > payload.size()) {
            break;
        }
        annexB.append("\0\0\0\1", 4);
        annexB.append(payload.constData() + offset, nalSize);
        offset += nalSize;
    }
    return annexB;
}

bool RtmpFlv::parseAacSequenceHeader(const QByteArray& payload, RtmpAacConfig* config,
                                     QString* error) {
    if (!config || payload.size() < 2) {
        if (error) *error = QStringLiteral("RTMP AAC sequence header is malformed.");
        return false;
    }
    const quint16 bits = (quint16(uchar(payload[0])) << 8) | quint16(uchar(payload[1]));
    RtmpAacConfig parsed;
    parsed.audioObjectType = (bits >> 11) & 0x1f;
    const int sampleRateIndex = (bits >> 7) & 0x0f;
    parsed.channelCount = (bits >> 3) & 0x0f;
    if (parsed.audioObjectType <= 0 || sampleRateIndex >= int(std::size(kAacSampleRates)) ||
        parsed.channelCount <= 0) {
        if (error) *error = QStringLiteral("RTMP AAC sequence header uses unsupported config.");
        return false;
    }
    parsed.sampleRate = kAacSampleRates[sampleRateIndex];
    *config = parsed;
    return true;
}

QByteArray RtmpFlv::adtsHeader(const RtmpAacConfig& config, int payloadSize) {
    int sampleRateIndex = -1;
    for (int i = 0; i < int(std::size(kAacSampleRates)); ++i) {
        if (kAacSampleRates[i] == config.sampleRate) {
            sampleRateIndex = i;
            break;
        }
    }
    if (sampleRateIndex < 0) {
        return {};
    }
    const int profile = std::max(0, config.audioObjectType - 1);
    const int frameLength = payloadSize + 7;
    QByteArray out(7, Qt::Uninitialized);
    out[0] = char(0xff);
    out[1] = char(0xf1);
    out[2] = char(((profile & 0x03) << 6) | ((sampleRateIndex & 0x0f) << 2) |
                  ((config.channelCount >> 2) & 0x01));
    out[3] = char(((config.channelCount & 0x03) << 6) | ((frameLength >> 11) & 0x03));
    out[4] = char((frameLength >> 3) & 0xff);
    out[5] = char(((frameLength & 0x07) << 5) | 0x1f);
    out[6] = char(0xfc);
    return out;
}

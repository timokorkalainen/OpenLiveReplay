#ifndef RTMPPROTOCOL_H
#define RTMPPROTOCOL_H

#include "h26xaccessunit.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

struct RtmpMessage {
    int type = 0;
    int streamId = 0;
    qint64 timestampMs = 0;
    QByteArray payload;
};

namespace RtmpAmf0 {
QByteArray number(double value);
QByteArray boolean(bool value);
QByteArray string(const QString& value);
QByteArray nullValue();
QByteArray object(const QList<QPair<QString, QByteArray>>& values);

bool readString(const QByteArray& data, int* offset, QString* value);
bool readNumber(const QByteArray& data, int* offset, double* value);
bool skipValue(const QByteArray& data, int* offset);
} // namespace RtmpAmf0

namespace RtmpChunkWriter {
QByteArray message(int chunkStreamId, int messageType, int messageStreamId, qint64 timestampMs,
                   const QByteArray& payload, int chunkSize);
} // namespace RtmpChunkWriter

class RtmpChunkParser {
public:
    bool push(const QByteArray& bytes, QList<RtmpMessage>* messages, QString* error);
    int inputChunkSize() const { return m_inputChunkSize; }
    void reset();

private:
    struct ChunkHeader {
        qint64 timestampMs = 0;
        qint64 timestampDeltaMs = 0;
        int messageLength = 0;
        int messageType = 0;
        int messageStreamId = 0;
        bool usesExtendedTimestamp = false;
    };
    struct ChunkAssembly {
        ChunkHeader header;
        QByteArray payload;
    };
    struct ParsedChunkFragment {
        int consumed = 0;
        int csid = 0;
        ChunkHeader header;
        QByteArray fragment;
        bool startsMessage = false;
    };

    bool tryParseFragment(ParsedChunkFragment* fragment, QString* error) const;

    QByteArray m_buffer;
    int m_inputChunkSize = 128;
    QHash<int, ChunkHeader> m_previousHeaders;
    QHash<int, ChunkAssembly> m_assemblies;
};

struct RtmpAvcConfig {
    int nalLengthSize = 4;
    H26xParameterSets parameterSets;
};

struct RtmpAacConfig {
    int audioObjectType = 0;
    int sampleRate = 0;
    int channelCount = 0;
};

namespace RtmpFlv {
qint32 readS24(const char* data);
bool parseAvcSequenceHeader(const QByteArray& payload, RtmpAvcConfig* config, QString* error);
QByteArray avcPayloadToAnnexB(const QByteArray& payload, int nalLengthSize);
bool parseAacSequenceHeader(const QByteArray& payload, RtmpAacConfig* config, QString* error);
QByteArray adtsHeader(const RtmpAacConfig& config, int payloadSize);
} // namespace RtmpFlv

#endif // RTMPPROTOCOL_H

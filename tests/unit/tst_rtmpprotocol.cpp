#include <QtTest>

#include "recorder_engine/ingest/rtmpprotocol.h"

namespace {
void appendU24(QByteArray* bytes, int value) {
    bytes->append(char((value >> 16) & 0xff));
    bytes->append(char((value >> 8) & 0xff));
    bytes->append(char(value & 0xff));
}

bool errorMentionsPreviousHeader(const QString& error) {
    return error.contains(QStringLiteral("previous"), Qt::CaseInsensitive) ||
           error.contains(QStringLiteral("continuation"), Qt::CaseInsensitive);
}

bool errorMentions(const QString& error, const QString& expected) {
    return error.contains(expected, Qt::CaseInsensitive);
}

void appendHevcArray(QByteArray* config, int nalType, const QByteArray& nal) {
    config->append(char(0x80 | nalType));
    config->append(char(0));
    config->append(char(1));
    config->append(char((nal.size() >> 8) & 0xff));
    config->append(char(nal.size() & 0xff));
    config->append(nal);
}

QByteArray hevcConfigWithParameterSets(bool includeUnknownArray = false) {
    const QByteArray vps = QByteArray::fromHex("40010c01ffff01600000030090000003000003005d959809");
    const QByteArray sps = QByteArray::fromHex("42010101600000030090000003000003005da00280802d1f");
    const QByteArray pps = QByteArray::fromHex("4401c172b46240");

    QByteArray config(23, char(0));
    config[0] = char(1);
    config[21] = char(0xfc | 3);                  // 4-byte NAL length
    config[22] = char(includeUnknownArray ? 4 : 3); // arrays
    if (includeUnknownArray) appendHevcArray(&config, 39, QByteArray::fromHex("4e01"));
    appendHevcArray(&config, 32, vps);
    appendHevcArray(&config, 33, sps);
    appendHevcArray(&config, 34, pps);
    return config;
}

bool readConnectObjectString(const QByteArray& payload, const QString& wantedKey, QString* value) {
    int offset = 0;
    QString command;
    double transactionId = 0;
    if (!RtmpAmf0::readString(payload, &offset, &command) ||
        !RtmpAmf0::readNumber(payload, &offset, &transactionId) || offset >= payload.size() ||
        uchar(payload[offset]) != 0x03) {
        return false;
    }
    ++offset;
    while (offset + 3 <= payload.size()) {
        if (uchar(payload[offset]) == 0 && uchar(payload[offset + 1]) == 0 &&
            uchar(payload[offset + 2]) == 0x09) {
            return false;
        }
        const int keySize =
            (int(uchar(payload[offset])) << 8) | int(uchar(payload[offset + 1]));
        offset += 2;
        if (offset + keySize > payload.size()) return false;
        const QString key = QString::fromUtf8(payload.constData() + offset, keySize);
        offset += keySize;
        if (key == wantedKey) {
            return RtmpAmf0::readString(payload, &offset, value);
        }
        if (!RtmpAmf0::skipValue(payload, &offset)) return false;
    }
    return false;
}
} // namespace

class TestRtmpProtocol : public QObject {
    Q_OBJECT
private slots:
    void amf0CommandRoundTripsNameAndTransaction();
    void amf0WritesStrictArrayForFourCcList();
    void rtmpUrlPartsPreserveSignedQueryInPlayPath();
    void rtmpUrlPartsPreserveEncodedPathAndQuery();
    void rtmpUrlPartsPreserveEmptyQueryMarker();
    void rtmpUrlPartsUseAppAsPlayPathWhenPathHasNoStream();
    void rtmpUrlPartsRejectHostOnlyUrls();
    void rtmpUrlRedactionForLogHidesQueryValues();
    void connectPayloadAdvertisesEnhancedCodecCapabilities();
    void connectPayloadDefaultProfileAdvertisesOnlyAvcAac();
    void amf0SkipsEcmaArrayMetadata();
    void amf0SkipsStrictArray();
    void amf0RejectsMalformedObjectKeyWithoutAdvancingOffset();
    void amf0RejectsMalformedEcmaArrayKeyWithoutAdvancingOffset();
    void amf0RejectsExcessiveNestingWithoutAdvancingOffset();
    void chunkParserReassemblesSplitMessageAndUpdatesChunkSize();
    void chunkParserDoesNotDoubleApplyTimestampDeltaWhenPayloadArrivesLater();
    void chunkParserConsumesExtendedTimestampOnContinuationChunks();
    void chunkParserRejectsFmtOneWithoutPreviousHeader();
    void chunkParserRejectsFmtTwoWithoutPreviousHeader();
    void chunkParserRejectsFmtThreeWithoutPreviousHeader();
    void chunkParserAdvancesTimestampForFmtThreeStartingSameHeaderMessage();
    void chunkParserRejectsUnsafeSetChunkSizeValues();
    void chunkParserAppliesFragmentedSetChunkSizeOnlyAfterCompletion();
    void chunkParserRejectsNewHeaderBeforeIncompleteAssemblyCompletes();
    void chunkParserRejectsNewFmtOneBeforeIncompleteAssemblyCompletes();
    void chunkParserRejectsNewFmtTwoBeforeIncompleteAssemblyCompletes();
    void chunkParserRejectsMessagesOverConfiguredLimit();
    void chunkParserRejectsBufferedBytesOverConfiguredLimit();
    void chunkParserRejectsAssemblyBytesOverConfiguredLimit();
    void chunkParserRejectsMalformedAbortPayloads();
    void chunkParserAbortClearsInFlightAssembly();
    void chunkParserFmtThreeAfterAbortDoesNotPoisonState();
    void chunkParserAbortForInactiveCsidDoesNotCreateTombstone();
    void chunkParserClearsAbortTombstoneAfterDiscardingRemainingBytes();
    void parsesEnhancedHevcSequenceStartHeader();
    void parsesEnhancedCodedFramesCompositionTime();
    void rejectsEnhancedVideoPacketWithoutFourCc();
    void rejectsEnhancedCodedFramesWithoutCompositionTime();
    void parsesEnhancedNegativeCompositionTime();
    void parsesEnhancedUnknownFourCcSafely();
    void parsesEnhancedMetadataAndMultitrack();
    void parsesEnhancedCodedFramesXWithoutCompositionTime();
    void parsesEnhancedSequenceEndWithoutCompositionTime();
    void parsesLegacyAvcVideoPacket();
    void parsesAvcSequenceHeaderAndConvertsNalusToAnnexB();
    void parsesHevcSequenceHeaderAndConvertsNalusToAnnexB();
    void parsesHevcSequenceHeaderIgnoringUnknownArrays();
    void rejectsMalformedHevcSequenceHeaders();
    void rejectsInvalidHevcSequenceHeaderVersion();
    void convertsVariableLengthPrefixedNalusToAnnexB();
    void rejectsMalformedLengthPrefixedNalus();
    void parsesAacSequenceHeaderAndBuildsAdtsFrame();
};

void TestRtmpProtocol::amf0CommandRoundTripsNameAndTransaction() {
    QByteArray payload;
    payload.append(RtmpAmf0::string(QStringLiteral("connect")));
    payload.append(RtmpAmf0::number(1));
    payload.append(RtmpAmf0::object({
        {QStringLiteral("app"), RtmpAmf0::string(QStringLiteral("live"))},
        {QStringLiteral("fpad"), RtmpAmf0::boolean(false)},
        {QStringLiteral("capabilities"), RtmpAmf0::number(15)},
    }));

    int offset = 0;
    QString command;
    double transactionId = 0;
    QVERIFY(RtmpAmf0::readString(payload, &offset, &command));
    QVERIFY(RtmpAmf0::readNumber(payload, &offset, &transactionId));
    QVERIFY(RtmpAmf0::skipValue(payload, &offset));

    QCOMPARE(command, QStringLiteral("connect"));
    QCOMPARE(transactionId, 1.0);
    QCOMPARE(offset, payload.size());
}

void TestRtmpProtocol::amf0WritesStrictArrayForFourCcList() {
    const QByteArray array = RtmpAmf0::strictArray({
        RtmpAmf0::string(QStringLiteral("avc1")),
        RtmpAmf0::string(QStringLiteral("hvc1")),
        RtmpAmf0::string(QStringLiteral("mp4a")),
    });

    QCOMPARE(uchar(array[0]), 0x0a);
    QCOMPARE(array.mid(1, 4).toHex(), QByteArray("00000003"));
    int offset = 5;
    QString value;
    QVERIFY(RtmpAmf0::readString(array, &offset, &value));
    QCOMPARE(value, QStringLiteral("avc1"));
    QVERIFY(RtmpAmf0::readString(array, &offset, &value));
    QCOMPARE(value, QStringLiteral("hvc1"));
    QVERIFY(RtmpAmf0::readString(array, &offset, &value));
    QCOMPARE(value, QStringLiteral("mp4a"));
    QCOMPARE(offset, array.size());
}

void TestRtmpProtocol::rtmpUrlPartsPreserveSignedQueryInPlayPath() {
    const RtmpUrlParts parts =
        RtmpUrlParts::fromUrl(QUrl(QStringLiteral("rtmps://host.example:443/live/cam1?token=abc&expires=123")));
    QCOMPARE(parts.app, QStringLiteral("live"));
    QCOMPARE(parts.playPath, QStringLiteral("cam1?token=abc&expires=123"));
    QCOMPARE(parts.tcUrl, QStringLiteral("rtmps://host.example:443/live"));
}

void TestRtmpProtocol::rtmpUrlPartsPreserveEncodedPathAndQuery() {
    const RtmpUrlParts parts =
        RtmpUrlParts::fromUrl(QUrl(QStringLiteral("rtmp://host/live/stream%2Fcam?token=a%2Fb&sig=x%3Dy")));
    QCOMPARE(parts.app, QStringLiteral("live"));
    QCOMPARE(parts.playPath, QStringLiteral("stream%2Fcam?token=a%2Fb&sig=x%3Dy"));
    QCOMPARE(parts.tcUrl, QStringLiteral("rtmp://host/live"));
}

void TestRtmpProtocol::rtmpUrlPartsPreserveEmptyQueryMarker() {
    const RtmpUrlParts parts =
        RtmpUrlParts::fromUrl(QUrl(QStringLiteral("rtmp://host/live/stream?")));
    QCOMPARE(parts.app, QStringLiteral("live"));
    QCOMPARE(parts.playPath, QStringLiteral("stream?"));
    QCOMPARE(parts.tcUrl, QStringLiteral("rtmp://host/live"));
}

void TestRtmpProtocol::rtmpUrlPartsUseAppAsPlayPathWhenPathHasNoStream() {
    const RtmpUrlParts parts =
        RtmpUrlParts::fromUrl(QUrl(QStringLiteral("rtmp://host/live?token=abc")));
    QCOMPARE(parts.app, QStringLiteral("live"));
    QCOMPARE(parts.playPath, QStringLiteral("live?token=abc"));
    QCOMPARE(parts.tcUrl, QStringLiteral("rtmp://host/live"));
}

void TestRtmpProtocol::rtmpUrlPartsRejectHostOnlyUrls() {
    const RtmpUrlParts parts = RtmpUrlParts::fromUrl(QUrl(QStringLiteral("rtmp://host")));
    QVERIFY(!parts.isValid());
}

void TestRtmpProtocol::rtmpUrlRedactionForLogHidesQueryValues() {
    const QString redacted = RtmpUrlParts::redactedForLog(
        QUrl(QStringLiteral("rtmp://host.example:1935/live/stream%2Fcam?token=abc&sig=x%3Dy")));
    QCOMPARE(redacted, QStringLiteral("rtmp://host.example:1935/live/stream%2Fcam?<redacted>"));
    QVERIFY(!redacted.contains(QStringLiteral("abc")));
    QVERIFY(!redacted.contains(QStringLiteral("x%3Dy")));

    const QString redactedUserInfo = RtmpUrlParts::redactedForLog(
        QUrl(QStringLiteral("rtmp://user:pass@host/live/stream?token=abc#frag")));
    QCOMPARE(redactedUserInfo, QStringLiteral("rtmp://host/live/stream?<redacted>"));
    QVERIFY(!redactedUserInfo.contains(QStringLiteral("user")));
    QVERIFY(!redactedUserInfo.contains(QStringLiteral("pass")));
    QVERIFY(!redactedUserInfo.contains(QStringLiteral("token")));
    QVERIFY(!redactedUserInfo.contains(QStringLiteral("abc")));
    QVERIFY(!redactedUserInfo.contains(QStringLiteral("frag")));

    QCOMPARE(RtmpUrlParts::redactedForLog(QUrl(QStringLiteral("rtmp://host/live/stream"))),
             QStringLiteral("rtmp://host/live/stream"));
}

void TestRtmpProtocol::connectPayloadAdvertisesEnhancedCodecCapabilities() {
    const QByteArray payload =
        RtmpAmf0::connectCommandPayload(QUrl(QStringLiteral("rtmp://127.0.0.1/live/stream")),
                                        RtmpConnectCodecProfile::EnhancedAvcHevcAac);
    QVERIFY(payload.contains("fourCcList"));
    QVERIFY(payload.contains("avc1"));
    QVERIFY(payload.contains("hvc1"));
    QVERIFY(payload.contains("mp4a"));
    QVERIFY(payload.contains("videoFourCcInfoMap"));
    QVERIFY(payload.contains("audioFourCcInfoMap"));
}

void TestRtmpProtocol::connectPayloadDefaultProfileAdvertisesOnlyAvcAac() {
    const QByteArray payload =
        RtmpAmf0::connectCommandPayload(QUrl(QStringLiteral("rtmp://127.0.0.1/live/stream")));

    int offset = 0;
    QString command;
    double transactionId = 0;
    QVERIFY(RtmpAmf0::readString(payload, &offset, &command));
    QVERIFY(RtmpAmf0::readNumber(payload, &offset, &transactionId));
    QCOMPARE(command, QStringLiteral("connect"));
    QCOMPARE(transactionId, 1.0);

    QString value;
    QVERIFY(readConnectObjectString(payload, QStringLiteral("app"), &value));
    QCOMPARE(value, QStringLiteral("live"));
    QVERIFY(readConnectObjectString(payload, QStringLiteral("tcUrl"), &value));
    QCOMPARE(value, QStringLiteral("rtmp://127.0.0.1/live"));
    QVERIFY(payload.contains("fourCcList"));
    QVERIFY(payload.contains("avc1"));
    QVERIFY(payload.contains("mp4a"));
    QVERIFY(!payload.contains("hvc1"));
}

void TestRtmpProtocol::amf0SkipsEcmaArrayMetadata() {
    QByteArray metadata;
    metadata.append(char(0x08));
    metadata.append(QByteArray::fromHex("00000002"));
    metadata.append(QByteArray::fromHex("0005"));
    metadata.append("width", 5);
    metadata.append(RtmpAmf0::number(1920));
    metadata.append(QByteArray::fromHex("000c"));
    metadata.append("videocodecid", 12);
    metadata.append(RtmpAmf0::string(QStringLiteral("hvc1")));
    metadata.append("\0\0\x09", 3);

    int offset = 0;
    QVERIFY(RtmpAmf0::skipValue(metadata, &offset));
    QCOMPARE(offset, metadata.size());
}

void TestRtmpProtocol::amf0SkipsStrictArray() {
    const QByteArray array = RtmpAmf0::strictArray({
        RtmpAmf0::string(QStringLiteral("avc1")),
        RtmpAmf0::number(1),
        RtmpAmf0::boolean(true),
        RtmpAmf0::nullValue(),
    });

    int offset = 0;
    QVERIFY(RtmpAmf0::skipValue(array, &offset));
    QCOMPARE(offset, array.size());
}

void TestRtmpProtocol::amf0RejectsMalformedObjectKeyWithoutAdvancingOffset() {
    QByteArray object;
    object.append(char(0x03));
    object.append(QByteArray::fromHex("0005"));
    object.append("ab", 2);

    int offset = 0;
    QVERIFY(!RtmpAmf0::skipValue(object, &offset));
    QCOMPARE(offset, 0);
}

void TestRtmpProtocol::amf0RejectsMalformedEcmaArrayKeyWithoutAdvancingOffset() {
    QByteArray ecmaArray;
    ecmaArray.append(char(0x08));
    ecmaArray.append(QByteArray::fromHex("00000001"));
    ecmaArray.append(QByteArray::fromHex("0005"));
    ecmaArray.append("ab", 2);

    int offset = 0;
    QVERIFY(!RtmpAmf0::skipValue(ecmaArray, &offset));
    QCOMPARE(offset, 0);
}

void TestRtmpProtocol::amf0RejectsExcessiveNestingWithoutAdvancingOffset() {
    QByteArray value = RtmpAmf0::nullValue();
    for (int i = 0; i < 130; ++i) {
        value = RtmpAmf0::strictArray({value});
    }

    int offset = 0;
    QVERIFY(!RtmpAmf0::skipValue(value, &offset));
    QCOMPARE(offset, 0);
}

void TestRtmpProtocol::chunkParserReassemblesSplitMessageAndUpdatesChunkSize() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray setChunkSize = RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("00000004"), 128);
    QVERIFY(parser.push(setChunkSize, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.takeFirst().type, 1);

    const QByteArray payload("abcdefghi", 9);
    const QByteArray chunks = RtmpChunkWriter::message(3, 20, 1, 12, payload, 4);
    QVERIFY(parser.push(chunks.left(7), &messages, &error));
    QVERIFY(messages.isEmpty());
    QVERIFY(parser.push(chunks.mid(7), &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().type, 20);
    QCOMPARE(messages.first().streamId, 1);
    QCOMPARE(messages.first().timestampMs, 12);
    QCOMPARE(messages.first().payload, payload);
}

void TestRtmpProtocol::chunkParserDoesNotDoubleApplyTimestampDeltaWhenPayloadArrivesLater() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray first =
        RtmpChunkWriter::message(6, 9, 1, 1000, QByteArray("abcd", 4), 128);
    QVERIFY(parser.push(first, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, 1000);

    QByteArray second;
    second.append(char((1 << 6) | 6)); // fmt=1, csid=6
    second.append(QByteArray::fromHex("000028")); // timestamp delta 40
    second.append(QByteArray::fromHex("000004")); // payload length 4
    second.append(char(9));                       // message type video
    second.append("xy", 2);                       // incomplete payload

    QVERIFY(parser.push(second, &messages, &error));
    QVERIFY(messages.isEmpty());

    QVERIFY(parser.push(QByteArray("zz", 2), &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, 1040);
    QCOMPARE(messages.first().payload, QByteArray("xyzz", 4));
}

void TestRtmpProtocol::chunkParserConsumesExtendedTimestampOnContinuationChunks() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray setChunkSize =
        RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("00000003"), 128);
    QVERIFY(parser.push(setChunkSize, &messages, &error));
    QCOMPARE(messages.size(), 1);
    messages.clear();

    const QByteArray payload("abcdef", 6);
    const QByteArray chunks = RtmpChunkWriter::message(6, 9, 1, 0x1000000, payload, 3);

    QVERIFY(parser.push(chunks, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, qint64(0x1000000));
    QCOMPARE(messages.first().payload, payload);
}

void TestRtmpProtocol::chunkParserRejectsFmtOneWithoutPreviousHeader() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    QByteArray chunk;
    chunk.append(char((1 << 6) | 6)); // fmt=1, csid=6
    appendU24(&chunk, 40);
    appendU24(&chunk, 1);
    chunk.append(char(9));
    chunk.append("x", 1);

    QVERIFY(!parser.push(chunk, &messages, &error));
    QVERIFY(errorMentionsPreviousHeader(error));
    QVERIFY(messages.isEmpty());
}

void TestRtmpProtocol::chunkParserRejectsFmtTwoWithoutPreviousHeader() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    QByteArray chunk;
    chunk.append(char((2 << 6) | 6)); // fmt=2, csid=6
    appendU24(&chunk, 40);

    QVERIFY(!parser.push(chunk, &messages, &error));
    QVERIFY(errorMentionsPreviousHeader(error));
    QVERIFY(messages.isEmpty());
}

void TestRtmpProtocol::chunkParserRejectsFmtThreeWithoutPreviousHeader() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    QByteArray chunk;
    chunk.append(char((3 << 6) | 6)); // fmt=3, csid=6
    chunk.append("x", 1);

    QVERIFY(!parser.push(chunk, &messages, &error));
    QVERIFY(errorMentionsPreviousHeader(error));
    QVERIFY(messages.isEmpty());
}

void TestRtmpProtocol::chunkParserAdvancesTimestampForFmtThreeStartingSameHeaderMessage() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray first =
        RtmpChunkWriter::message(6, 9, 1, 1000, QByteArray("aa", 2), 128);
    QVERIFY(parser.push(first, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, 1000);

    QByteArray second;
    second.append(char((1 << 6) | 6)); // fmt=1, csid=6
    appendU24(&second, 40);
    appendU24(&second, 2);
    second.append(char(9));
    second.append("bb", 2);

    QVERIFY(parser.push(second, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, 1040);

    QByteArray third;
    third.append(char((3 << 6) | 6)); // fmt=3, csid=6
    third.append("cc", 2);

    QVERIFY(parser.push(third, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, 1080);
    QCOMPARE(messages.first().payload, QByteArray("cc", 2));
}

void TestRtmpProtocol::chunkParserRejectsUnsafeSetChunkSizeValues() {
    {
        RtmpChunkParser parser;
        QList<RtmpMessage> messages;
        QString error;

        const QByteArray chunkSizeZero =
            RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("00000000"), 128);

        QVERIFY(!parser.push(chunkSizeZero, &messages, &error));
        QVERIFY(error.contains(QStringLiteral("chunk size"), Qt::CaseInsensitive));
        QCOMPARE(parser.inputChunkSize(), 128);
    }

    {
        RtmpChunkParser parser;
        QList<RtmpMessage> messages;
        QString error;

        const QByteArray chunkSizeHighBit =
            RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("80000000"), 128);

        QVERIFY(!parser.push(chunkSizeHighBit, &messages, &error));
        QVERIFY(error.contains(QStringLiteral("chunk size"), Qt::CaseInsensitive));
        QCOMPARE(parser.inputChunkSize(), 128);
    }

    {
        RtmpChunkParser parser;
        QList<RtmpMessage> messages;
        QString error;

        const QByteArray shortChunkSize =
            RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("000004"), 128);

        QVERIFY(!parser.push(shortChunkSize, &messages, &error));
        QVERIFY(error.contains(QStringLiteral("malformed"), Qt::CaseInsensitive));
        QVERIFY(error.contains(QStringLiteral("chunk size"), Qt::CaseInsensitive));
        QCOMPARE(parser.inputChunkSize(), 128);
    }

    {
        RtmpChunkParser parser;
        QList<RtmpMessage> messages;
        QString error;

        const QByteArray longChunkSize =
            RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("0000000400"), 128);

        QVERIFY(!parser.push(longChunkSize, &messages, &error));
        QVERIFY(error.contains(QStringLiteral("malformed"), Qt::CaseInsensitive));
        QVERIFY(error.contains(QStringLiteral("chunk size"), Qt::CaseInsensitive));
        QCOMPARE(parser.inputChunkSize(), 128);
    }
}

void TestRtmpProtocol::chunkParserAppliesFragmentedSetChunkSizeOnlyAfterCompletion() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray setChunkSize =
        RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("00000004"), 128);

    QVERIFY(parser.push(setChunkSize.left(setChunkSize.size() - 1), &messages, &error));
    QVERIFY(messages.isEmpty());
    QCOMPARE(parser.inputChunkSize(), 128);

    QVERIFY(parser.push(setChunkSize.right(1), &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().type, 1);
    QCOMPARE(parser.inputChunkSize(), 4);
}

void TestRtmpProtocol::chunkParserRejectsNewHeaderBeforeIncompleteAssemblyCompletes() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray setChunkSize =
        RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("00000002"), 128);
    QVERIFY(parser.push(setChunkSize, &messages, &error));
    QCOMPARE(messages.size(), 1);

    const QByteArray first = RtmpChunkWriter::message(6, 9, 1, 1000, QByteArray("xyzz", 4), 2);
    const int firstFragmentSize = 1 + 11 + 2;
    QVERIFY(parser.push(first.left(firstFragmentSize), &messages, &error));
    QVERIFY(messages.isEmpty());

    const QByteArray second = RtmpChunkWriter::message(6, 9, 1, 2000, QByteArray("abcd", 4), 128);
    QVERIFY(!parser.push(second, &messages, &error));
    QVERIFY(error.contains(QStringLiteral("incomplete"), Qt::CaseInsensitive));
}

void TestRtmpProtocol::chunkParserRejectsNewFmtOneBeforeIncompleteAssemblyCompletes() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray setChunkSize =
        RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("00000002"), 128);
    QVERIFY(parser.push(setChunkSize, &messages, &error));
    QCOMPARE(messages.size(), 1);

    const QByteArray previous = RtmpChunkWriter::message(6, 9, 1, 1000, QByteArray("aa", 2), 2);
    QVERIFY(parser.push(previous, &messages, &error));
    QCOMPARE(messages.size(), 1);

    const QByteArray incomplete =
        RtmpChunkWriter::message(6, 9, 1, 1040, QByteArray("xyzz", 4), 2);
    const int firstFragmentSize = 1 + 11 + 2;
    QVERIFY(parser.push(incomplete.left(firstFragmentSize), &messages, &error));
    QVERIFY(messages.isEmpty());

    QByteArray fmtOne;
    fmtOne.append(char((1 << 6) | 6)); // fmt=1, csid=6
    appendU24(&fmtOne, 40);
    appendU24(&fmtOne, 2);
    fmtOne.append(char(9));
    fmtOne.append("bb", 2);

    QVERIFY(!parser.push(fmtOne, &messages, &error));
    QVERIFY(error.contains(QStringLiteral("incomplete"), Qt::CaseInsensitive));
}

void TestRtmpProtocol::chunkParserRejectsNewFmtTwoBeforeIncompleteAssemblyCompletes() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray setChunkSize =
        RtmpChunkWriter::message(2, 1, 0, 0, QByteArray::fromHex("00000002"), 128);
    QVERIFY(parser.push(setChunkSize, &messages, &error));
    QCOMPARE(messages.size(), 1);

    const QByteArray previous = RtmpChunkWriter::message(6, 9, 1, 1000, QByteArray("aa", 2), 2);
    QVERIFY(parser.push(previous, &messages, &error));
    QCOMPARE(messages.size(), 1);

    QByteArray fmtOne;
    fmtOne.append(char((1 << 6) | 6)); // fmt=1, csid=6
    appendU24(&fmtOne, 40);
    appendU24(&fmtOne, 4);
    fmtOne.append(char(9));
    fmtOne.append("xy", 2);
    QVERIFY(parser.push(fmtOne, &messages, &error));
    QVERIFY(messages.isEmpty());

    QByteArray fmtTwo;
    fmtTwo.append(char((2 << 6) | 6)); // fmt=2, csid=6
    appendU24(&fmtTwo, 40);
    fmtTwo.append("bb", 2);

    QVERIFY(!parser.push(fmtTwo, &messages, &error));
    QVERIFY(error.contains(QStringLiteral("incomplete"), Qt::CaseInsensitive));
}

void TestRtmpProtocol::chunkParserRejectsMessagesOverConfiguredLimit() {
    RtmpChunkParser parser;
    parser.setMaxMessageSize(4);
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray bytes =
        RtmpChunkWriter::message(6, 9, 1, 0, QByteArray("abcde", 5), 128);
    QVERIFY(!parser.push(bytes, &messages, &error));
    QVERIFY(error.contains(QStringLiteral("exceeds")));
}

void TestRtmpProtocol::chunkParserRejectsBufferedBytesOverConfiguredLimit() {
    RtmpChunkParser parser;
    parser.setMaxBufferedBytes(4);
    QList<RtmpMessage> messages;
    QString error;

    QVERIFY(!parser.push(QByteArray("abcde", 5), &messages, &error));
    QVERIFY(error.contains(QStringLiteral("buffered"), Qt::CaseInsensitive));
    QVERIFY(error.contains(QStringLiteral("limit"), Qt::CaseInsensitive));
}

void TestRtmpProtocol::chunkParserRejectsAssemblyBytesOverConfiguredLimit() {
    RtmpChunkParser parser;
    parser.setInputChunkSizeForTest(2);
    parser.setMaxAssemblyBytes(3);
    QList<RtmpMessage> messages;
    QString error;

    const QByteArray first = RtmpChunkWriter::message(6, 9, 1, 0, QByteArray("abcd", 4), 2);
    QVERIFY(parser.push(first.left(14), &messages, &error));
    QVERIFY(messages.isEmpty());

    const QByteArray second = RtmpChunkWriter::message(7, 9, 1, 0, QByteArray("wxyz", 4), 2);
    QVERIFY(!parser.push(second.left(14), &messages, &error));
    QVERIFY(error.contains(QStringLiteral("exceed"), Qt::CaseInsensitive));
    QVERIFY(error.contains(QStringLiteral("limit"), Qt::CaseInsensitive));
}

void TestRtmpProtocol::chunkParserRejectsMalformedAbortPayloads() {
    {
        RtmpChunkParser parser;
        QList<RtmpMessage> messages;
        QString error;

        const QByteArray abort =
            RtmpChunkWriter::message(2, 2, 0, 0, QByteArray::fromHex("000006"), 128);

        QVERIFY(!parser.push(abort, &messages, &error));
        QVERIFY(error.contains(QStringLiteral("abort"), Qt::CaseInsensitive));
        QVERIFY(error.contains(QStringLiteral("malformed"), Qt::CaseInsensitive));
    }

    {
        RtmpChunkParser parser;
        QList<RtmpMessage> messages;
        QString error;

        const QByteArray abort =
            RtmpChunkWriter::message(2, 2, 0, 0, QByteArray::fromHex("0000000600"), 128);

        QVERIFY(!parser.push(abort, &messages, &error));
        QVERIFY(error.contains(QStringLiteral("abort"), Qt::CaseInsensitive));
        QVERIFY(error.contains(QStringLiteral("malformed"), Qt::CaseInsensitive));
    }
}

void TestRtmpProtocol::chunkParserAbortClearsInFlightAssembly() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    parser.setInputChunkSizeForTest(2);
    const QByteArray video = RtmpChunkWriter::message(6, 9, 1, 0, QByteArray("abcdef", 6), 2);
    QVERIFY(parser.push(video.left(14), &messages, &error));
    QVERIFY(messages.isEmpty());

    QByteArray abortPayload;
    abortPayload.append(char(0));
    abortPayload.append(char(0));
    abortPayload.append(char(0));
    abortPayload.append(char(6));
    const QByteArray abort = RtmpChunkWriter::message(2, 2, 0, 0, abortPayload, 2);
    QVERIFY(parser.push(abort, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().type, 2);

    QVERIFY(parser.push(video.mid(14), &messages, &error));
    QVERIFY(messages.isEmpty());

    const QByteArray fresh = RtmpChunkWriter::message(6, 9, 1, 100, QByteArray("xy", 2), 2);
    QVERIFY(parser.push(fresh, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().type, 9);
    QCOMPARE(messages.first().timestampMs, qint64(100));
    QCOMPARE(messages.first().payload, QByteArray("xy", 2));
}

void TestRtmpProtocol::chunkParserFmtThreeAfterAbortDoesNotPoisonState() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    parser.setInputChunkSizeForTest(2);
    const QByteArray video = RtmpChunkWriter::message(6, 9, 1, 0, QByteArray("abcdef", 6), 2);
    QVERIFY(parser.push(video.left(14), &messages, &error));
    QVERIFY(messages.isEmpty());

    const QByteArray abort = RtmpChunkWriter::message(2, 2, 0, 0, QByteArray::fromHex("00000006"), 2);
    QVERIFY(parser.push(abort, &messages, &error));
    QCOMPARE(messages.size(), 1);

    QVERIFY(parser.push(video.mid(14, 3), &messages, &error));
    QVERIFY(messages.isEmpty());

    const QByteArray fresh = RtmpChunkWriter::message(6, 9, 1, 40, QByteArray("zz", 2), 2);
    QVERIFY(parser.push(fresh, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().timestampMs, qint64(40));
    QCOMPARE(messages.first().payload, QByteArray("zz", 2));
}

void TestRtmpProtocol::chunkParserAbortForInactiveCsidDoesNotCreateTombstone() {
    {
        RtmpChunkParser parser;
        QList<RtmpMessage> messages;
        QString error;

        const QByteArray abort =
            RtmpChunkWriter::message(2, 2, 0, 0, QByteArray::fromHex("00000006"), 128);
        QVERIFY(parser.push(abort, &messages, &error));
        QCOMPARE(messages.size(), 1);

        QByteArray fmtThree;
        fmtThree.append(char((3 << 6) | 6));
        fmtThree.append("x", 1);

        QVERIFY(!parser.push(fmtThree, &messages, &error));
        QVERIFY(errorMentionsPreviousHeader(error));
    }

    {
        RtmpChunkParser parser;
        QList<RtmpMessage> messages;
        QString error;

        const QByteArray first =
            RtmpChunkWriter::message(6, 9, 1, 0, QByteArray("aa", 2), 128);
        QVERIFY(parser.push(first, &messages, &error));
        QCOMPARE(messages.size(), 1);

        const QByteArray abort =
            RtmpChunkWriter::message(2, 2, 0, 0, QByteArray::fromHex("00000006"), 128);
        QVERIFY(parser.push(abort, &messages, &error));
        QCOMPARE(messages.size(), 1);

        QByteArray fmtThree;
        fmtThree.append(char((3 << 6) | 6));
        fmtThree.append("bb", 2);

        QVERIFY(parser.push(fmtThree, &messages, &error));
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().type, 9);
        QCOMPARE(messages.first().payload, QByteArray("bb", 2));

        const QByteArray fresh =
            RtmpChunkWriter::message(6, 9, 1, 20, QByteArray("cc", 2), 128);
        QVERIFY(parser.push(fresh, &messages, &error));
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().timestampMs, qint64(20));
        QCOMPARE(messages.first().payload, QByteArray("cc", 2));
    }
}

void TestRtmpProtocol::chunkParserClearsAbortTombstoneAfterDiscardingRemainingBytes() {
    RtmpChunkParser parser;
    QList<RtmpMessage> messages;
    QString error;

    parser.setInputChunkSizeForTest(2);
    const QByteArray video = RtmpChunkWriter::message(6, 9, 1, 0, QByteArray("abcd", 4), 2);
    QVERIFY(parser.push(video.left(14), &messages, &error));
    QVERIFY(messages.isEmpty());

    const QByteArray abort =
        RtmpChunkWriter::message(2, 2, 0, 0, QByteArray::fromHex("00000006"), 2);
    QVERIFY(parser.push(abort, &messages, &error));
    QCOMPARE(messages.size(), 1);

    QVERIFY(parser.push(video.mid(14), &messages, &error));
    QVERIFY(messages.isEmpty());

    QByteArray fmtThreeFirst;
    fmtThreeFirst.append(char((3 << 6) | 6));
    fmtThreeFirst.append("zz", 2);
    QVERIFY(parser.push(fmtThreeFirst, &messages, &error));
    QVERIFY(messages.isEmpty());

    QByteArray fmtThreeSecond;
    fmtThreeSecond.append(char((3 << 6) | 6));
    fmtThreeSecond.append("qq", 2);
    QVERIFY(parser.push(fmtThreeSecond, &messages, &error));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().payload, QByteArray("zzqq", 4));
}

void TestRtmpProtocol::parsesEnhancedHevcSequenceStartHeader() {
    QByteArray payload;
    payload.append(char(0x80 | 0)); // enhanced + SequenceStart
    payload.append("hvc1", 4);
    payload.append("CONFIG", 6);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::Enhanced);
    QCOMPARE(packet.codec, NativeVideoCodec::Hevc);
    QCOMPARE(packet.enhancedType, RtmpEnhancedVideoPacketType::SequenceStart);
    QCOMPARE(packet.fourCc, QStringLiteral("hvc1"));
    QCOMPARE(packet.codecPayload, QByteArray("CONFIG", 6));
}

void TestRtmpProtocol::parsesEnhancedCodedFramesCompositionTime() {
    QByteArray payload;
    payload.append(char(0x80 | 1)); // enhanced + CodedFrames
    payload.append("avc1", 4);
    payload.append(QByteArray::fromHex("00002a")); // composition time 42
    payload.append("FRAME", 5);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::Enhanced);
    QCOMPARE(packet.codec, NativeVideoCodec::H264);
    QCOMPARE(packet.enhancedType, RtmpEnhancedVideoPacketType::CodedFrames);
    QCOMPARE(packet.fourCc, QStringLiteral("avc1"));
    QCOMPARE(packet.compositionTimeMs, 42);
    QCOMPARE(packet.codecPayload, QByteArray("FRAME", 5));
}

void TestRtmpProtocol::rejectsEnhancedVideoPacketWithoutFourCc() {
    const QList<QByteArray> payloads = {
        QByteArray(1, char(0x80 | 0)),
        QByteArray(1, char(0x80 | 0)) + QByteArray("hvc", 3),
    };

    for (const QByteArray& payload : payloads) {
        RtmpVideoPacket packet;
        QString error;
        QVERIFY(!RtmpFlv::parseVideoPacket(payload, &packet, &error));
        QVERIFY(errorMentions(error, QStringLiteral("FourCC")));
        QVERIFY(errorMentions(error, QStringLiteral("malformed")));
    }
}

void TestRtmpProtocol::rejectsEnhancedCodedFramesWithoutCompositionTime() {
    QByteArray payload;
    payload.append(char(0x80 | 1)); // enhanced + CodedFrames
    payload.append("avc1", 4);
    payload.append(QByteArray::fromHex("0000"));

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(!RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QVERIFY(errorMentions(error, QStringLiteral("composition")));
    QVERIFY(errorMentions(error, QStringLiteral("malformed")));
}

void TestRtmpProtocol::parsesEnhancedNegativeCompositionTime() {
    QByteArray payload;
    payload.append(char(0x80 | 1)); // enhanced + CodedFrames
    payload.append("avc1", 4);
    payload.append(QByteArray::fromHex("ffffd6")); // composition time -42
    payload.append("FRAME", 5);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::Enhanced);
    QCOMPARE(packet.codec, NativeVideoCodec::H264);
    QCOMPARE(packet.enhancedType, RtmpEnhancedVideoPacketType::CodedFrames);
    QCOMPARE(packet.compositionTimeMs, -42);
    QCOMPARE(packet.codecPayload, QByteArray("FRAME", 5));
}

void TestRtmpProtocol::parsesEnhancedUnknownFourCcSafely() {
    QByteArray payload;
    payload.append(char(0x80 | 3)); // enhanced + CodedFramesX
    payload.append("zzzz", 4);
    payload.append("FRAME", 5);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::Enhanced);
    QCOMPARE(packet.codec, NativeVideoCodec::Unknown);
    QCOMPARE(packet.enhancedType, RtmpEnhancedVideoPacketType::CodedFramesX);
    QCOMPARE(packet.fourCc, QStringLiteral("zzzz"));
    QCOMPARE(packet.compositionTimeMs, 0);
    QCOMPARE(packet.codecPayload, QByteArray("FRAME", 5));
}

void TestRtmpProtocol::parsesEnhancedMetadataAndMultitrack() {
    const QList<int> packetTypes = {
        int(RtmpEnhancedVideoPacketType::Metadata),
        int(RtmpEnhancedVideoPacketType::Multitrack),
    };

    for (int packetType : packetTypes) {
        QByteArray payload;
        payload.append(char(0x80 | packetType));
        payload.append("avc1", 4);
        payload.append("DATA", 4);

        RtmpVideoPacket packet;
        QString error;
        QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
        QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::Enhanced);
        QCOMPARE(packet.codec, NativeVideoCodec::H264);
        QCOMPARE(packet.enhancedType, static_cast<RtmpEnhancedVideoPacketType>(packetType));
        QCOMPARE(packet.compositionTimeMs, 0);
        QCOMPARE(packet.codecPayload, QByteArray("DATA", 4));
    }
}

void TestRtmpProtocol::parsesEnhancedCodedFramesXWithoutCompositionTime() {
    QByteArray payload;
    payload.append(char(0x80 | 3)); // enhanced + CodedFramesX
    payload.append("hvc1", 4);
    payload.append("FRAME", 5);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::Enhanced);
    QCOMPARE(packet.codec, NativeVideoCodec::Hevc);
    QCOMPARE(packet.enhancedType, RtmpEnhancedVideoPacketType::CodedFramesX);
    QCOMPARE(packet.compositionTimeMs, 0);
    QCOMPARE(packet.codecPayload, QByteArray("FRAME", 5));
}

void TestRtmpProtocol::parsesEnhancedSequenceEndWithoutCompositionTime() {
    QByteArray payload;
    payload.append(char(0x80 | 2)); // enhanced + SequenceEnd
    payload.append("hvc1", 4);
    payload.append("END", 3);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::Enhanced);
    QCOMPARE(packet.codec, NativeVideoCodec::Hevc);
    QCOMPARE(packet.enhancedType, RtmpEnhancedVideoPacketType::SequenceEnd);
    QCOMPARE(packet.compositionTimeMs, 0);
    QCOMPARE(packet.codecPayload, QByteArray("END", 3));
}

void TestRtmpProtocol::parsesLegacyAvcVideoPacket() {
    QByteArray payload;
    payload.append(char((1 << 4) | 7)); // keyframe + AVC
    payload.append(char(1));            // AVC NALU
    payload.append(QByteArray::fromHex("00002a"));
    payload.append("FRAME", 5);

    RtmpVideoPacket packet;
    QString error;
    QVERIFY(RtmpFlv::parseVideoPacket(payload, &packet, &error));
    QCOMPARE(packet.flavor, RtmpVideoPacketFlavor::LegacyAvc);
    QCOMPARE(packet.codec, NativeVideoCodec::H264);
    QCOMPARE(packet.enhancedType, RtmpEnhancedVideoPacketType::CodedFrames);
    QCOMPARE(packet.compositionTimeMs, 42);
    QCOMPARE(packet.trackId, 0);
    QCOMPARE(packet.codecPayload, QByteArray("FRAME", 5));
}

void TestRtmpProtocol::parsesAvcSequenceHeaderAndConvertsNalusToAnnexB() {
    const QByteArray sps = QByteArray::fromHex("6742c01eda014016ec0440000003004000000f03c60c6580");
    const QByteArray pps = QByteArray::fromHex("68ce06e2");

    QByteArray header;
    header.append(char(1));
    header.append(char(0x42));
    header.append(char(0xc0));
    header.append(char(0x1e));
    header.append(char(0xff)); // 4-byte NAL length
    header.append(char(0xe1)); // one SPS
    header.append(char((sps.size() >> 8) & 0xff));
    header.append(char(sps.size() & 0xff));
    header.append(sps);
    header.append(char(1)); // one PPS
    header.append(char((pps.size() >> 8) & 0xff));
    header.append(char(pps.size() & 0xff));
    header.append(pps);

    RtmpAvcConfig config;
    QString error;
    QVERIFY(RtmpFlv::parseAvcSequenceHeader(header, &config, &error));
    QCOMPARE(config.nalLengthSize, 4);
    QCOMPARE(config.parameterSets.h264Sps, QList<QByteArray>{sps});
    QCOMPARE(config.parameterSets.h264Pps, QList<QByteArray>{pps});

    const QByteArray nal = QByteArray::fromHex("65888421");
    QByteArray lengthPrefixed;
    lengthPrefixed.append(char(0));
    lengthPrefixed.append(char(0));
    lengthPrefixed.append(char(0));
    lengthPrefixed.append(char(nal.size()));
    lengthPrefixed.append(nal);

    QCOMPARE(RtmpFlv::avcPayloadToAnnexB(lengthPrefixed, 4), QByteArray::fromHex("0000000165888421"));
}

void TestRtmpProtocol::parsesHevcSequenceHeaderAndConvertsNalusToAnnexB() {
    const QByteArray vps = QByteArray::fromHex("40010c01ffff01600000030090000003000003005d959809");
    const QByteArray sps = QByteArray::fromHex("42010101600000030090000003000003005da00280802d1f");
    const QByteArray pps = QByteArray::fromHex("4401c172b46240");

    const QByteArray config = hevcConfigWithParameterSets();

    RtmpHevcConfig parsed;
    QString error;
    QVERIFY(RtmpFlv::parseHevcSequenceHeader(config, &parsed, &error));
    QCOMPARE(parsed.nalLengthSize, 4);
    QCOMPARE(parsed.parameterSets.hevcVps, QList<QByteArray>{vps});
    QCOMPARE(parsed.parameterSets.hevcSps, QList<QByteArray>{sps});
    QCOMPARE(parsed.parameterSets.hevcPps, QList<QByteArray>{pps});

    QByteArray frame;
    frame.append(QByteArray::fromHex("00000002"));
    frame.append(QByteArray::fromHex("2601"));
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(frame, 4),
             QByteArray::fromHex("000000012601"));
}

void TestRtmpProtocol::parsesHevcSequenceHeaderIgnoringUnknownArrays() {
    RtmpHevcConfig parsed;
    QString error;
    QVERIFY(RtmpFlv::parseHevcSequenceHeader(hevcConfigWithParameterSets(true), &parsed, &error));
    QCOMPARE(parsed.parameterSets.hevcVps.size(), 1);
    QCOMPARE(parsed.parameterSets.hevcSps.size(), 1);
    QCOMPARE(parsed.parameterSets.hevcPps.size(), 1);
}

void TestRtmpProtocol::rejectsMalformedHevcSequenceHeaders() {
    RtmpHevcConfig parsed;
    QString error;
    QVERIFY(!RtmpFlv::parseHevcSequenceHeader(QByteArray(22, char(0)), &parsed, &error));
    QVERIFY(errorMentions(error, QStringLiteral("malformed")));

    QByteArray missingParameterSets(23, char(0));
    missingParameterSets[0] = char(1);
    missingParameterSets[21] = char(0xfc | 3);
    missingParameterSets[22] = char(0);
    error.clear();
    QVERIFY(!RtmpFlv::parseHevcSequenceHeader(missingParameterSets, &parsed, &error));
    QVERIFY(errorMentions(error, QStringLiteral("VPS/SPS/PPS")));

    QByteArray truncatedArray(23, char(0));
    truncatedArray[0] = char(1);
    truncatedArray[21] = char(0xfc | 3);
    truncatedArray[22] = char(1);
    truncatedArray.append(char(0x80 | 32));
    truncatedArray.append(char(0));
    error.clear();
    QVERIFY(!RtmpFlv::parseHevcSequenceHeader(truncatedArray, &parsed, &error));
    QVERIFY(errorMentions(error, QStringLiteral("truncated")));
}

void TestRtmpProtocol::rejectsInvalidHevcSequenceHeaderVersion() {
    QByteArray config = hevcConfigWithParameterSets();
    config[0] = char(2);

    RtmpHevcConfig parsed;
    QString error;
    QVERIFY(!RtmpFlv::parseHevcSequenceHeader(config, &parsed, &error));
    QVERIFY(errorMentions(error, QStringLiteral("version")));
}

void TestRtmpProtocol::convertsVariableLengthPrefixedNalusToAnnexB() {
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(QByteArray::fromHex("02aabb"), 1),
             QByteArray::fromHex("00000001aabb"));
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(QByteArray::fromHex("0002aabb"), 2),
             QByteArray::fromHex("00000001aabb"));
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(QByteArray::fromHex("000002aabb"), 3),
             QByteArray::fromHex("00000001aabb"));
}

void TestRtmpProtocol::rejectsMalformedLengthPrefixedNalus() {
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(QByteArray::fromHex("00000000"), 4),
             QByteArray());
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(QByteArray::fromHex("00000004aabb"), 4),
             QByteArray());
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(QByteArray::fromHex("0001aa"), 0),
             QByteArray());
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(QByteArray::fromHex("0001aa00"), 2),
             QByteArray());
    QCOMPARE(RtmpFlv::lengthPrefixedPayloadToAnnexB(QByteArray::fromHex("ffffffff"), 4),
             QByteArray());
}

void TestRtmpProtocol::parsesAacSequenceHeaderAndBuildsAdtsFrame() {
    RtmpAacConfig config;
    QString error;
    QVERIFY(RtmpFlv::parseAacSequenceHeader(QByteArray::fromHex("1190"), &config, &error));
    QCOMPARE(config.audioObjectType, 2);
    QCOMPARE(config.sampleRate, 48000);
    QCOMPARE(config.channelCount, 2);

    const QByteArray header = RtmpFlv::adtsHeader(config, 4);
    QCOMPARE(header.size(), 7);
    QCOMPARE(header.toHex(), QByteArray("fff14c80017ffc"));
}

QTEST_GUILESS_MAIN(TestRtmpProtocol)
#include "tst_rtmpprotocol.moc"

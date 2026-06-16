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
} // namespace

class TestRtmpProtocol : public QObject {
    Q_OBJECT
private slots:
    void amf0CommandRoundTripsNameAndTransaction();
    void amf0WritesStrictArrayForFourCcList();
    void rtmpUrlPartsPreserveSignedQueryInPlayPath();
    void rtmpUrlPartsPreserveEncodedPathAndQuery();
    void rtmpUrlPartsUseAppAsPlayPathWhenPathHasNoStream();
    void connectPayloadAdvertisesEnhancedCodecCapabilities();
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
    void parsesAvcSequenceHeaderAndConvertsNalusToAnnexB();
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

void TestRtmpProtocol::rtmpUrlPartsUseAppAsPlayPathWhenPathHasNoStream() {
    const RtmpUrlParts parts =
        RtmpUrlParts::fromUrl(QUrl(QStringLiteral("rtmp://host/live?token=abc")));
    QCOMPARE(parts.app, QStringLiteral("live"));
    QCOMPARE(parts.playPath, QStringLiteral("live?token=abc"));
    QCOMPARE(parts.tcUrl, QStringLiteral("rtmp://host/live"));
}

void TestRtmpProtocol::connectPayloadAdvertisesEnhancedCodecCapabilities() {
    const QByteArray payload =
        RtmpAmf0::connectCommandPayload(QUrl(QStringLiteral("rtmp://127.0.0.1/live/stream")));
    QVERIFY(payload.contains("fourCcList"));
    QVERIFY(payload.contains("avc1"));
    QVERIFY(payload.contains("hvc1"));
    QVERIFY(payload.contains("mp4a"));
    QVERIFY(payload.contains("videoFourCcInfoMap"));
    QVERIFY(payload.contains("audioFourCcInfoMap"));
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

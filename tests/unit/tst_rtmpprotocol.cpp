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

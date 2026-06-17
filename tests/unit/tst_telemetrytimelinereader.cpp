#include <QtTest>
#include <QTemporaryDir>

#include "playback/telemetrytimelinereader.h"
#include "recorder_engine/muxer.h"

#include <cstring>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

class TestTelemetryTimelineReader : public QObject {
    Q_OBJECT

private slots:
    void readsLatestFeedTelemetryByPlayhead();
    void keysStateByStreamMetadataWhenPayloadFeedDiffers();
    void includesExactPlayheadAndUsesLaterPacketForEqualPts();

private:
    static bool writeSingleFeedTelemetryFile(const QString &filePath,
                                             const QString &feedId,
                                             const QList<QPair<qint64, QByteArray>> &packets);
};

bool TestTelemetryTimelineReader::writeSingleFeedTelemetryFile(
    const QString &filePath,
    const QString &feedId,
    const QList<QPair<qint64, QByteArray>> &packets) {
    AVFormatContext *formatContext = nullptr;
    const QByteArray encodedPath = filePath.toUtf8();
    if (avformat_alloc_output_context2(&formatContext, nullptr, "matroska", encodedPath.constData()) < 0 ||
        !formatContext) {
        return false;
    }

    AVStream *stream = avformat_new_stream(formatContext, nullptr);
    if (!stream) {
        avformat_free_context(formatContext);
        return false;
    }

    stream->id = 0;
    stream->codecpar->codec_id = AV_CODEC_ID_TEXT;
    stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    stream->time_base = AVRational{1, 1000};
    av_dict_set(&stream->metadata, "olr_track_type", "feed_telemetry", 0);
    av_dict_set(&stream->metadata, "olr_feed_id", feedId.toUtf8().constData(), 0);

    if (!(formatContext->oformat->flags & AVFMT_NOFILE) &&
        avio_open(&formatContext->pb, encodedPath.constData(), AVIO_FLAG_WRITE) < 0) {
        avformat_free_context(formatContext);
        return false;
    }

    if (avformat_write_header(formatContext, nullptr) < 0) {
        if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatContext->pb);
        }
        avformat_free_context(formatContext);
        return false;
    }

    for (const auto &packetData : packets) {
        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            av_write_trailer(formatContext);
            if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&formatContext->pb);
            }
            avformat_free_context(formatContext);
            return false;
        }

        if (av_new_packet(packet, packetData.second.size()) < 0) {
            av_packet_free(&packet);
            av_write_trailer(formatContext);
            if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&formatContext->pb);
            }
            avformat_free_context(formatContext);
            return false;
        }

        memcpy(packet->data, packetData.second.constData(), packetData.second.size());
        packet->stream_index = stream->index;
        packet->pts = av_rescale_q(packetData.first, AVRational{1, 1000}, stream->time_base);
        packet->dts = packet->pts;
        packet->duration = av_rescale_q(1, AVRational{1, 1000}, stream->time_base);

        const int ret = av_write_frame(formatContext, packet);
        av_packet_free(&packet);
        if (ret < 0) {
            av_write_trailer(formatContext);
            if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&formatContext->pb);
            }
            avformat_free_context(formatContext);
            return false;
        }
    }

    av_write_trailer(formatContext);
    if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&formatContext->pb);
    }
    avformat_free_context(formatContext);
    return true;
}

void TestTelemetryTimelineReader::readsLatestFeedTelemetryByPlayhead() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(dir.path());
    const QStringList viewNames{QStringLiteral("View 1")};
    const QStringList feedIds{QStringLiteral("cam-main"), QStringLiteral("cam-reverse")};
    const QStringList feedNames{QStringLiteral("Main"), QStringLiteral("Reverse")};
    QVERIFY(muxer.init(QStringLiteral("telemetry_reader"), 1, 320, 240, FrameRate{30, 1}, viewNames,
                       feedIds, feedNames, 48000, 2));

    muxer.writeTelemetryPacket(0, 100, QByteArrayLiteral(
        "{\"feedId\":\"cam-main\",\"values\":{\"batteryPercent\":90}}"));
    muxer.writeTelemetryPacket(0, 150, QByteArrayLiteral("{malformed"));
    muxer.writeTelemetryPacket(1, 200, QByteArrayLiteral(
        "{\"feedId\":\"cam-reverse\",\"values\":{\"signalDb\":-61}}"));
    muxer.writeTelemetryPacket(0, 300, QByteArrayLiteral(
        "{\"feedId\":\"cam-main\",\"values\":{\"batteryPercent\":88}}"));
    muxer.close();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(dir.filePath(QStringLiteral("telemetry_reader.mkv"))),
             qPrintable(reader.lastError()));
    QCOMPARE(reader.feedIds(), feedIds);
    QVERIFY(reader.lastError().isEmpty());

    const QVariantMap at50 = reader.stateAt(50);
    QVERIFY(at50.isEmpty());

    const QVariantMap at150 = reader.stateAt(150);
    QCOMPARE(at150.size(), 1);
    QCOMPARE(at150.value(QStringLiteral("cam-main"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("batteryPercent"))
                 .toInt(),
             90);
    QVERIFY(!at150.contains(QStringLiteral("cam-reverse")));

    const QVariantMap at250 = reader.stateAt(250);
    QCOMPARE(at250.size(), 2);
    QCOMPARE(at250.value(QStringLiteral("cam-main"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("batteryPercent"))
                 .toInt(),
             90);
    QCOMPARE(at250.value(QStringLiteral("cam-reverse"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("signalDb"))
                 .toInt(),
             -61);

    const QVariantMap at350 = reader.stateAt(350);
    QCOMPARE(at350.value(QStringLiteral("cam-main"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("batteryPercent"))
                 .toInt(),
             88);
    QCOMPARE(at350.value(QStringLiteral("cam-reverse"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("signalDb"))
                 .toInt(),
             -61);
}

void TestTelemetryTimelineReader::keysStateByStreamMetadataWhenPayloadFeedDiffers() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(dir.path());
    const QStringList viewNames{QStringLiteral("View 1")};
    const QStringList feedIds{QStringLiteral("cam-main")};
    const QStringList feedNames{QStringLiteral("Main")};
    QVERIFY(muxer.init(QStringLiteral("telemetry_metadata_key"), 1, 320, 240, FrameRate{30, 1},
                       viewNames, feedIds, feedNames, 48000, 2));

    muxer.writeTelemetryPacket(0, 100, QByteArrayLiteral(
        "{\"feedId\":\"payload-spoof\",\"values\":{\"batteryPercent\":77}}"));
    muxer.close();

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(dir.filePath(QStringLiteral("telemetry_metadata_key.mkv"))),
             qPrintable(reader.lastError()));

    const QVariantMap state = reader.stateAt(100);
    QCOMPARE(state.size(), 1);
    QVERIFY(state.contains(QStringLiteral("cam-main")));
    QVERIFY(!state.contains(QStringLiteral("payload-spoof")));
    QCOMPARE(state.value(QStringLiteral("cam-main"))
                 .toMap()
                 .value(QStringLiteral("feedId"))
                 .toString(),
             QStringLiteral("payload-spoof"));
}

void TestTelemetryTimelineReader::includesExactPlayheadAndUsesLaterPacketForEqualPts() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString filePath = dir.filePath(QStringLiteral("telemetry_equal_pts.mkv"));
    QVERIFY(writeSingleFeedTelemetryFile(
        filePath,
        QStringLiteral("cam-main"),
        {
            {100, QByteArrayLiteral("{\"values\":{\"batteryPercent\":90}}")},
            {100, QByteArrayLiteral("{\"values\":{\"batteryPercent\":88}}")},
        }));

    TelemetryTimelineReader reader;
    QVERIFY2(reader.load(filePath), qPrintable(reader.lastError()));

    QVERIFY(reader.stateAt(99).isEmpty());

    const QVariantMap state = reader.stateAt(100);
    QCOMPARE(state.value(QStringLiteral("cam-main"))
                 .toMap()
                 .value(QStringLiteral("values"))
                 .toMap()
                 .value(QStringLiteral("batteryPercent"))
                 .toInt(),
             88);
}

QTEST_GUILESS_MAIN(TestTelemetryTimelineReader)
#include "tst_telemetrytimelinereader.moc"

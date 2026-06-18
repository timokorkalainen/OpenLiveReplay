// Unit tests for Muxer — the FFmpeg Matroska container writer. Verifies the
// track layout (video / audio / subtitle offsets), stream bounds checking,
// audio channel-layout handling, and that init() actually produces a file.
//
// Hermetic: Muxer::getVideoPath() normally writes to <Documents>/videos, which
// on macOS cannot be redirected via $HOME or QStandardPaths test mode. Each
// test instead points the muxer at a per-run QTemporaryDir via
// setOutputDirectory(), so nothing is written outside the temp dir and the whole
// tree is auto-removed when the test object is destroyed.
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QScopeGuard>

#include "recorder_engine/muxer.h"

class TestMuxer : public QObject {
    Q_OBJECT
private slots:
    void initBuildsTrackLayout();
    void getStreamIsBoundsChecked();
    void stereoAudioChannelLayout();
    void monoAudioChannelLayout();
    void initProducesAFile();
    void initBuildsTelemetryTrackLayoutAndMetadata();
    void initFailureResetsTelemetryTrackState();
    void writeTelemetryPacketAcceptsValidFeedAndIgnoresInvalidFeed();
    void initFailsForH264WithoutExtradata();
    void initWritesTimecodeTagWhenStartTimecodeGiven();
    void initWritesNoTimecodeTagWhenStartTimecodeEmpty();
    void initIgnoresMalformedStartTimecode();
    void headerDeferredUntilFirstPacketThenCarriesCandidate();
    void setStartTimecodeCandidateFirstWins();
    void noTimecodeTagWhenCandidateAbsentButPacketWritten();
    void emptyRecordingClosesToValidMkv();

private:
    QTemporaryDir m_home;
    QString videoPathFor(const QString& name) const;
};

QString TestMuxer::videoPathFor(const QString& name) const {
    // With an explicit output directory set, the muxer writes <dir>/<name>.mkv
    // (the "videos" subfolder is only appended for the default Documents path).
    return m_home.path() + "/" + name + ".mkv";
}

void TestMuxer::initBuildsTrackLayout() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A"), QStringLiteral("B")};
    QVERIFY(m.init(QStringLiteral("olr_unit_layout"), 2, 640, 480, 30, names, 48000, 2));
    // 2 video + 2 audio + 2 subtitle, in that order.
    QCOMPARE(m.audioTrackOffset(), 2);
    QCOMPARE(m.subtitleTrackOffset(), 4);
    // Default codec must remain MPEG-2 (no behavior change).
    QCOMPARE(m.getStream(0)->codecpar->codec_id, AV_CODEC_ID_MPEG2VIDEO);
    QCOMPARE(m.getStream(1)->codecpar->codec_id, AV_CODEC_ID_MPEG2VIDEO);
    m.close();
}

void TestMuxer::getStreamIsBoundsChecked() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_bounds"), 1, 320, 240, 30, names, 48000, 2));
    // 1 video + 1 audio + 1 subtitle = 3 streams (indices 0..2).
    QVERIFY(m.getStream(0) != nullptr);
    QVERIFY(m.getStream(2) != nullptr);
    QVERIFY(m.getStream(3) == nullptr);
    QVERIFY(m.getStream(-1) == nullptr);
    m.close();
}

void TestMuxer::stereoAudioChannelLayout() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_stereo"), 1, 320, 240, 30, names, 48000, 2));
    AVStream* audio = m.getStream(m.audioTrackOffset());
    QVERIFY(audio != nullptr);
    QCOMPARE(audio->codecpar->ch_layout.nb_channels, 2);
    m.close();
}

void TestMuxer::monoAudioChannelLayout() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_mono"), 1, 320, 240, 30, names, 48000, 1));
    AVStream* audio = m.getStream(m.audioTrackOffset());
    QVERIFY(audio != nullptr);
    QCOMPARE(audio->codecpar->ch_layout.nb_channels, 1);
    m.close();
}

void TestMuxer::initProducesAFile() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_file"), 1, 320, 240, 30, names, 48000, 2));
    m.close();
    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_file")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));
    // The path must be inside the temp dir — guards against a regression where
    // the muxer ignores the override and writes to the real Documents tree.
    QVERIFY2(fi.filePath().startsWith(m_home.path()), "output escaped the temp dir");
    QVERIFY(fi.size() > 0);
}

void TestMuxer::initBuildsTelemetryTrackLayoutAndMetadata() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("View A"), QStringLiteral("View B")};
    const QStringList feedIds{
        QStringLiteral("feed-alpha"),
        QStringLiteral("feed-beta"),
        QStringLiteral("feed-gamma"),
    };
    const QStringList feedNames{
        QStringLiteral("Alpha Feed"),
        QStringLiteral("Beta Feed"),
    };

    QVERIFY(m.init(QStringLiteral("olr_unit_telemetry_layout"),
                   2,
                   640,
                   480,
                   30,
                   names,
                   feedIds,
                   feedNames,
                   48000,
                   2));

    QCOMPARE(m.audioTrackOffset(), 2);
    QCOMPARE(m.subtitleTrackOffset(), 4);
    QCOMPARE(m.telemetryTrackOffset(), 6);

    // 2 video + 2 audio + 2 per-view metadata + 3 feed telemetry.
    QVERIFY(m.getStream(8) != nullptr);
    QVERIFY(m.getStream(9) == nullptr);

    for (int i = 0; i < feedIds.size(); ++i) {
        AVStream* telemetry = m.getStream(m.telemetryTrackOffset() + i);
        QVERIFY(telemetry != nullptr);
        QCOMPARE(telemetry->codecpar->codec_id, AV_CODEC_ID_TEXT);
        QCOMPARE(telemetry->codecpar->codec_type, AVMEDIA_TYPE_SUBTITLE);
        QCOMPARE(telemetry->time_base.num, 1);
        QCOMPARE(telemetry->time_base.den, 1000);

        AVDictionaryEntry* title = av_dict_get(telemetry->metadata, "title", nullptr, 0);
        QVERIFY(title != nullptr);
        QCOMPARE(QString::fromUtf8(title->value), QStringLiteral("Feed %1 Telemetry").arg(feedIds.at(i)));

        AVDictionaryEntry* trackType = av_dict_get(telemetry->metadata, "olr_track_type", nullptr, 0);
        QVERIFY(trackType != nullptr);
        QCOMPARE(QString::fromUtf8(trackType->value), QStringLiteral("feed_telemetry"));

        AVDictionaryEntry* feedId = av_dict_get(telemetry->metadata, "olr_feed_id", nullptr, 0);
        QVERIFY(feedId != nullptr);
        QCOMPARE(QString::fromUtf8(feedId->value), feedIds.at(i));

        AVDictionaryEntry* feedName = av_dict_get(telemetry->metadata, "olr_feed_name", nullptr, 0);
        QVERIFY(feedName != nullptr);
        const QString expectedName = i < feedNames.size() ? feedNames.at(i) : QString();
        QCOMPARE(QString::fromUtf8(feedName->value), expectedName);
    }

    m.close();
}

void TestMuxer::initFailureResetsTelemetryTrackState() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("View A")};
    const QStringList feedIds{QStringLiteral("feed-alpha")};
    const QStringList feedNames{QStringLiteral("Alpha Feed")};

    QVERIFY(!m.init(QStringLiteral("missing/olr_unit_telemetry_init_fail"),
                    1,
                    320,
                    240,
                    30,
                    names,
                    feedIds,
                    feedNames,
                    48000,
                    2));

    QCOMPARE(m.telemetryTrackOffset(), 0);
    QVERIFY(m.getStream(0) == nullptr);
}

void TestMuxer::writeTelemetryPacketAcceptsValidFeedAndIgnoresInvalidFeed() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("View A")};
    const QStringList feedIds{QStringLiteral("feed-alpha")};
    const QStringList feedNames{QStringLiteral("Alpha Feed")};
    const QByteArray validPayload = QByteArrayLiteral("{\"speed\":42}");

    QVERIFY(m.init(QStringLiteral("olr_unit_telemetry_write"),
                   1,
                   320,
                   240,
                   30,
                   names,
                   feedIds,
                   feedNames,
                   48000,
                   2));

    m.writeTelemetryPacket(0, 123, validPayload);
    m.writeTelemetryPacket(1, 124, QByteArrayLiteral("{\"ignored\":true}"));
    m.writeTelemetryPacket(-1, 125, QByteArrayLiteral("{\"ignored\":true}"));
    m.writeTelemetryPacket(0, 126, QByteArray());
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_telemetry_write")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));
    QVERIFY(fi.size() > 0);

    AVFormatContext* ctx = nullptr;
    const QByteArray filePath = fi.filePath().toUtf8();
    QVERIFY(avformat_open_input(&ctx, filePath.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] {
        avformat_close_input(&ctx);
    });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    int telemetryStreamIndex = -1;
    AVStream* telemetryStream = nullptr;
    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        AVStream* st = ctx->streams[i];
        AVDictionaryEntry* trackType = av_dict_get(st->metadata, "olr_track_type", nullptr, 0);
        AVDictionaryEntry* feedId = av_dict_get(st->metadata, "olr_feed_id", nullptr, 0);
        if (trackType && feedId &&
            QString::fromUtf8(trackType->value) == QStringLiteral("feed_telemetry") &&
            QString::fromUtf8(feedId->value) == QStringLiteral("feed-alpha")) {
            QVERIFY2(telemetryStreamIndex == -1, "expected exactly one telemetry stream for feed-alpha");
            telemetryStreamIndex = static_cast<int>(i);
            telemetryStream = st;
        }
    }
    QVERIFY(telemetryStream != nullptr);

    AVPacket* pkt = av_packet_alloc();
    QVERIFY(pkt != nullptr);
    const auto freePacket = qScopeGuard([&pkt] {
        av_packet_free(&pkt);
    });

    int totalPackets = 0;
    int telemetryPackets = 0;
    int ret = 0;
    while ((ret = av_read_frame(ctx, pkt)) >= 0) {
        ++totalPackets;
        if (pkt->stream_index == telemetryStreamIndex) {
            ++telemetryPackets;
            QCOMPARE(pkt->stream_index, telemetryStreamIndex);
            QCOMPARE(pkt->pts, av_rescale_q(123, AVRational{1, 1000}, telemetryStream->time_base));
            QCOMPARE(av_rescale_q(pkt->pts, telemetryStream->time_base, AVRational{1, 1000}), int64_t(123));
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(pkt->data), pkt->size), validPayload);
        }
        av_packet_unref(pkt);
    }
    QCOMPARE(ret, AVERROR_EOF);
    QCOMPARE(totalPackets, 1);
    QCOMPARE(telemetryPackets, 1);
}

void TestMuxer::initFailsForH264WithoutExtradata() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    // H.264 requires avcC extradata; empty must be rejected, not silently accepted.
    QVERIFY(!m.init(QStringLiteral("olr_unit_h264_noextradata"), 1, 320, 240, 30, names,
                    48000, 2, VideoCodecChoice::H264Hardware, QByteArray()));
}

void TestMuxer::initWritesTimecodeTagWhenStartTimecodeGiven() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A"), QStringLiteral("B")};
    const QString startTc = QStringLiteral("01:00:00:05");

    // Two video tracks; pass a valid HH:MM:SS:FF start timecode.
    QVERIFY(
        m.init(QStringLiteral("olr_unit_tc_present"), 2, 320, 240, 30, names, 48000, 2, startTc));
    // Write at least one packet so the MKV is well-formed when reopened (an empty
    // cluster-less file the demuxer cannot re-parse otherwise — not a TC concern).
    m.writeMetadataPacket(0, 0, QByteArrayLiteral("{}"));
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_tc_present")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));
    QVERIFY(fi.size() > 0);

    AVFormatContext* ctx = nullptr;
    const QByteArray filePath = fi.filePath().toUtf8();
    QVERIFY(avformat_open_input(&ctx, filePath.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    // The Matroska muxer materialises the format-level "timecode" tag.
    AVDictionaryEntry* fmtTc = av_dict_get(ctx->metadata, "timecode", nullptr, 0);
    QVERIFY2(fmtTc != nullptr, "expected a format-level timecode tag");
    QCOMPARE(QString::fromUtf8(fmtTc->value), startTc);

    // Each video track also carries the timecode tag.
    int videoTracksWithTc = 0;
    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        AVStream* st = ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        AVDictionaryEntry* tc = av_dict_get(st->metadata, "timecode", nullptr, 0);
        QVERIFY2(tc != nullptr, "expected a per-video-track timecode tag");
        QCOMPARE(QString::fromUtf8(tc->value), startTc);
        ++videoTracksWithTc;
    }
    QCOMPARE(videoTracksWithTc, 2);
}

void TestMuxer::initWritesNoTimecodeTagWhenStartTimecodeEmpty() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};

    // Empty start timecode (the default) must produce NO timecode tag anywhere —
    // a no-TC recording is otherwise unchanged.
    QVERIFY(
        m.init(QStringLiteral("olr_unit_tc_absent"), 1, 320, 240, 30, names, 48000, 2, QString()));
    m.writeMetadataPacket(0, 0, QByteArrayLiteral("{}"));
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_tc_absent")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));

    AVFormatContext* ctx = nullptr;
    const QByteArray filePath = fi.filePath().toUtf8();
    QVERIFY(avformat_open_input(&ctx, filePath.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    QVERIFY2(av_dict_get(ctx->metadata, "timecode", nullptr, 0) == nullptr,
             "no format-level timecode tag expected for a no-TC recording");
    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        AVStream* st = ctx->streams[i];
        QVERIFY2(av_dict_get(st->metadata, "timecode", nullptr, 0) == nullptr,
                 "no per-track timecode tag expected for a no-TC recording");
    }
}

void TestMuxer::initIgnoresMalformedStartTimecode() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};

    // A malformed start timecode must be treated as "no TC" — no tag, no regression.
    QVERIFY(m.init(QStringLiteral("olr_unit_tc_malformed"), 1, 320, 240, 30, names, 48000, 2,
                   QStringLiteral("not-a-timecode")));
    m.writeMetadataPacket(0, 0, QByteArrayLiteral("{}"));
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_tc_malformed")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));

    AVFormatContext* ctx = nullptr;
    const QByteArray filePath = fi.filePath().toUtf8();
    QVERIFY(avformat_open_input(&ctx, filePath.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    QVERIFY2(av_dict_get(ctx->metadata, "timecode", nullptr, 0) == nullptr,
             "malformed start timecode must not write a format-level tag");
    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        AVStream* st = ctx->streams[i];
        QVERIFY2(av_dict_get(st->metadata, "timecode", nullptr, 0) == nullptr,
                 "malformed start timecode must not write a per-track tag");
    }
}

void TestMuxer::headerDeferredUntilFirstPacketThenCarriesCandidate() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A"), QStringLiteral("B")};
    const QString startTc = QStringLiteral("01:00:00:05");

    // init() with an up-front candidate, but DEFERS the header write. Before any
    // packet, the file on disk must NOT yet be a parseable MKV (no header).
    QVERIFY(
        m.init(QStringLiteral("olr_unit_tc_deferred"), 2, 320, 240, 30, names, 48000, 2, startTc));

    const QFileInfo fiPre(videoPathFor(QStringLiteral("olr_unit_tc_deferred")));
    {
        // No header yet → the demuxer cannot open/parse it as Matroska. (The file
        // may exist as an empty/zero-length avio target, but it is not a valid MKV.)
        AVFormatContext* preCtx = nullptr;
        const QByteArray prePath = fiPre.filePath().toUtf8();
        const int openRet = avformat_open_input(&preCtx, prePath.constData(), nullptr, nullptr);
        QVERIFY2(openRet < 0, "header must NOT be written before the first packet");
        if (preCtx) avformat_close_input(&preCtx);
    }

    // First packet → header is written NOW, materialising the stored candidate.
    m.writeMetadataPacket(0, 0, QByteArrayLiteral("{}"));
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_tc_deferred")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));
    QVERIFY(fi.size() > 0);

    AVFormatContext* ctx = nullptr;
    const QByteArray filePath = fi.filePath().toUtf8();
    QVERIFY(avformat_open_input(&ctx, filePath.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    AVDictionaryEntry* fmtTc = av_dict_get(ctx->metadata, "timecode", nullptr, 0);
    QVERIFY2(fmtTc != nullptr, "expected a format-level timecode tag after the first packet");
    QCOMPARE(QString::fromUtf8(fmtTc->value), startTc);

    int videoTracksWithTc = 0;
    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        AVStream* st = ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        AVDictionaryEntry* tc = av_dict_get(st->metadata, "timecode", nullptr, 0);
        QVERIFY2(tc != nullptr, "expected a per-video-track timecode tag");
        QCOMPARE(QString::fromUtf8(tc->value), startTc);
        ++videoTracksWithTc;
    }
    QCOMPARE(videoTracksWithTc, 2);
}

void TestMuxer::setStartTimecodeCandidateFirstWins() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    const QString firstTc = QStringLiteral("02:03:04:05");
    const QString secondTc = QStringLiteral("09:09:09:09");

    // No up-front candidate; the worker supplies it via setStartTimecodeCandidate.
    QVERIFY(m.init(QStringLiteral("olr_unit_tc_firstwins"), 1, 320, 240, 30, names, 48000, 2,
                   QString()));

    // First valid candidate wins; a second, different candidate is ignored.
    m.setStartTimecodeCandidate(firstTc);
    m.setStartTimecodeCandidate(secondTc);

    m.writeMetadataPacket(0, 0, QByteArrayLiteral("{}"));
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_tc_firstwins")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));

    AVFormatContext* ctx = nullptr;
    const QByteArray filePath = fi.filePath().toUtf8();
    QVERIFY(avformat_open_input(&ctx, filePath.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    AVDictionaryEntry* fmtTc = av_dict_get(ctx->metadata, "timecode", nullptr, 0);
    QVERIFY2(fmtTc != nullptr, "expected a format-level timecode tag from the first candidate");
    QCOMPARE(QString::fromUtf8(fmtTc->value), firstTc);
}

void TestMuxer::noTimecodeTagWhenCandidateAbsentButPacketWritten() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};

    // No candidate up-front, none supplied later → after a packet the header must
    // still write but carry NO timecode tag (no regression for a no-TC recording).
    QVERIFY(
        m.init(QStringLiteral("olr_unit_tc_none"), 1, 320, 240, 30, names, 48000, 2, QString()));
    m.writeMetadataPacket(0, 0, QByteArrayLiteral("{}"));
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_tc_none")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));
    QVERIFY(fi.size() > 0);

    AVFormatContext* ctx = nullptr;
    const QByteArray filePath = fi.filePath().toUtf8();
    QVERIFY(avformat_open_input(&ctx, filePath.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    QVERIFY2(av_dict_get(ctx->metadata, "timecode", nullptr, 0) == nullptr,
             "no format-level timecode tag expected when no candidate was supplied");
    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        AVStream* st = ctx->streams[i];
        QVERIFY2(av_dict_get(st->metadata, "timecode", nullptr, 0) == nullptr,
                 "no per-track timecode tag expected when no candidate was supplied");
    }
}

void TestMuxer::emptyRecordingClosesToValidMkv() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};

    // init() then close() with NO packets ever written. Because the header is now
    // deferred, close() must still flush a header + trailer so the file is exactly
    // what a zero-packet recording always was: a non-empty MKV header on disk (not
    // a regressed 0-byte file). NOTE: FFmpeg's matroska muxer produces a clusterless
    // header+trailer that the demuxer cannot re-parse even on the pre-existing
    // immediate-header path — so the contract is "file exists and is non-empty",
    // which is what initProducesAFile() has always asserted, NOT reopenability.
    QVERIFY(m.init(QStringLiteral("olr_unit_empty"), 1, 320, 240, 30, names, 48000, 2, QString()));
    m.close();

    const QFileInfo fi(videoPathFor(QStringLiteral("olr_unit_empty")));
    QVERIFY2(fi.exists(), qPrintable("expected output at " + fi.filePath()));
    // A header WAS written at close (deferred path): the file is non-empty, exactly
    // as the immediate-header path produced before this change.
    QVERIFY2(fi.size() > 0, "empty recording must still write a header (non-zero file)");

    // And it begins with the EBML magic (0x1A45DFA3): the header really is present.
    QFile f(fi.filePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray head = f.read(4);
    f.close();
    QVERIFY2(head.size() == 4 && static_cast<unsigned char>(head[0]) == 0x1A &&
                 static_cast<unsigned char>(head[1]) == 0x45 &&
                 static_cast<unsigned char>(head[2]) == 0xDF &&
                 static_cast<unsigned char>(head[3]) == 0xA3,
             "empty recording must still carry the EBML/Matroska header magic");
}

QTEST_GUILESS_MAIN(TestMuxer)
#include "tst_muxer.moc"

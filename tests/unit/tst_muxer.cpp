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

#include <atomic>
#include <array>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

#include "recorder_engine/muxer.h"

namespace {
struct StackPacketCompletion {
    void operator()(bool) const {}
};

template <typename Callback, typename Callable, typename = void>
struct HasGenericAsyncBind : std::false_type {};

template <typename Callback, typename Callable>
struct HasGenericAsyncBind<
    Callback, Callable,
    std::void_t<decltype(Callback::template bind<Callable>(std::declval<Callable&>()))>>
    : std::true_type {};

static_assert(!HasGenericAsyncBind<Muxer::PacketWriteCallback, StackPacketCompletion>::value,
              "asynchronous mux callbacks must not bind stack-local callables");

struct PacketCompletionProbe {
    std::atomic<int> written{0};
    std::atomic<int> rejected{0};

    static void complete(void* context, uint64_t, bool wasWritten) {
        auto& probe = *static_cast<PacketCompletionProbe*>(context);
        (wasWritten ? probe.written : probe.rejected).fetch_add(1, std::memory_order_acq_rel);
    }

    Muxer::PacketWriteCallback callback(uint64_t id = 0) {
        return Muxer::PacketWriteCallback{this, id, &PacketCompletionProbe::complete};
    }
};

struct RecursiveBatchProbe {
    Muxer* muxer = nullptr;
    int streamIndex = -1;
    std::atomic<bool> firstAccepted{false};
    std::atomic<bool> secondAccepted{true};
    std::atomic<int> firstWritten{0};
    std::atomic<int> firstRejected{0};
    std::atomic<int> secondWritten{0};
    std::atomic<int> secondRejected{0};

    static AVPacket* makePacket(int streamIndex, int64_t pts) {
        AVPacket* packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, 1) < 0) {
            av_packet_free(&packet);
            return nullptr;
        }
        packet->data[0] = '{';
        packet->stream_index = streamIndex;
        packet->pts = packet->dts = pts;
        packet->duration = 1;
        return packet;
    }

    static void complete(void* context, uint64_t id, bool written) {
        auto& probe = *static_cast<RecursiveBatchProbe*>(context);
        if (id >= 1 && id <= Muxer::kMaxPacketBatch) {
            (written ? probe.firstWritten : probe.firstRejected)
                .fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        if (id > Muxer::kMaxPacketBatch) {
            (written ? probe.secondWritten : probe.secondRejected)
                .fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        if (!written) return;

        std::array<AVPacket*, Muxer::kMaxPacketBatch * 2> owned{};
        std::array<Muxer::PacketWriteRequest, Muxer::kMaxPacketBatch> first{};
        std::array<Muxer::PacketWriteRequest, Muxer::kMaxPacketBatch> second{};
        bool ready = true;
        for (size_t i = 0; i < Muxer::kMaxPacketBatch; ++i) {
            owned[i] = makePacket(probe.streamIndex, int64_t(i + 1));
            owned[Muxer::kMaxPacketBatch + i] =
                makePacket(probe.streamIndex, int64_t(Muxer::kMaxPacketBatch + i + 1));
            ready = ready && owned[i] && owned[Muxer::kMaxPacketBatch + i];
            first[i].packet = owned[i];
            first[i].onWritten =
                Muxer::PacketWriteCallback{&probe, i + 1, &RecursiveBatchProbe::complete};
            second[i].packet = owned[Muxer::kMaxPacketBatch + i];
            second[i].onWritten = Muxer::PacketWriteCallback{&probe, Muxer::kMaxPacketBatch + i + 1,
                                                             &RecursiveBatchProbe::complete};
        }
        if (ready) {
            probe.firstAccepted.store(probe.muxer->writePacketBatch(first.data(), first.size()),
                                      std::memory_order_release);
            probe.secondAccepted.store(probe.muxer->writePacketBatch(second.data(), second.size()),
                                       std::memory_order_release);
        }
        for (AVPacket*& packet : owned)
            av_packet_free(&packet);
    }
};

struct ReentrantSidecarProbe {
    Muxer* muxer = nullptr;
    std::atomic<int>* videoCompletions = nullptr;
    std::atomic<int>* sidecarCompletions = nullptr;
    std::atomic<int>* sidecarRejections = nullptr;

    static void sidecarWritten(void* context, uint64_t, bool written) {
        auto& probe = *static_cast<ReentrantSidecarProbe*>(context);
        (written ? *probe.sidecarCompletions : *probe.sidecarRejections)
            .fetch_add(1, std::memory_order_acq_rel);
    }

    static void videoWritten(void* context, uint64_t, bool written) {
        auto& probe = *static_cast<ReentrantSidecarProbe*>(context);
        if (written) probe.videoCompletions->fetch_add(1, std::memory_order_acq_rel);
        AVPacket* packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, 2) < 0) {
            av_packet_free(&packet);
            return;
        }
        packet->data[0] = '{';
        packet->data[1] = '}';
        packet->stream_index = probe.muxer->subtitleTrackOffset();
        packet->pts = packet->dts = 1;
        packet->duration = 1;
        probe.muxer->writePacket(
            packet, Muxer::PacketWriteCallback{&probe, 0, &ReentrantSidecarProbe::sidecarWritten});
        av_packet_free(&packet);
    }
};
} // namespace

class TestMuxer : public QObject {
    Q_OBJECT
private slots:
    void fatalWriteErrorFlagAndMessage();
    void recordWriteOutcomeThresholdAndLatch();
    void recordWriteOutcomeResetOnSuccess();
    void recordWriteOutcomeInitClears();
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
    void advertisesRationalFrameRate();
    void writePacketCompletionRunsAfterWriterSuccess();
    void writePacketCompletionReportsRejectedPacket();
    void writePacketBatchRejectsAllEntriesBeforeQueueCommit();
    void writePacketBatchRejectsAllWhenAnyCarrierIsStale();
    void writePacketBatchRejectsOversizeWithoutWaiting();
    void queueSequenceExhaustionRejectsBeforeCandidateCommit();
    void rejectedPacketCandidateIsIgnored();
    void concurrentCandidateWinnerFollowsQueueAcceptanceOrder();
    void laterAcceptedCandidateWinsWithinHeaderGrace();
    void candidateAcceptedAtGraceBoundarySeedsHeader();
    void staleAdmissionAfterBackpressureCannotSeedCandidate();
    void queuedCarrierResetRejectsPixelsAndCandidate();
    void stalePublishedCandidateReopensWindowAndPreservesCurrentPacket();
    void stalePacketGuardRejectsPacketIndependentlyOfPublishedCandidate();
    void minWrittenVideoPtsTracksCommittedVideoPackets();
    void beginShutdownDrainWakesBlockedProducer();
    void beginShutdownDrainAcceptsInFlightPacketWhenQueueHasRoom();
    void writerThreadDrainHeadroomRejectsWholeBatch();
    void closeAllowsCallbackSidecarPacketDuringDrain();
    void dtsStateUpdatesOnlyAfterSuccessfulCommit();

private:
    QTemporaryDir m_home;
    QString videoPathFor(const QString& name) const;
};

QString TestMuxer::videoPathFor(const QString& name) const {
    // With an explicit output directory set, the muxer writes <dir>/<name>.mkv
    // (the "videos" subfolder is only appended for the default Documents path).
    return m_home.path() + "/" + name + ".mkv";
}

// ── recordWriteOutcome threshold / latch / reset tests ─────────────────────
// These tests call the private helper directly via the TestMuxer friend seam.
// No real recording is started; the muxer is never init()'d.

void TestMuxer::recordWriteOutcomeThresholdAndLatch() {
    Muxer m;
    // Two failures — below the threshold of 3 — must NOT trip the flag.
    m.recordWriteOutcome(true, "x");
    m.recordWriteOutcome(true, "x");
    QVERIFY(!m.hasFatalWriteError());
    QCOMPARE(m.fatalWriteMessage(), QString());

    // Third consecutive failure reaches kFatalWriteThreshold: flag is set.
    m.recordWriteOutcome(true, "x");
    QVERIFY(m.hasFatalWriteError());
    QVERIFY(!m.fatalWriteMessage().isEmpty());
}

void TestMuxer::recordWriteOutcomeResetOnSuccess() {
    Muxer m;
    // Two failures — not yet fatal.
    m.recordWriteOutcome(true, "x");
    m.recordWriteOutcome(true, "x");
    QVERIFY(!m.hasFatalWriteError());

    // A success resets the consecutive counter.
    m.recordWriteOutcome(false, nullptr);

    // Two more failures — counter restarted from 0, still below threshold.
    m.recordWriteOutcome(true, "x");
    m.recordWriteOutcome(true, "x");
    QVERIFY(!m.hasFatalWriteError());
}

void TestMuxer::recordWriteOutcomeInitClears() {
    Muxer m;
    m.setOutputDirectory(m_home.path());

    // Drive the muxer into a fatal state via the helper.
    m.recordWriteOutcome(true, "x");
    m.recordWriteOutcome(true, "x");
    m.recordWriteOutcome(true, "x");
    QVERIFY(m.hasFatalWriteError());

    // A successful init() must reset the flag and counter.
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_init_clears"), 1, 320, 240, 30, names, 48000, 2));
    QVERIFY(!m.hasFatalWriteError());
    QCOMPARE(m.fatalWriteMessage(), QString());
    m.close();
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

void TestMuxer::advertisesRationalFrameRate() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    // 29.97 (rounded fps 30, rational 30000/1001): the stream must advertise the
    // rational rate, not the legacy {30, 1}.
    QVERIFY(m.init(QStringLiteral("olr_unit_rate_2997"), 1, 320, 240, 30, names, 48000, 2,
                   VideoCodecChoice::Mpeg2Software, QByteArray(), QString(), 30000, 1001));
    AVStream* v = m.getStream(0);
    QVERIFY(v != nullptr);
    QCOMPARE(v->avg_frame_rate.num, 30000);
    QCOMPARE(v->avg_frame_rate.den, 1001);
    QCOMPARE(v->r_frame_rate.num, 30000);
    QCOMPARE(v->r_frame_rate.den, 1001);
    m.close();

    // Default (no rational supplied) keeps the legacy integer {fps, 1}.
    Muxer m2;
    m2.setOutputDirectory(m_home.path());
    QVERIFY(m2.init(QStringLiteral("olr_unit_rate_int"), 1, 320, 240, 30, names, 48000, 2));
    AVStream* v2 = m2.getStream(0);
    QVERIFY(v2 != nullptr);
    QCOMPARE(v2->avg_frame_rate.num, 30);
    QCOMPARE(v2->avg_frame_rate.den, 1);
    m2.close();
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

void TestMuxer::writePacketCompletionRunsAfterWriterSuccess() {
    QVERIFY(m_home.isValid());
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_unit_packet_completion"), 1, 320, 240, 30, names, 48000, 2,
                   QStringLiteral("01:02:03:04")));

    AVPacket* pkt = av_packet_alloc();
    QVERIFY(pkt != nullptr);
    QVERIFY(av_new_packet(pkt, 2) == 0);
    pkt->data[0] = '{';
    pkt->data[1] = '}';
    pkt->stream_index = m.subtitleTrackOffset();
    pkt->pts = 0;
    pkt->dts = 0;
    pkt->duration = 1;

    PacketCompletionProbe completion;
    m.writePacket(pkt, completion.callback());
    av_packet_free(&pkt);

    QTRY_COMPARE_WITH_TIMEOUT(completion.written.load(std::memory_order_acquire), 1, 2000);
    QCOMPARE(completion.rejected.load(std::memory_order_acquire), 0);
    m.close();
}

void TestMuxer::writePacketCompletionReportsRejectedPacket() {
    Muxer m;

    AVPacket* pkt = av_packet_alloc();
    QVERIFY(pkt != nullptr);
    QVERIFY(av_new_packet(pkt, 2) == 0);
    pkt->stream_index = 0;
    pkt->pts = 0;
    pkt->dts = 0;
    pkt->duration = 1;

    PacketCompletionProbe completion;
    m.writePacket(pkt, completion.callback());
    av_packet_free(&pkt);

    QCOMPARE(completion.written.load(std::memory_order_acquire), 0);
    QCOMPARE(completion.rejected.load(std::memory_order_acquire), 1);
}

void TestMuxer::writePacketBatchRejectsAllEntriesBeforeQueueCommit() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_packet_batch_invalid"), 1, 320, 240, 30,
                   {QStringLiteral("A")}, 48000, 2, QStringLiteral("01:02:03:04")));

    AVPacket* packet = av_packet_alloc();
    QVERIFY(packet != nullptr);
    QVERIFY(av_new_packet(packet, 2) == 0);
    packet->stream_index = m.subtitleTrackOffset();
    PacketCompletionProbe completion;
    std::array<Muxer::PacketWriteRequest, 2> batch{};
    batch[0].packet = packet;
    batch[0].onWritten = completion.callback(1);
    batch[1].packet = nullptr;
    batch[1].onWritten = completion.callback(2);

    const uint64_t sequenceBefore = m.m_nextQueuedPacketSequence;
    QVERIFY(!m.writePacketBatch(batch.data(), batch.size()));
    QCOMPARE(m.m_nextQueuedPacketSequence, sequenceBefore);
    QCOMPARE(completion.written.load(std::memory_order_acquire), 0);
    QCOMPARE(completion.rejected.load(std::memory_order_acquire), 2);

    av_packet_free(&packet);
    m.close();
}

void TestMuxer::writePacketBatchRejectsAllWhenAnyCarrierIsStale() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_packet_batch_stale"), 1, 320, 240, 30,
                   {QStringLiteral("A")}, 48000, 2, QStringLiteral("01:02:03:04")));

    AVPacket* first = av_packet_alloc();
    AVPacket* second = av_packet_alloc();
    QVERIFY(first != nullptr);
    QVERIFY(second != nullptr);
    QVERIFY(av_new_packet(first, 2) == 0);
    QVERIFY(av_new_packet(second, 2) == 0);
    first->stream_index = second->stream_index = m.subtitleTrackOffset();
    std::atomic<uint64_t> currentEpoch{1};
    PacketCompletionProbe completion;
    std::array<Muxer::PacketWriteRequest, 2> batch{};
    batch[0] = {first, completion.callback(1), QString(),
                Muxer::PacketCarrierGuard{&currentEpoch, 1}};
    batch[1] = {second, completion.callback(2), QString(),
                Muxer::PacketCarrierGuard{&currentEpoch, 2}};

    const uint64_t sequenceBefore = m.m_nextQueuedPacketSequence;
    QVERIFY(!m.writePacketBatch(batch.data(), batch.size()));
    QCOMPARE(m.m_nextQueuedPacketSequence, sequenceBefore);
    QCOMPARE(completion.written.load(std::memory_order_acquire), 0);
    QCOMPARE(completion.rejected.load(std::memory_order_acquire), 2);

    av_packet_free(&first);
    av_packet_free(&second);
    m.close();
}

void TestMuxer::writePacketBatchRejectsOversizeWithoutWaiting() {
    Muxer m;
    PacketCompletionProbe completion;
    std::array<Muxer::PacketWriteRequest, Muxer::kMaxPacketBatch + 1> batch{};
    for (size_t i = 0; i < batch.size(); ++i)
        batch[i].onWritten = completion.callback(i + 1);

    QVERIFY(!m.writePacketBatch(batch.data(), batch.size()));
    QCOMPARE(completion.written.load(std::memory_order_acquire), 0);
    QCOMPARE(completion.rejected.load(std::memory_order_acquire), int(batch.size()));
}

void TestMuxer::queueSequenceExhaustionRejectsBeforeCandidateCommit() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_queue_sequence_exhaustion"), 1, 320, 240, 30,
                   {QStringLiteral("A")}, 48000, 2, QStringLiteral("01:02:03:04")));

    auto makePacket = [&m](int64_t pts) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 2) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->data[1] = '}';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        return pkt;
    };

    {
        std::lock_guard<std::mutex> queueLock(m.m_qMutex);
        m.m_nextQueuedPacketSequence = std::numeric_limits<uint64_t>::max();
    }
    AVPacket* last = makePacket(0);
    QVERIFY(last != nullptr);
    QVERIFY(m.writePacket(last));
    av_packet_free(&last);

    AVPacket* exhausted = makePacket(1);
    QVERIFY(exhausted != nullptr);
    QVERIFY(!m.writePacket(exhausted, {}, QStringLiteral("05:06:07:08")));
    av_packet_free(&exhausted);

    {
        std::lock_guard<std::mutex> queueLock(m.m_qMutex);
        QCOMPARE(m.m_nextQueuedPacketSequence, uint64_t(0));
        QVERIFY(m.m_acceptedStartTimecodeCandidate.isEmpty());
    }
    m.close();
}

void TestMuxer::rejectedPacketCandidateIsIgnored() {
    Muxer m;
    AVPacket* pkt = av_packet_alloc();
    QVERIFY(pkt != nullptr);
    QVERIFY(av_new_packet(pkt, 1) == 0);
    pkt->stream_index = 0;
    pkt->pts = pkt->dts = 0;

    QVERIFY(!m.writePacket(pkt, {}, QStringLiteral("01:02:03:04")));
    av_packet_free(&pkt);

    QMutexLocker headerLock(&m.m_headerMutex);
    QVERIFY(m.m_startTimecodeCandidate.isEmpty());
    {
        std::lock_guard<std::mutex> queueLock(m.m_qMutex);
        QVERIFY(m.m_acceptedStartTimecodeCandidate.isEmpty());
    }
}

void TestMuxer::concurrentCandidateWinnerFollowsQueueAcceptanceOrder() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_tc_concurrent_order"), 1, 320, 240, 30,
                   {QStringLiteral("A")}, 48000, 2, QString()));

    auto makePacket = [&m](int64_t pts) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 1) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        return pkt;
    };

    AVPacket* firstProducerPacket = makePacket(1);
    AVPacket* secondProducerPacket = makePacket(2);
    QVERIFY(firstProducerPacket != nullptr);
    QVERIFY(secondProducerPacket != nullptr);
    const QString candidateOne = QStringLiteral("01:00:00:01");
    const QString candidateTwo = QStringLiteral("02:00:00:02");

    // Keep the writer from consuming either packet while the producers race for
    // qMutex. The writer releases qMutex before taking this header lock, so this
    // also asserts the intended lock order does not deadlock producers.
    m.m_headerMutex.lock();
    bool headerMutexLocked = true;
    const auto unlockHeaderMutex = qScopeGuard([&] {
        if (headerMutexLocked) m.m_headerMutex.unlock();
    });
    std::unique_lock<std::mutex> queueGate(m.m_qMutex);
    std::atomic<bool> beforeCandidatePublication{false};
    m.m_beforeCandidatePublicationForTest = [&] {
        beforeCandidatePublication.store(true, std::memory_order_release);
    };
    std::atomic<int> ready{0};
    std::thread producerOne([&] {
        ready.fetch_add(1, std::memory_order_release);
        m.writePacket(firstProducerPacket, {}, candidateOne);
    });
    std::thread producerTwo([&] {
        ready.fetch_add(1, std::memory_order_release);
        m.writePacket(secondProducerPacket, {}, candidateTwo);
    });
    while (ready.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    queueGate.unlock();
    producerOne.join();
    producerTwo.join();

    QTRY_VERIFY_WITH_TIMEOUT(beforeCandidatePublication.load(std::memory_order_acquire), 2000);
    {
        std::lock_guard<std::mutex> queueLock(m.m_qMutex);
        QVERIFY(!m.m_pktQueue.empty());
        const int64_t acceptedFirstPts = m.m_pktQueue.front().pkt->pts;
        QCOMPARE(m.m_acceptedStartTimecodeCandidate,
                 acceptedFirstPts == 1 ? candidateOne : candidateTwo);
    }
    m.m_headerMutex.unlock();
    headerMutexLocked = false;

    av_packet_free(&firstProducerPacket);
    av_packet_free(&secondProducerPacket);
    m.close();
}

void TestMuxer::laterAcceptedCandidateWinsWithinHeaderGrace() {
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "5000");
    const auto restoreGrace = qScopeGuard([] { qunsetenv("OLR_MUXER_TMCD_GRACE_MS"); });

    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QString baseName = QStringLiteral("olr_unit_tc_later_candidate");
    QVERIFY(m.init(baseName, 1, 320, 240, 30, {QStringLiteral("A")}, 48000, 2, QString()));

    auto writeSubtitle = [&m](int64_t pts, const QString& candidate) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 2) < 0) {
            av_packet_free(&pkt);
            return false;
        }
        pkt->data[0] = '{';
        pkt->data[1] = '}';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        const bool accepted = m.writePacket(pkt, {}, candidate);
        av_packet_free(&pkt);
        return accepted;
    };

    QVERIFY(writeSubtitle(0, QString()));
    const QString candidate = QStringLiteral("03:04:05:06");
    QVERIFY(writeSubtitle(1, candidate));
    m.close();

    AVFormatContext* ctx = nullptr;
    const QByteArray path = videoPathFor(baseName).toUtf8();
    QVERIFY(avformat_open_input(&ctx, path.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);
    AVDictionaryEntry* tag = av_dict_get(ctx->metadata, "timecode", nullptr, 0);
    QVERIFY(tag != nullptr);
    QCOMPARE(QString::fromUtf8(tag->value), candidate);
}

void TestMuxer::candidateAcceptedAtGraceBoundarySeedsHeader() {
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "0");
    const auto restoreGrace = qScopeGuard([] { qunsetenv("OLR_MUXER_TMCD_GRACE_MS"); });

    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QString baseName = QStringLiteral("olr_unit_tc_grace_boundary");
    QVERIFY(m.init(baseName, 1, 320, 240, 30, {QStringLiteral("A")}, 48000, 2, QString()));

    auto makeSubtitlePacket = [&m](int64_t pts) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 2) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->data[1] = '}';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        return pkt;
    };

    const QString boundaryCandidate = QStringLiteral("04:05:06:07");
    m.m_afterCandidateSnapshotForTest = [&m, &makeSubtitlePacket, boundaryCandidate] {
        AVPacket* candidatePacket = makeSubtitlePacket(1);
        if (!candidatePacket) return;
        m.writePacket(candidatePacket, {}, boundaryCandidate);
        av_packet_free(&candidatePacket);
    };

    AVPacket* firstPacket = makeSubtitlePacket(0);
    QVERIFY(firstPacket != nullptr);
    QVERIFY(m.writePacket(firstPacket));
    av_packet_free(&firstPacket);
    m.close();

    AVFormatContext* ctx = nullptr;
    const QByteArray path = videoPathFor(baseName).toUtf8();
    QVERIFY(avformat_open_input(&ctx, path.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);
    AVDictionaryEntry* tag = av_dict_get(ctx->metadata, "timecode", nullptr, 0);
    QVERIFY(tag != nullptr);
    QCOMPARE(QString::fromUtf8(tag->value), boundaryCandidate);
}

void TestMuxer::staleAdmissionAfterBackpressureCannotSeedCandidate() {
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "60000");
    const auto restoreGrace = qScopeGuard([] { qunsetenv("OLR_MUXER_TMCD_GRACE_MS"); });
    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_tc_stale_backpressure"), 1, 320, 240, 30,
                   {QStringLiteral("A")}, 48000, 2, QString()));

    auto makePacket = [&m]() {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 1) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pkt->dts = 0;
        pkt->duration = 1;
        return pkt;
    };
    for (size_t i = 0; i < Muxer::kMaxQueuedPackets; ++i) {
        AVPacket* pkt = makePacket();
        QVERIFY(pkt != nullptr);
        QVERIFY(m.writePacket(pkt));
        av_packet_free(&pkt);
    }

    std::atomic<uint64_t> currentEpoch{7};
    std::atomic<bool> entered{false};
    bool accepted = true;
    AVPacket* stale = makePacket();
    QVERIFY(stale != nullptr);
    std::thread producer([&] {
        entered.store(true, std::memory_order_release);
        accepted = m.writePacket(stale, {}, QStringLiteral("05:06:07:08"),
                                 Muxer::PacketCarrierGuard{&currentEpoch, 7});
    });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    currentEpoch.store(8, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m.m_qMutex);
        QVERIFY(!m.m_pktQueue.empty());
        AVPacket* released = m.m_pktQueue.front().pkt;
        m.m_pktQueue.pop();
        av_packet_free(&released);
    }
    m.m_qCv.notify_all();
    producer.join();
    av_packet_free(&stale);

    QVERIFY(!accepted);
    {
        std::lock_guard<std::mutex> lock(m.m_qMutex);
        QVERIFY(m.m_acceptedStartTimecodeCandidate.isEmpty());
    }
    m.close();
}

void TestMuxer::queuedCarrierResetRejectsPixelsAndCandidate() {
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "5000");
    const auto restoreGrace = qScopeGuard([] { qunsetenv("OLR_MUXER_TMCD_GRACE_MS"); });

    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QString baseName = QStringLiteral("olr_unit_tc_queued_reset");
    QVERIFY(m.init(baseName, 1, 320, 240, 30, {QStringLiteral("A")}, 48000, 2, QString()));

    auto makePacket = [&m](int64_t pts) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 2) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->data[1] = '}';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        return pkt;
    };

    std::atomic<uint64_t> currentEpoch{7};
    PacketCompletionProbe staleCompletion;
    std::atomic<bool> beforeCandidatePublication{false};
    m.m_beforeCandidatePublicationForTest = [&beforeCandidatePublication] {
        beforeCandidatePublication.store(true, std::memory_order_release);
    };
    m.m_headerMutex.lock();
    bool headerMutexLocked = true;
    const auto unlockHeaderMutex = qScopeGuard([&] {
        if (headerMutexLocked) m.m_headerMutex.unlock();
    });
    AVPacket* stale = makePacket(0);
    QVERIFY(stale != nullptr);
    QVERIFY(m.writePacket(stale, staleCompletion.callback(), QStringLiteral("05:06:07:08"),
                          Muxer::PacketCarrierGuard{&currentEpoch, 7}));
    av_packet_free(&stale);
    QTRY_VERIFY_WITH_TIMEOUT(beforeCandidatePublication.load(std::memory_order_acquire), 2000);
    currentEpoch.store(8, std::memory_order_release);
    m.m_headerMutex.unlock();
    headerMutexLocked = false;
    QTRY_COMPARE_WITH_TIMEOUT(staleCompletion.rejected.load(std::memory_order_acquire), 1, 2000);
    QCOMPARE(staleCompletion.written.load(std::memory_order_acquire), 0);

    const QString currentCandidate = QStringLiteral("06:07:08:09");
    AVPacket* current = makePacket(1);
    QVERIFY(current != nullptr);
    QVERIFY(
        m.writePacket(current, {}, currentCandidate, Muxer::PacketCarrierGuard{&currentEpoch, 8}));
    av_packet_free(&current);
    m.close();

    AVFormatContext* ctx = nullptr;
    const QByteArray path = videoPathFor(baseName).toUtf8();
    QVERIFY(avformat_open_input(&ctx, path.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);
    AVDictionaryEntry* tag = av_dict_get(ctx->metadata, "timecode", nullptr, 0);
    QVERIFY(tag != nullptr);
    QCOMPARE(QString::fromUtf8(tag->value), currentCandidate);
}

void TestMuxer::stalePublishedCandidateReopensWindowAndPreservesCurrentPacket() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QString baseName = QStringLiteral("olr_unit_tc_final_candidate_reset");
    QVERIFY(m.init(baseName, 1, 320, 240, 30, {QStringLiteral("A")}, 48000, 2, QString()));

    auto makePacket = [&m](int64_t pts) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 2) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->data[1] = '}';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        return pkt;
    };

    std::atomic<uint64_t> packetEpoch{1};
    std::atomic<uint64_t> candidateEpoch{1};
    std::mutex hookMutex;
    std::condition_variable hookCv;
    bool guardsInstalled = false;
    bool releaseWriter = false;
    m.m_afterCandidateSnapshotForTest = [&] {
        {
            std::lock_guard<std::mutex> queueLock(m.m_qMutex);
            m.m_pktQueue.front().carrierGuard = Muxer::PacketCarrierGuard{&packetEpoch, 1};
            m.m_acceptedStartTimecodeCandidates.front().carrierGuard =
                Muxer::PacketCarrierGuard{&candidateEpoch, 1};
        }
        std::unique_lock<std::mutex> lock(hookMutex);
        guardsInstalled = true;
        hookCv.notify_all();
        hookCv.wait(lock, [&] { return releaseWriter; });
    };
    m.m_afterCandidatePublicationForTest = [&] {
        candidateEpoch.store(2, std::memory_order_release);
    };

    const QString staleCandidate = QStringLiteral("07:08:09:10");
    const QString currentCandidate = QStringLiteral("08:09:10:11");
    PacketCompletionProbe firstCompletion;
    PacketCompletionProbe secondCompletion;
    AVPacket* first = makePacket(1);
    QVERIFY(first != nullptr);
    QVERIFY(m.writePacket(first, firstCompletion.callback(), staleCandidate));
    av_packet_free(&first);
    {
        std::unique_lock<std::mutex> lock(hookMutex);
        QVERIFY(hookCv.wait_for(lock, std::chrono::seconds(2), [&] { return guardsInstalled; }));
    }
    AVPacket* second = makePacket(2);
    QVERIFY(second != nullptr);
    QVERIFY(m.writePacket(second, secondCompletion.callback(), currentCandidate));
    av_packet_free(&second);
    {
        std::lock_guard<std::mutex> lock(hookMutex);
        releaseWriter = true;
    }
    hookCv.notify_all();
    m.close();

    QCOMPARE(firstCompletion.written.load(std::memory_order_acquire), 1);
    QCOMPARE(firstCompletion.rejected.load(std::memory_order_acquire), 0);
    QCOMPARE(secondCompletion.written.load(std::memory_order_acquire), 1);
    AVFormatContext* ctx = nullptr;
    const QByteArray path = videoPathFor(baseName).toUtf8();
    QVERIFY(avformat_open_input(&ctx, path.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&ctx] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);
    const AVDictionaryEntry* tag = av_dict_get(ctx->metadata, "timecode", nullptr, 0);
    QVERIFY(tag != nullptr);
    QCOMPARE(QString::fromUtf8(tag->value), currentCandidate);
}

void TestMuxer::stalePacketGuardRejectsPacketIndependentlyOfPublishedCandidate() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QString baseName = QStringLiteral("olr_unit_tc_final_packet_reset");
    QVERIFY(m.init(baseName, 1, 320, 240, 30, {QStringLiteral("A")}, 48000, 2, QString()));

    auto makePacket = [&m](int64_t pts) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, 2) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->data[1] = '}';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pkt->dts = pts;
        pkt->duration = 1;
        return pkt;
    };

    std::atomic<uint64_t> packetEpoch{1};
    std::atomic<uint64_t> candidateEpoch{1};
    m.m_afterCandidateSnapshotForTest = [&] {
        std::lock_guard<std::mutex> queueLock(m.m_qMutex);
        m.m_pktQueue.front().carrierGuard = Muxer::PacketCarrierGuard{&packetEpoch, 1};
        m.m_acceptedStartTimecodeCandidates.front().carrierGuard =
            Muxer::PacketCarrierGuard{&candidateEpoch, 1};
    };
    m.m_afterCandidatePublicationForTest = [&] { packetEpoch.store(2, std::memory_order_release); };

    const QString candidate = QStringLiteral("09:10:11:12");
    PacketCompletionProbe staleCompletion;
    PacketCompletionProbe currentCompletion;
    AVPacket* stale = makePacket(1);
    QVERIFY(stale != nullptr);
    QVERIFY(m.writePacket(stale, staleCompletion.callback(), candidate));
    av_packet_free(&stale);
    AVPacket* current = makePacket(2);
    QVERIFY(current != nullptr);
    QVERIFY(m.writePacket(current, currentCompletion.callback()));
    av_packet_free(&current);
    m.close();

    QCOMPARE(staleCompletion.written.load(std::memory_order_acquire), 0);
    QCOMPARE(staleCompletion.rejected.load(std::memory_order_acquire), 1);
    QCOMPARE(currentCompletion.written.load(std::memory_order_acquire), 1);
}

void TestMuxer::minWrittenVideoPtsTracksCommittedVideoPackets() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A"), QStringLiteral("B")};
    QVERIFY(m.init(QStringLiteral("olr_unit_video_tail"), 2, 320, 240, 30, names, 48000, 2,
                   QStringLiteral("01:02:03:04")));

    auto makeVideoPacket = [](int streamIndex, qint64 ptsMs) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return pkt;
        if (av_new_packet(pkt, 2) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = char(0x00);
        pkt->data[1] = char(0x01);
        pkt->stream_index = streamIndex;
        pkt->pts = ptsMs;
        pkt->dts = ptsMs;
        pkt->duration = 40;
        pkt->flags |= AV_PKT_FLAG_KEY;
        return pkt;
    };

    PacketCompletionProbe completion;
    AVPacket* a = makeVideoPacket(0, 1200);
    QVERIFY(a != nullptr);
    QVERIFY(m.writePacket(a, completion.callback(1)));
    av_packet_free(&a);

    AVPacket* b = makeVideoPacket(1, 1000);
    QVERIFY(b != nullptr);
    QVERIFY(m.writePacket(b, completion.callback(2)));
    av_packet_free(&b);

    QTRY_COMPARE_WITH_TIMEOUT(completion.written.load(std::memory_order_acquire), 2, 2000);
    QCOMPARE(completion.rejected.load(std::memory_order_acquire), 0);
    QCOMPARE(m.minWrittenVideoPtsMs(), qint64(1000));
    m.close();
}

void TestMuxer::beginShutdownDrainWakesBlockedProducer() {
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "60000");
    auto restoreGrace = qScopeGuard([] { qunsetenv("OLR_MUXER_TMCD_GRACE_MS"); });

    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_stop_accepting"), 1, 320, 240, 30,
                   {QStringLiteral("A")}, 48000, 2, QString()));

    auto makePacket = [&m]() {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return pkt;
        if (av_new_packet(pkt, 1) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = 0;
        pkt->dts = 0;
        pkt->duration = 1;
        return pkt;
    };

    for (size_t i = 0; i < Muxer::kMaxQueuedPackets; ++i) {
        AVPacket* pkt = makePacket();
        QVERIFY(pkt != nullptr);
        m.writePacket(pkt);
        av_packet_free(&pkt);
    }

    std::atomic<bool> producerReturned{false};
    PacketCompletionProbe completion;
    std::thread blockedProducer([&] {
        AVPacket* first = makePacket();
        AVPacket* second = makePacket();
        if (!first || !second) {
            av_packet_free(&first);
            av_packet_free(&second);
            producerReturned.store(true, std::memory_order_release);
            return;
        }
        std::array<Muxer::PacketWriteRequest, 2> batch{};
        batch[0].packet = first;
        batch[0].onWritten = completion.callback(1);
        batch[1].packet = second;
        batch[1].onWritten = completion.callback(2);
        m.writePacketBatch(batch.data(), batch.size());
        av_packet_free(&first);
        av_packet_free(&second);
        producerReturned.store(true, std::memory_order_release);
    });

    QTest::qWait(50);
    QVERIFY(!producerReturned.load(std::memory_order_acquire));

    m.beginShutdownDrain();
    QTRY_VERIFY_WITH_TIMEOUT(producerReturned.load(std::memory_order_acquire), 2000);
    blockedProducer.join();
    QCOMPARE(completion.written.load(std::memory_order_acquire), 0);
    QCOMPARE(completion.rejected.load(std::memory_order_acquire), 2);

    m.close();
}

void TestMuxer::beginShutdownDrainAcceptsInFlightPacketWhenQueueHasRoom() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_shutdown_drain_accepts"), 1, 320, 240, 30,
                   {QStringLiteral("A")}, 48000, 2, QStringLiteral("01:02:03:04")));

    m.beginShutdownDrain();

    AVPacket* pkt = av_packet_alloc();
    QVERIFY(pkt != nullptr);
    QVERIFY(av_new_packet(pkt, 2) == 0);
    pkt->data[0] = '{';
    pkt->data[1] = '}';
    pkt->stream_index = m.subtitleTrackOffset();
    pkt->pts = 0;
    pkt->dts = 0;
    pkt->duration = 1;

    PacketCompletionProbe completion;
    m.writePacket(pkt, completion.callback());
    av_packet_free(&pkt);

    QTRY_COMPARE_WITH_TIMEOUT(completion.written.load(std::memory_order_acquire), 1, 2000);
    QCOMPARE(completion.rejected.load(std::memory_order_acquire), 0);
    m.close();
}

void TestMuxer::writerThreadDrainHeadroomRejectsWholeBatch() {
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "60000");
    auto restoreGrace = qScopeGuard([] { qunsetenv("OLR_MUXER_TMCD_GRACE_MS"); });

    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_recursive_batch_headroom"), 1, 320, 240, 30,
                   {QStringLiteral("A")}, 48000, 2, QString()));

    std::atomic<bool> writerHoldingFront{false};
    m.m_afterCandidateSnapshotForTest = [&writerHoldingFront] {
        writerHoldingFront.store(true, std::memory_order_release);
    };
    RecursiveBatchProbe probe{&m, m.subtitleTrackOffset()};
    AVPacket* trigger = RecursiveBatchProbe::makePacket(m.subtitleTrackOffset(), 0);
    QVERIFY(trigger != nullptr);
    QVERIFY(m.writePacket(trigger,
                          Muxer::PacketWriteCallback{&probe, 0, &RecursiveBatchProbe::complete}));
    av_packet_free(&trigger);
    QTRY_VERIFY_WITH_TIMEOUT(writerHoldingFront.load(std::memory_order_acquire), 2000);

    for (size_t i = 1; i < Muxer::kMaxQueuedPackets; ++i) {
        AVPacket* packet = RecursiveBatchProbe::makePacket(m.subtitleTrackOffset(), int64_t(i));
        QVERIFY(packet != nullptr);
        QVERIFY(m.writePacket(packet));
        av_packet_free(&packet);
    }

    m.close();

    QVERIFY(probe.firstAccepted.load(std::memory_order_acquire));
    QVERIFY(!probe.secondAccepted.load(std::memory_order_acquire));
    QCOMPARE(probe.firstWritten.load(std::memory_order_acquire), int(Muxer::kMaxPacketBatch));
    QCOMPARE(probe.firstRejected.load(std::memory_order_acquire), 0);
    QCOMPARE(probe.secondWritten.load(std::memory_order_acquire), 0);
    QCOMPARE(probe.secondRejected.load(std::memory_order_acquire), int(Muxer::kMaxPacketBatch));
    QVERIFY(m.m_pktQueue.empty());
}

void TestMuxer::closeAllowsCallbackSidecarPacketDuringDrain() {
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "60000");
    auto restoreGrace = qScopeGuard([] { qunsetenv("OLR_MUXER_TMCD_GRACE_MS"); });

    Muxer m;
    m.setOutputDirectory(m_home.path());
    QVERIFY(m.init(QStringLiteral("olr_unit_close_sidecar"), 1, 320, 240, 30, {QStringLiteral("A")},
                   48000, 2, QString()));

    auto makePacket = [&m](int64_t pts) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return pkt;
        if (av_new_packet(pkt, 2) < 0) {
            av_packet_free(&pkt);
            return pkt;
        }
        pkt->data[0] = '{';
        pkt->data[1] = '}';
        pkt->stream_index = m.subtitleTrackOffset();
        pkt->pts = pts;
        pkt->dts = pts;
        pkt->duration = 1;
        return pkt;
    };

    std::atomic<int> videoCompletions{0};
    std::atomic<int> sidecarCompletions{0};
    std::atomic<int> sidecarRejections{0};
    AVPacket* pkt = makePacket(0);
    QVERIFY(pkt != nullptr);
    ReentrantSidecarProbe probe{&m, &videoCompletions, &sidecarCompletions, &sidecarRejections};
    m.writePacket(pkt, Muxer::PacketWriteCallback{&probe, 0, &ReentrantSidecarProbe::videoWritten});
    av_packet_free(&pkt);

    m.close();

    QCOMPARE(videoCompletions.load(std::memory_order_acquire), 1);
    QCOMPARE(sidecarCompletions.load(std::memory_order_acquire), 1);
    QCOMPARE(sidecarRejections.load(std::memory_order_acquire), 0);
}

void TestMuxer::dtsStateUpdatesOnlyAfterSuccessfulCommit() {
    Muxer m;
    m.m_lastDts.insert(0, 10);

    AVPacket* pkt = av_packet_alloc();
    QVERIFY(pkt != nullptr);
    pkt->stream_index = 0;
    pkt->pts = 5;
    pkt->dts = 5;

    m.normalizePacketDts(pkt);

    QCOMPARE(pkt->dts, int64_t(11));
    QCOMPARE(pkt->pts, int64_t(11));
    QCOMPARE(m.m_lastDts.value(0), int64_t(10));

    m.rememberWrittenPacketDts(pkt);

    QCOMPARE(m.m_lastDts.value(0), int64_t(11));
    av_packet_free(&pkt);
}

void TestMuxer::fatalWriteErrorFlagAndMessage() {
    Muxer m;
    QVERIFY(!m.hasFatalWriteError());
    QCOMPARE(m.fatalWriteMessage(), QString());

    {
        std::lock_guard<std::mutex> lk(m.m_fatalMsgMutex);
        m.m_fatalWriteMsg = "No space left on device";
    }
    m.m_fatalWriteError.store(true, std::memory_order_release);

    QVERIFY(m.hasFatalWriteError());
    QCOMPARE(m.fatalWriteMessage(), QStringLiteral("No space left on device"));
}

QTEST_GUILESS_MAIN(TestMuxer)
#include "tst_muxer.moc"

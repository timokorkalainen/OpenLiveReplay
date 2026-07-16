#include <QtTest>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>

#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/replaymanager.h"
#include "recorder_engine/ingest/ingestsession.h"
#include "recorder_engine/timing/smpte12m.h"
#include "recorder_engine/timing/sourceoffsetestimator.h"
#include "recorder_engine/timing/timingreference.h"

#include <atomic>
#include <array>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

namespace allocation_probe {
std::atomic<bool> enabled{false};
std::atomic<uint64_t> count{0};
} // namespace allocation_probe

void* operator new(std::size_t size) {
    if (allocation_probe::enabled.load(std::memory_order_relaxed))
        allocation_probe::count.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

class OneFrameDelayedEncoder final : public NativeVideoEncoder {
public:
    bool encode(const AVFrame*, int64_t ptsTicks, const PacketCallback& onPacket,
                QString*) override {
        if (m_pendingPts) {
            onPacket(QByteArray::fromHex("000001b300100113"), *m_pendingPts, true);
        }
        m_pendingPts = ptsTicks;
        return true;
    }

    bool encodeSurface(GpuSurface*, int64_t, const ColorMetadata&, const PacketCallback&,
                       QString*) override {
        return false;
    }

    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }

private:
    std::optional<int64_t> m_pendingPts;
};

class CountingNativeEncoder final : public NativeVideoEncoder {
public:
    bool encode(const AVFrame*, int64_t, const PacketCallback&, QString*) override {
        ++calls;
        return true;
    }
    bool encodeSurface(GpuSurface*, int64_t, const ColorMetadata&, const PacketCallback&,
                       QString*) override {
        return false;
    }
    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }

    int calls = 0;
};

class PacketThenFailNativeEncoder final : public NativeVideoEncoder {
public:
    bool encode(const AVFrame*, int64_t ptsTicks, const PacketCallback& onPacket,
                QString*) override {
        onPacket(QByteArray::fromHex("000001b300100113"), ptsTicks, true);
        return false;
    }
    bool encodeSurface(GpuSurface*, int64_t, const ColorMetadata&, const PacketCallback&,
                       QString*) override {
        return false;
    }
    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }
};

class TwoPacketNativeEncoder final : public NativeVideoEncoder {
public:
    bool encode(const AVFrame*, int64_t ptsTicks, const PacketCallback& onPacket,
                QString*) override {
        onPacket(QByteArray::fromHex("000001b300100113"), ptsTicks, true);
        onPacket(QByteArray::fromHex("000001b300100114"), ptsTicks, false);
        return true;
    }
    bool encodeSurface(GpuSurface*, int64_t, const ColorMetadata&, const PacketCallback&,
                       QString*) override {
        return false;
    }
    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArrayLiteral("avcc"); }
};

AVCodecContext* makeDelayedMpeg2Encoder() {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec) return nullptr;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) return nullptr;
    context->width = 64;
    context->height = 64;
    context->time_base = AVRational{1, 30};
    context->framerate = AVRational{30, 1};
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->gop_size = 12;
    context->max_b_frames = 1;
    context->bit_rate = 2'000'000;
    if (avcodec_open2(context, codec, nullptr) < 0) {
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

AVFrame* makeSoftwareFrame() {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 64;
    frame->height = 64;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

} // namespace

// Exercises the production seam ReplayManager::onFrameTimecode -> m_tcAligner.observe
// and the public sourcesFrameAligned()/sourceFrameOffset() queries that Phase 4
// consumes. onFrameTimecode is a private slot, so it is driven exactly as the
// queued StreamWorker::frameTimecode signal would drive it: by name via the
// meta-object, which is the same dispatch the real connect() uses.
class TestReplayManagerTimecode : public QObject {
    Q_OBJECT

private slots:
    void jamSyncedSourcesReportAligned();
    void offsetSourcesReportNotAlignedAndFrameOffset();
    void noTimecodeSourcesAreNotAligned();
    void streamWorkerCarriesSelectedEvidenceExactlyOnce();
    void delayedNativeOutputUsesEvidenceForPacketPts();
    void delayedNativeOutputAcrossGenerationResetDropsOldEvidence();
    void delayedSoftwareOutputUsesEvidenceForPacketPts();
    void delayedSoftwareCompletionAfterResetDropsEvidence();
    void completionAfterGenerationResetDropsOldEvidence();
    void sharedNativeGpuMuxCallbackReportsRejectedCompletion();
    void oldCaptureSessionCannotEnqueueAfterReplacement();
    void oldSessionEndCannotInvalidateNewSession();
    void identityBoundaryRotatesAtAuthorizedFrameIngress();
    void resetBetweenLatestValidationAndSoftwareSubmissionRejectsFrame();
    void resetBetweenLatestValidationAndNativeSubmissionRejectsFrame();
    void oldSessionDisconnectCannotMutateReplacement();
    void oldSessionAudioCannotMutateReplacement();
    void oldSessionStatsCannotEmitForReplacement();
    void oppositeConnectionTransitionsRemainOrdered();
    void reentrantConnectionTransitionDoesNotDeadlock();
    void queuedTimecodeIsRejectedAfterCarrierRotation();
    void queuedTimecodeIsRejectedAfterWorkerRemoval();
    void queuedTimecodeIsRejectedAfterWorkerReplacementWithSameEpoch();
    void muxSubmissionCapturesCarrierEpochAtomically();
    void concurrentOldCarrierCannotCreateCurrentEntry();
    void concurrentCarrierRotationNeverErasesNewEpochEntry();
    void unstampedEvidenceCannotEncodeOrSeedTimecode();
    void callbackPoolsRejectExhaustionBeforeEvidenceConsumption();
    void callbackPoolsRejectStaleExactCarrierBeforeInsertion();
    void callbackPoolGenerationRejectsAbaCompletion();
    void callbackDescriptorsAndSlotLifecycleAllocateNothing();
    void nativeFailureAfterPacketDoesNotWritePartialOutput();
    void nativeTwoPacketBatchRejectsBeforePartialCommit();
    void nativeMultiPtsBatchConsumesEveryDistinctEvidenceMapping();
    void nativeDuplicatePtsBatchSharesOneEvidenceMapping();
    void nativeMultiPtsBatchMissingEvidenceConsumesNothingAndCommitsNothing();
    void boundedDriftReportsNonzeroUiBound();
    void overConfidenceTimecodeDoesNotMoveServo();
    void disconnectClearsTimecodeAnchor();
    void urlReplacementClearsTimecodeAnchor();
    void generationAndRateChangesReanchor();
    void sourceFrameOffsetRoundsSymmetricallyAtHalfFrame();
    void discontinuityRequiresFreshAnchor();
    void legalRolloverSurvivesTypedPipeline();

    // Phase 4 Task 3: reference-source selection + inter-cam phase estimation.
    void referenceIsHighestClockQualityTieLowestIndex();
    void timecodeAlignedSourceGradesFrameAccurate();
    void lockedNoTimecodeSourceGradesBoundedWithOffset();
    void arrivalOnlySourceGradesApproximate();
    void referenceSourceHasZeroPhase();
    void disconnectReselectsReferenceAwayFromDeadSource();
    void allSourcesDisconnectedClearServoBeforeReconnect();

    // Phase 4 Task 4: the bounded, gentle phase servo.
    void servoSignPullsLateSourceEarlier();
    void servoRampsTowardTargetNotFullJump();
    void servoNeverExceedsCap();
    void referenceAndApproximateGetNoServo();
    void ineligibleClockRelaxesExistingServoTrim();
    void singleSourceServoIsZero();
    void servoUsesExactTcOffsetWhenCommonTimecode();

    // Phase 4 Task 5: the relayed IngestStats carries the estimator's tier/phase.
    void relayedStatsCarryTierPhaseAndReference();
    void relayedStatsLeavePreExistingFieldsUntouched();

    // Phase 5 Task 6: the reference tier is surfaced for the UI. Default (no PTP,
    // no startRecording → no TimingReference built yet) reports LocalMonotonic and
    // not-external, so the accessor the UIManager binds to is byte-identical to today.
    void referenceTierDefaultsToLocalMonotonic();

private:
    static int64_t tcFrames(int h, int m, int s, int f) {
        return Smpte12m::toFrameCount(Smpte12mTimecode{h, m, s, f, /*drop*/ false, /*valid*/ true},
                                      30);
    }

    static TimecodeEvidence evidence(int64_t tc, int64_t frame, int rateNum = 30, int rateDen = 1,
                                     uint64_t sourceGeneration = 1, uint64_t timingGeneration = 1,
                                     int64_t quantizationBoundUs = 0) {
        TimecodeEvidence value;
        value.frameOfDay = tc;
        value.labelRate = FrameRateQ{rateNum, rateDen};
        value.sourceGeneration = sourceGeneration;
        value.timingGeneration = timingGeneration;
        value.provenance = TimecodeProvenance::Ndi;
        value.arrivalSessionFrame = frame;
        value.sessionRate = FrameRateQ{30, 1};
        value.quantizationBoundUs = quantizationBoundUs;
        return value;
    }

    static bool feedEvidence(ReplayManager& m, int src, const TimecodeEvidence& value) {
        return QMetaObject::invokeMethod(&m, "onFrameTimecode", Qt::DirectConnection,
                                         Q_ARG(int, src), Q_ARG(uint64_t, uint64_t(0)),
                                         Q_ARG(uint64_t, uint64_t(0)),
                                         Q_ARG(TimecodeEvidence, value));
    }

    static bool feedFrameTimecode(ReplayManager& m, int src, int64_t tc, int64_t frame,
                                  int rateNum = 30, int rateDen = 1) {
        return feedEvidence(m, src, evidence(tc, frame, rateNum, rateDen));
    }

    // Drives the production seam ReplayManager::onSourceStatsUpdated exactly as the
    // queued StreamWorker::statsUpdated signal would: by name via the meta-object.
    static bool feedStats(ReplayManager& m, int src, const IngestStats& stats) {
        return QMetaObject::invokeMethod(&m, "onSourceStatsUpdated", Qt::DirectConnection,
                                         Q_ARG(int, src), Q_ARG(IngestStats, stats));
    }

    static IngestStats clockStats(ClockQuality q, bool locked, int64_t offsetNs, double ppm = 0.0) {
        IngestStats s;
        s.clockQuality = int(q);
        s.clockLocked = locked;
        s.clockOffsetNs = offsetNs;
        s.clockPpm = ppm;
        return s;
    }
};

void TestReplayManagerTimecode::jamSyncedSourcesReportAligned() {
    ReplayManager manager;
    // Two jam-synced sources: 01:00:00:00 lands on the SAME session frame 100.
    QVERIFY(feedFrameTimecode(manager, 0, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tcFrames(1, 0, 0, 0), 100));

    QVERIFY(manager.sourcesFrameAligned(0, 1));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(0));
}

void TestReplayManagerTimecode::offsetSourcesReportNotAlignedAndFrameOffset() {
    ReplayManager manager;
    // Same TC, but source 1's TC arrived 3 session frames LATER than source 0's.
    QVERIFY(feedFrameTimecode(manager, 0, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tcFrames(1, 0, 0, 0), 103));

    QVERIFY(!manager.sourcesFrameAligned(0, 1));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(-3)); // pull B back 3 frames
}

void TestReplayManagerTimecode::noTimecodeSourcesAreNotAligned() {
    ReplayManager manager;
    // Only source 0 ever carried TC; -1 timecodes are ignored by the aligner.
    QVERIFY(feedFrameTimecode(manager, 0, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, int64_t(-1), 100));

    QVERIFY(!manager.sourcesFrameAligned(0, 1));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(0));
}

void TestReplayManagerTimecode::streamWorkerCarriesSelectedEvidenceExactlyOnce() {
    QTemporaryDir output;
    QVERIFY(output.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-production-path"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2, QStringLiteral("01:00:00:00")));

    ReplayManager manager;
    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1);
    worker.beginCaptureSession();
    manager.m_workers.append(&worker);
    QVERIFY(QObject::connect(&worker, &StreamWorker::frameTimecode, &manager,
                             &ReplayManager::onFrameTimecode, Qt::QueuedConnection));
    worker.moveToThread(&worker);
    int deliveryCount = 0;
    TimecodeEvidence delivered;
    QThread* deliveryThread = nullptr;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&](int sourceIndex, uint64_t, uint64_t, TimecodeEvidence value) {
            QCOMPARE(sourceIndex, 0);
            ++deliveryCount;
            delivered = value;
            deliveryThread = QThread::currentThread();
        },
        Qt::QueuedConnection));

    worker.start();
    auto cleanup = qScopeGuard([&] {
        worker.stop();
        worker.wait();
        manager.m_workers.clear();
        muxer.close();
    });
    QTRY_VERIFY_WITH_TIMEOUT(worker.isRunning(), 2000);

    QThread* eventLoopThread = nullptr;
    QVERIFY(QMetaObject::invokeMethod(
        &worker, [&] { eventLoopThread = QThread::currentThread(); },
        Qt::BlockingQueuedConnection));
    QCOMPARE(eventLoopThread, static_cast<QThread*>(&worker));

    DecodedVideoFrame decoded;
    decoded.frame = av_frame_alloc();
    QVERIFY(decoded.frame != nullptr);
    decoded.frame->format = AV_PIX_FMT_YUV420P;
    decoded.frame->width = 64;
    decoded.frame->height = 64;
    QVERIFY(av_frame_get_buffer(decoded.frame, 32) >= 0);
    QVERIFY(av_frame_make_writable(decoded.frame) >= 0);
    memset(decoded.frame->data[0], 96,
           static_cast<size_t>(decoded.frame->linesize[0]) * decoded.frame->height);
    memset(decoded.frame->data[1], 128,
           static_cast<size_t>(decoded.frame->linesize[1]) * (decoded.frame->height / 2));
    memset(decoded.frame->data[2], 128,
           static_cast<size_t>(decoded.frame->linesize[2]) * (decoded.frame->height / 2));
    decoded.sourcePtsMs = 0;
    decoded.timecodeEvidence = evidence(tcFrames(1, 0, 0, 0), 999, 30, 1, 7, 11, 250);
    worker.enqueueDecodedVideoFrame(std::move(decoded));

    // An unmapped tick selects the decoded frame but must not encode, mux, or emit.
    QVERIFY(QMetaObject::invokeMethod(
        &worker, [&] { worker.onMasterPulse(9, 300); }, Qt::QueuedConnection));
    QVERIFY(QMetaObject::invokeMethod(&worker, [] {}, Qt::BlockingQueuedConnection));
    QCOMPARE(deliveryCount, 0);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));

    // The mapped tick traverses the real software encoder and Muxer writer. Evidence
    // is emitted only from the successful async disk-completion callback.
    worker.setViewTrack(0);
    QVERIFY(QMetaObject::invokeMethod(
        &worker, [&] { worker.onMasterPulse(10, 333); }, Qt::QueuedConnection));
    QVERIFY(QMetaObject::invokeMethod(
        &worker, [&] { worker.onMasterPulse(11, 366); }, Qt::QueuedConnection));
    QTRY_COMPARE_WITH_TIMEOUT(deliveryCount, 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(muxer.minWrittenVideoPtsMs() >= 0, 5000);
    const int64_t firstWrittenPtsMs = muxer.minWrittenVideoPtsMs();
    QCOMPARE(deliveryThread, QCoreApplication::instance()->thread());
    QCOMPARE(delivered.frameOfDay, tcFrames(1, 0, 0, 0));
    QCOMPARE(delivered.labelRate, (FrameRateQ{30, 1}));
    QCOMPARE(delivered.sourceGeneration, uint64_t(7));
    QCOMPARE(delivered.timingGeneration, uint64_t(11));
    QCOMPARE(delivered.provenance, TimecodeProvenance::Ndi);
    QCOMPARE(delivered.quantizationBoundUs, int64_t(250));
    // The encoder delays the packet until the following tick, but evidence remains
    // bound to the input frame whose PTS the packet actually carries.
    QCOMPARE(delivered.arrivalSessionFrame, int64_t(10));
    QCOMPARE(delivered.sessionRate, (FrameRateQ{30, 1}));

    TimecodeEvidence follower = delivered;
    QVERIFY(feedEvidence(manager, 1, follower));
    QTRY_VERIFY_WITH_TIMEOUT(manager.sourcesFrameAligned(0, 1), 2000);

    // A held-frame tick can write another packet, but the selected evidence was
    // consumed by the first successful completion and must never be emitted twice.
    QVERIFY(QMetaObject::invokeMethod(
        &worker, [&] { worker.onMasterPulse(12, 400); }, Qt::QueuedConnection));
    QTRY_VERIFY_WITH_TIMEOUT(muxer.minWrittenVideoPtsMs() > firstWrittenPtsMs, 5000);
    QCOMPARE(deliveryCount, 1);

    DecodedVideoFrame rejected;
    rejected.frame = av_frame_alloc();
    QVERIFY(rejected.frame != nullptr);
    rejected.frame->format = AV_PIX_FMT_YUV420P;
    rejected.frame->width = 64;
    rejected.frame->height = 64;
    QVERIFY(av_frame_get_buffer(rejected.frame, 32) >= 0);
    rejected.sourcePtsMs = 0;
    rejected.timecodeEvidence = evidence(tcFrames(2, 0, 0, 0), 1000, 30, 1, 8, 12, 500);
    worker.enqueueDecodedVideoFrame(std::move(rejected));
    worker.m_beforeMuxPacketWriteForTest = [&muxer] { muxer.close(); };
    QVERIFY(QMetaObject::invokeMethod(
        &worker, [&] { worker.onMasterPulse(13, 433); }, Qt::QueuedConnection));
    QVERIFY(QMetaObject::invokeMethod(&worker, [] {}, Qt::BlockingQueuedConnection));
    QTest::qWait(100);
    QCOMPARE(deliveryCount, 1);

    worker.stop();
    QVERIFY(worker.wait(5000));
    manager.m_workers.clear();
    muxer.close();
    cleanup.dismiss();

    const QFileInfo recording(output.filePath(QStringLiteral("timecode-production-path.mkv")));
    QVERIFY(recording.exists());
    QVERIFY(recording.size() > 0);
}

void TestReplayManagerTimecode::delayedNativeOutputUsesEvidenceForPacketPts() {
    QTemporaryDir output;
    QVERIFY(output.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-delayed-native"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.m_nativeEncoder = std::make_unique<OneFrameDelayedEncoder>();
    worker.setViewTrack(0);
    worker.m_latestFrame = av_frame_alloc();
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.m_latestFrame->format = AV_PIX_FMT_YUV420P;
    worker.m_latestFrame->width = 64;
    worker.m_latestFrame->height = 64;
    QVERIFY(av_frame_get_buffer(worker.m_latestFrame, 32) >= 0);
    worker.beginCaptureSession();
    worker.m_latestFrameCarrierToken = worker.snapshotActiveCarrierToken();

    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int sourceIndex, uint64_t, uint64_t, TimecodeEvidence value) {
            QCOMPARE(sourceIndex, 0);
            delivered.append(value);
        },
        Qt::QueuedConnection));

    const TimecodeEvidence first = evidence(tcFrames(1, 0, 0, 0), 10, 30, 1, 7, 11);
    worker.m_latestFrameTimecodeEvidence = first;
    worker.m_latestFrameTimecode100ns.store(1, std::memory_order_release);
    worker.m_internalFrameCount = 10;
    worker.processEncoderTick(nullptr, 333, 0, 0);
    QCOMPARE(delivered.size(), 0);

    const TimecodeEvidence second = evidence(tcFrames(2, 0, 0, 0), 11, 30, 1, 7, 11);
    worker.m_latestFrameTimecodeEvidence = second;
    worker.m_latestFrameTimecode100ns.store(2, std::memory_order_release);
    worker.m_internalFrameCount = 11;
    worker.processEncoderTick(nullptr, 366, 0, 0);

    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, 5000);
    QCOMPARE(delivered.front().frameOfDay, first.frameOfDay);
    QCOMPARE(delivered.front().sourceGeneration, first.sourceGeneration);
    QCOMPARE(delivered.front().timingGeneration, first.timingGeneration);
    QCOMPARE(delivered.front().arrivalSessionFrame, int64_t(10));

    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::delayedNativeOutputAcrossGenerationResetDropsOldEvidence() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const QByteArray previousGrace = qgetenv("OLR_MUXER_TMCD_GRACE_MS");
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "0");
    const auto restoreGrace = qScopeGuard([previousGrace] {
        if (previousGrace.isNull())
            qunsetenv("OLR_MUXER_TMCD_GRACE_MS");
        else
            qputenv("OLR_MUXER_TMCD_GRACE_MS", previousGrace);
    });

    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-delayed-reset"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.m_nativeEncoder = std::make_unique<OneFrameDelayedEncoder>();
    worker.setViewTrack(0);
    worker.m_latestFrame = av_frame_alloc();
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.m_latestFrame->format = AV_PIX_FMT_YUV420P;
    worker.m_latestFrame->width = 64;
    worker.m_latestFrame->height = 64;
    QVERIFY(av_frame_get_buffer(worker.m_latestFrame, 32) >= 0);
    const uint64_t session = worker.beginCaptureSession();

    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int, uint64_t, uint64_t, TimecodeEvidence value) { delivered.append(value); },
        Qt::QueuedConnection));

    const TimecodeEvidence oldGeneration = evidence(tcFrames(1, 0, 0, 0), 20, 30, 1, 7, 11);
    worker.m_latestFrameTimecodeEvidence = oldGeneration;
    const auto oldToken = worker.prepareCarrierTokenForFrameIngress(session, oldGeneration);
    worker.m_latestFrameCarrierToken = oldToken;
    worker.m_internalFrameCount = 20;
    worker.processEncoderTick(nullptr, 666, 0, 0);

    const TimecodeEvidence firstNewGeneration = evidence(tcFrames(2, 0, 0, 0), 21, 30, 1, 8, 12);
    worker.m_latestFrameTimecodeEvidence = firstNewGeneration;
    const auto tokenBeforeAuthorizedIngress = worker.snapshotActiveCarrierToken();
    QVERIFY(tokenBeforeAuthorizedIngress);
    QCOMPARE(tokenBeforeAuthorizedIngress->epoch, oldToken->epoch);
    const auto firstNewToken =
        worker.prepareCarrierTokenForFrameIngress(session, firstNewGeneration);
    QVERIFY(firstNewToken->epoch != oldToken->epoch);
    worker.m_latestFrameCarrierToken = firstNewToken;
    worker.m_internalFrameCount = 21;
    worker.processEncoderTick(nullptr, 700, 0, 0);
    QTest::qWait(100);
    QCOMPARE(delivered.size(), 0);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));

    const TimecodeEvidence secondNewGeneration = evidence(tcFrames(2, 0, 0, 1), 22, 30, 1, 8, 12);
    worker.m_latestFrameTimecodeEvidence = secondNewGeneration;
    worker.m_latestFrameCarrierToken =
        worker.prepareCarrierTokenForFrameIngress(session, secondNewGeneration);
    worker.m_internalFrameCount = 22;
    worker.processEncoderTick(nullptr, 733, 0, 0);

    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, 5000);
    QCOMPARE(delivered.front().frameOfDay, firstNewGeneration.frameOfDay);
    QCOMPARE(delivered.front().sourceGeneration, uint64_t(8));
    QCOMPARE(delivered.front().timingGeneration, uint64_t(12));
    QCOMPARE(delivered.front().arrivalSessionFrame, int64_t(21));

    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::delayedSoftwareOutputUsesEvidenceForPacketPts() {
    QTemporaryDir output;
    QVERIFY(output.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-delayed-software"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1);
    worker.setViewTrack(0);
    worker.m_latestFrame = makeSoftwareFrame();
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.beginCaptureSession();
    worker.m_latestFrameCarrierToken = worker.snapshotActiveCarrierToken();
    AVCodecContext* encoder = makeDelayedMpeg2Encoder();
    QVERIFY(encoder != nullptr);

    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int sourceIndex, uint64_t, uint64_t, TimecodeEvidence value) {
            QCOMPARE(sourceIndex, 0);
            delivered.append(value);
        },
        Qt::QueuedConnection));

    const TimecodeEvidence first = evidence(tcFrames(1, 0, 0, 0), 40, 30, 1, 7, 11);
    worker.m_latestFrameTimecodeEvidence = first;
    worker.m_latestFrameTimecode100ns.store(1, std::memory_order_release);
    worker.m_internalFrameCount = 40;
    worker.processEncoderTick(encoder, 1333, 0, 0);
    QCOMPARE(delivered.size(), 0);

    const TimecodeEvidence second = evidence(tcFrames(2, 0, 0, 0), 41, 30, 1, 7, 11);
    worker.m_latestFrameTimecodeEvidence = second;
    worker.m_latestFrameTimecode100ns.store(2, std::memory_order_release);
    worker.m_internalFrameCount = 41;
    worker.processEncoderTick(encoder, 1366, 0, 0);

    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, 5000);
    QCOMPARE(delivered.front().frameOfDay, first.frameOfDay);
    QCOMPARE(delivered.front().arrivalSessionFrame, int64_t(40));

    avcodec_free_context(&encoder);
    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::delayedSoftwareCompletionAfterResetDropsEvidence() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const QByteArray previousGrace = qgetenv("OLR_MUXER_TMCD_GRACE_MS");
    qputenv("OLR_MUXER_TMCD_GRACE_MS", "0");
    const auto restoreGrace = qScopeGuard([previousGrace] {
        if (previousGrace.isNull())
            qunsetenv("OLR_MUXER_TMCD_GRACE_MS");
        else
            qputenv("OLR_MUXER_TMCD_GRACE_MS", previousGrace);
    });

    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    const QString baseName = QStringLiteral("timecode-delayed-software-reset");
    QVERIFY(muxer.init(baseName, 1, 64, 64, 30, {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1);
    worker.setViewTrack(0);
    worker.m_latestFrame = makeSoftwareFrame();
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.beginCaptureSession();
    worker.m_latestFrameCarrierToken = worker.snapshotActiveCarrierToken();
    AVCodecContext* encoder = makeDelayedMpeg2Encoder();
    QVERIFY(encoder != nullptr);

    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int, uint64_t, uint64_t, TimecodeEvidence value) { delivered.append(value); },
        Qt::QueuedConnection));

    const TimecodeEvidence first = evidence(tcFrames(1, 0, 0, 0), 50, 30, 1, 7, 11);
    worker.m_latestFrameTimecodeEvidence = first;
    worker.m_latestFrameTimecode100ns.store(1, std::memory_order_release);
    worker.m_internalFrameCount = 50;
    worker.processEncoderTick(encoder, 1666, 0, 0);
    QCOMPARE(delivered.size(), 0);

    worker.m_latestFrameTimecodeEvidence.reset();
    worker.m_internalFrameCount = 51;
    worker.m_beforeMuxPacketWriteForTest = [&worker] { worker.clearMuxFrameEvidence(); };
    worker.processEncoderTick(encoder, 1700, 0, 0);

    QTest::qWait(100);
    QCOMPARE(delivered.size(), 0);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));

    // MPEG-2 B-frame order emits post-reset frame 52 next. It is evidence-free but
    // properly mapped, so it remains valid and becomes the first written packet.
    worker.m_latestFrameCarrierToken = worker.snapshotActiveCarrierToken();
    worker.m_internalFrameCount = 52;
    worker.processEncoderTick(encoder, 1733, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(muxer.minWrittenVideoPtsMs(), int64_t(1733), 5000);

    // The following B-frame output belongs to pre-reset frame 51, whose mapping
    // was cleared. It must be rejected rather than lowering the written minimum
    // to 1700 ms as the old epoch-inference behavior did.
    worker.m_internalFrameCount = 53;
    worker.processEncoderTick(encoder, 1766, 0, 0);
    QTest::qWait(100);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(1733));

    avcodec_free_context(&encoder);
    av_frame_free(&worker.m_latestFrame);
    muxer.close();

    AVFormatContext* input = nullptr;
    const QByteArray path = output.filePath(baseName + QStringLiteral(".mkv")).toUtf8();
    QVERIFY(avformat_open_input(&input, path.constData(), nullptr, nullptr) >= 0);
    const auto closeInput = qScopeGuard([&input] { avformat_close_input(&input); });
    QVERIFY(avformat_find_stream_info(input, nullptr) >= 0);
    QVERIFY2(av_dict_get(input->metadata, "timecode", nullptr, 0) == nullptr,
             "a stale software completion must not seed the recording timecode tag");
}

void TestReplayManagerTimecode::completionAfterGenerationResetDropsOldEvidence() {
    QTemporaryDir output;
    QVERIFY(output.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-completion-reset"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));
    AVStream* stream = muxer.getStream(0);
    QVERIFY(stream != nullptr);

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1);
    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int, uint64_t, uint64_t, TimecodeEvidence value) { delivered.append(value); },
        Qt::QueuedConnection));

    const TimecodeEvidence oldGeneration = evidence(tcFrames(1, 0, 0, 0), 30, 30, 1, 7, 11);
    const TimecodeEvidence newGeneration = evidence(tcFrames(2, 0, 0, 0), 31, 30, 1, 8, 12);
    const uint64_t session = worker.beginCaptureSession();
    const auto oldToken = worker.prepareCarrierTokenForFrameIngress(session, oldGeneration);
    const uint64_t callbackSlot =
        worker.acquireEncodeSubmission(false, 0, stream, nullptr, *oldToken, 0);
    QVERIFY(callbackSlot != 0);
    const auto oldSubmission = worker.enqueueMuxFrameEvidence(30, -1, oldGeneration, oldToken);
    QVERIFY(oldSubmission.id != 0);
    QVERIFY(worker.setEncodeSubmissionEvidenceId(callbackSlot, oldSubmission.id));
    worker.m_beforeMuxPacketWriteForTest = [&worker, newGeneration] {
        const auto token = worker.prepareCarrierTokenForFrameIngress(
            worker.m_activeCaptureSessionIdentity, newGeneration);
        worker.enqueueMuxFrameEvidence(31, -1, newGeneration, token);
    };

    auto callback = worker.packetCallbackForSubmission(callbackSlot);
    callback(QByteArray::fromHex("000001b300100113"), 30, true);
    worker.finishEncodeSubmission(callbackSlot);
    QTest::qWait(100);
    QCOMPARE(delivered.size(), 0);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));

    muxer.close();
}

void TestReplayManagerTimecode::sharedNativeGpuMuxCallbackReportsRejectedCompletion() {
    QTemporaryDir output;
    QVERIFY(output.isValid());

    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-shared-callback"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2, QStringLiteral("01:00:00:00")));
    AVStream* stream = muxer.getStream(0);
    QVERIFY(stream != nullptr);

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1);
    bool acceptedPacket = false;
    worker.m_beforeMuxPacketWriteForTest = [&muxer] { muxer.close(); };
    const auto syntheticToken = worker.snapshotActiveCarrierToken();
    QVERIFY(syntheticToken);
    const uint64_t callbackSlot =
        worker.acquireEncodeSubmission(false, 0, stream, &acceptedPacket, *syntheticToken, 0);
    QVERIFY(callbackSlot != 0);
    const auto mapping = worker.enqueueMuxFrameEvidence(1, -1, std::nullopt, syntheticToken, true);
    QVERIFY(mapping.id != 0);
    QVERIFY(worker.setEncodeSubmissionEvidenceId(callbackSlot, mapping.id));
    auto callback = worker.packetCallbackForSubmission(callbackSlot);

    callback(QByteArray::fromHex("000001b3"), 1, true);
    worker.finishEncodeSubmission(callbackSlot);
    QVERIFY(!acceptedPacket);
    for (size_t i = 0; i < StreamWorker::kSubmissionPoolCapacity; ++i)
        QVERIFY(!worker.m_submissionPool[i].active);
    for (size_t i = 0; i < StreamWorker::kMuxCompletionPoolCapacity; ++i)
        QVERIFY(!worker.m_muxCompletionPool[i].active);
}

void TestReplayManagerTimecode::oldCaptureSessionCannotEnqueueAfterReplacement() {
    StreamWorker worker(QStringLiteral("old"), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const uint64_t oldSession = worker.beginCaptureSession();
    worker.changeSource(QStringLiteral("new"));

    DecodedVideoFrame late;
    late.frame = makeSoftwareFrame();
    QVERIFY(late.frame != nullptr);
    late.sourcePtsMs = 0;
    late.timecodeEvidence = evidence(tcFrames(1, 0, 0, 0), 1, 30, 1, 7, 11);
    worker.enqueueDecodedVideoFrameForSession(std::move(late), oldSession);
    QCOMPARE(worker.m_frameQueue.size(), 0);

    const uint64_t newSession = worker.beginCaptureSession();
    DecodedVideoFrame current;
    current.frame = makeSoftwareFrame();
    QVERIFY(current.frame != nullptr);
    current.sourcePtsMs = 0;
    // Deliberately reuse the same sourceGeneration: session identity, not decoder
    // generation, is what rejects the late callback.
    current.timecodeEvidence = evidence(tcFrames(1, 0, 0, 1), 2, 30, 1, 7, 11);
    worker.enqueueDecodedVideoFrameForSession(std::move(current), newSession);
    QCOMPARE(worker.m_frameQueue.size(), 1);
    auto queued = worker.m_frameQueue.dequeue();
    av_frame_free(&queued.frame);
}

void TestReplayManagerTimecode::oldSessionEndCannotInvalidateNewSession() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const uint64_t oldSession = worker.beginCaptureSession();
    const uint64_t newSession = worker.beginCaptureSession();
    const uint64_t newEpoch = worker.currentCarrierEpoch();

    worker.endCaptureSession(oldSession);

    QCOMPARE(worker.currentCarrierEpoch(), newEpoch);
    QVERIFY(worker.snapshotCarrierTokenForSession(newSession) != nullptr);
}

void TestReplayManagerTimecode::identityBoundaryRotatesAtAuthorizedFrameIngress() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const uint64_t session = worker.beginCaptureSession();

    DecodedVideoFrame first;
    first.frame = makeSoftwareFrame();
    QVERIFY(first.frame != nullptr);
    first.timecodeEvidence = evidence(tcFrames(1, 0, 0, 0), 1, 30, 1, 7, 11);
    worker.enqueueDecodedVideoFrameForSession(std::move(first), session);
    QCOMPARE(worker.m_frameQueue.size(), 1);
    const uint64_t firstEpoch = worker.m_frameQueue.front().carrierToken->epoch;

    DecodedVideoFrame boundary;
    boundary.frame = makeSoftwareFrame();
    QVERIFY(boundary.frame != nullptr);
    boundary.timecodeEvidence = evidence(tcFrames(2, 0, 0, 0), 2, 30, 1, 8, 12);
    worker.enqueueDecodedVideoFrameForSession(std::move(boundary), session);
    QCOMPARE(worker.m_frameQueue.size(), 2);
    const uint64_t boundaryEpoch = worker.m_frameQueue.back().carrierToken->epoch;
    QVERIFY(boundaryEpoch > firstEpoch);
    QCOMPARE(boundaryEpoch, worker.currentCarrierEpoch());

    while (!worker.m_frameQueue.isEmpty()) {
        auto queued = worker.m_frameQueue.dequeue();
        av_frame_free(&queued.frame);
    }
}

void TestReplayManagerTimecode::resetBetweenLatestValidationAndSoftwareSubmissionRejectsFrame() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("software-validation-reset"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));
    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1);
    worker.setViewTrack(0);
    worker.beginCaptureSession();
    worker.m_latestFrame = makeSoftwareFrame();
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.m_latestFrameCarrierToken = worker.snapshotActiveCarrierToken();
    worker.m_latestFrameTimecodeEvidence = evidence(tcFrames(1, 0, 0, 0), 1);
    worker.m_beforeMuxEvidenceSubmissionForTest = [&worker] { worker.clearMuxFrameEvidence(); };
    AVCodecContext* encoder = makeDelayedMpeg2Encoder();
    QVERIFY(encoder != nullptr);

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(encoder, 33, 0, 0);

    QCOMPARE(worker.m_muxFrameEvidence.size(), 0);
    avcodec_free_context(&encoder);
    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::resetBetweenLatestValidationAndNativeSubmissionRejectsFrame() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("native-validation-reset"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));
    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    worker.setViewTrack(0);
    worker.beginCaptureSession();
    worker.m_latestFrame = makeSoftwareFrame();
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.m_latestFrameCarrierToken = worker.snapshotActiveCarrierToken();
    worker.m_latestFrameTimecodeEvidence = evidence(tcFrames(1, 0, 0, 0), 1);
    auto encoder = std::make_unique<CountingNativeEncoder>();
    auto* encoderPtr = encoder.get();
    worker.m_nativeEncoder = std::move(encoder);
    worker.m_beforeMuxEvidenceSubmissionForTest = [&worker] { worker.clearMuxFrameEvidence(); };

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(nullptr, 33, 0, 0);

    QCOMPARE(encoderPtr->calls, 0);
    QCOMPARE(worker.m_muxFrameEvidence.size(), 0);
    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::oldSessionDisconnectCannotMutateReplacement() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const uint64_t oldSession = worker.beginCaptureSession();
    worker.setConnectedForSession(oldSession, true);
    const uint64_t newSession = worker.beginCaptureSession();

    worker.setConnectedForSession(oldSession, false);

    QVERIFY(worker.m_connected.load(std::memory_order_acquire));
    QVERIFY(worker.snapshotCarrierTokenForSession(newSession) != nullptr);
}

void TestReplayManagerTimecode::oldSessionAudioCannotMutateReplacement() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const uint64_t oldSession = worker.beginCaptureSession();
    worker.beginCaptureSession();
    const uint8_t pcm[8] = {};

    worker.enqueueAudioForSession(oldSession, 0, pcm, 2);

    QVERIFY(worker.m_audioFifo.isEmpty());
    QCOMPARE(worker.m_audioFifoStartSample, int64_t(-1));
}

void TestReplayManagerTimecode::oldSessionStatsCannotEmitForReplacement() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const uint64_t oldSession = worker.beginCaptureSession();
    worker.beginCaptureSession();
    int emissions = 0;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::statsUpdated, &worker,
        [&emissions](int, const IngestStats&) { ++emissions; }, Qt::DirectConnection));

    worker.reportStatsForSession(oldSession, IngestStats{});

    QCOMPARE(emissions, 0);
}

void TestReplayManagerTimecode::oppositeConnectionTransitionsRemainOrdered() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    std::mutex observedMutex;
    QList<bool> observed;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::connectionChanged, &worker,
        [&](int, bool connected) {
            std::lock_guard<std::mutex> lock(observedMutex);
            observed.append(connected);
        },
        Qt::DirectConnection));
    worker.setConnected(true);

    for (int iteration = 0; iteration < 50; ++iteration) {
        std::atomic<bool> go{false};
        std::thread disconnect([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            worker.setConnected(false);
        });
        std::thread reconnect([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            worker.setConnected(true);
        });
        go.store(true, std::memory_order_release);
        disconnect.join();
        reconnect.join();
        QCoreApplication::processEvents();

        std::lock_guard<std::mutex> lock(observedMutex);
        QVERIFY(!observed.isEmpty());
        QCOMPARE(observed.back(), worker.m_connected.load(std::memory_order_acquire));
    }
}

void TestReplayManagerTimecode::reentrantConnectionTransitionDoesNotDeadlock() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    QList<bool> observed;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::connectionChanged, &worker,
        [&](int, bool connected) {
            observed.append(connected);
            if (connected) worker.setConnected(false);
        },
        Qt::DirectConnection));

    worker.setConnected(true);
    QCoreApplication::processEvents();

    QCOMPARE(observed, (QList<bool>{true, false}));
    QVERIFY(!worker.m_connected.load(std::memory_order_acquire));
}

void TestReplayManagerTimecode::queuedTimecodeIsRejectedAfterCarrierRotation() {
    ReplayManager manager;
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    manager.m_workers.append(&worker);
    QVERIFY(QObject::connect(&worker, &StreamWorker::frameTimecode, &manager,
                             &ReplayManager::onFrameTimecode, Qt::QueuedConnection));

    const uint64_t postedEpoch = worker.currentCarrierEpoch();
    worker.frameTimecode(0, worker.workerInstanceIdentity(), postedEpoch,
                         evidence(tcFrames(1, 0, 0, 0), 10));
    worker.clearMuxFrameEvidence();
    QCoreApplication::processEvents();

    QVERIFY(!manager.m_tcAligner.hasTimecode(0));
    manager.m_workers.clear();
}

void TestReplayManagerTimecode::queuedTimecodeIsRejectedAfterWorkerRemoval() {
    ReplayManager manager;
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    manager.m_workers.append(&worker);
    QVERIFY(QObject::connect(&worker, &StreamWorker::frameTimecode, &manager,
                             &ReplayManager::onFrameTimecode, Qt::QueuedConnection));

    worker.frameTimecode(0, worker.workerInstanceIdentity(), worker.currentCarrierEpoch(),
                         evidence(tcFrames(1, 0, 0, 0), 10));
    manager.m_workers.clear();
    QCoreApplication::processEvents();

    QVERIFY(!manager.m_tcAligner.hasTimecode(0));
}

void TestReplayManagerTimecode::queuedTimecodeIsRejectedAfterWorkerReplacementWithSameEpoch() {
    ReplayManager manager;
    StreamWorker oldWorker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    StreamWorker replacementWorker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    QCOMPARE(oldWorker.currentCarrierEpoch(), replacementWorker.currentCarrierEpoch());

    manager.m_workers.append(&oldWorker);
    QVERIFY(QObject::connect(&oldWorker, &StreamWorker::frameTimecode, &manager,
                             &ReplayManager::onFrameTimecode, Qt::QueuedConnection));
    oldWorker.frameTimecode(0, oldWorker.workerInstanceIdentity(), oldWorker.currentCarrierEpoch(),
                            evidence(tcFrames(1, 0, 0, 0), 10));

    manager.m_workers[0] = &replacementWorker;
    QVERIFY(QObject::connect(&replacementWorker, &StreamWorker::frameTimecode, &manager,
                             &ReplayManager::onFrameTimecode, Qt::QueuedConnection));
    QCoreApplication::processEvents();
    QVERIFY(!manager.m_tcAligner.hasTimecode(0));

    replacementWorker.frameTimecode(0, replacementWorker.workerInstanceIdentity(),
                                    replacementWorker.currentCarrierEpoch(),
                                    evidence(tcFrames(1, 0, 0, 1), 11));
    QCoreApplication::processEvents();
    QVERIFY(manager.m_tcAligner.hasTimecode(0));
    manager.m_workers.clear();
}

void TestReplayManagerTimecode::muxSubmissionCapturesCarrierEpochAtomically() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    worker.beginCaptureSession();
    const uint64_t originalEpoch = worker.currentCarrierEpoch();
    const TimecodeEvidence value = evidence(tcFrames(1, 0, 0, 0), 1, 30, 1, 7, 11);
    const auto token =
        worker.prepareCarrierTokenForFrameIngress(worker.m_activeCaptureSessionIdentity, value);

    const auto submission = worker.enqueueMuxFrameEvidence(1, -1, value, token);
    QCOMPARE(submission.carrierToken.sessionIdentity, token->sessionIdentity);
    QCOMPARE(submission.carrierToken.epoch, originalEpoch);
    QVERIFY(submission.id > 0);

    worker.clearMuxFrameEvidence();
    QVERIFY(worker.currentCarrierEpoch() != submission.carrierToken.epoch);
    QVERIFY(!worker.carrierTokenIsCurrent(submission.carrierToken));
}

void TestReplayManagerTimecode::concurrentOldCarrierCannotCreateCurrentEntry() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const uint64_t session = worker.beginCaptureSession();
    const TimecodeEvidence value = evidence(tcFrames(1, 0, 0, 0), 1, 30, 1, 7, 11);
    const auto oldToken = worker.prepareCarrierTokenForFrameIngress(session, value);
    std::atomic<bool> go{false};
    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        worker.enqueueMuxFrameEvidence(1, -1, value, oldToken);
    });
    std::thread rotator([&] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        worker.clearMuxFrameEvidence();
    });
    go.store(true, std::memory_order_release);
    producer.join();
    rotator.join();

    QCOMPARE(worker.m_muxFrameEvidence.size(), 0);
}

void TestReplayManagerTimecode::concurrentCarrierRotationNeverErasesNewEpochEntry() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    worker.beginCaptureSession();
    const TimecodeEvidence value = evidence(tcFrames(1, 0, 0, 0), 1, 30, 1, 7, 11);

    for (int iteration = 0; iteration < 50; ++iteration) {
        std::atomic<bool> go{false};
        const int64_t pts = 100 + iteration;
        const auto token =
            worker.prepareCarrierTokenForFrameIngress(worker.m_activeCaptureSessionIdentity, value);
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            worker.enqueueMuxFrameEvidence(pts, -1, value, token);
        });
        std::thread rotator([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            worker.clearMuxFrameEvidence();
        });
        go.store(true, std::memory_order_release);
        producer.join();
        rotator.join();

        QVERIFY(!worker.takeMuxFrameEvidence(pts).has_value());
        worker.clearMuxFrameEvidence();
    }
}

void TestReplayManagerTimecode::unstampedEvidenceCannotEncodeOrSeedTimecode() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-unstamped"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1);
    worker.setViewTrack(0);
    worker.m_latestFrame = makeSoftwareFrame();
    QVERIFY(worker.m_latestFrame != nullptr);
    worker.m_latestFrameTimecodeEvidence = evidence(tcFrames(1, 0, 0, 0), 1);
    worker.m_latestFrameTimecode100ns.store(1, std::memory_order_release);
    worker.m_latestFrameCarrierToken.reset();
    AVCodecContext* encoder = makeDelayedMpeg2Encoder();
    QVERIFY(encoder != nullptr);

    worker.m_internalFrameCount = 1;
    worker.processEncoderTick(encoder, 33, 0, 0);
    QTest::qWait(50);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));

    avcodec_free_context(&encoder);
    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::callbackPoolsRejectExhaustionBeforeEvidenceConsumption() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-callback-pool-full"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const uint64_t session = worker.beginCaptureSession();
    const TimecodeEvidence value = evidence(tcFrames(1, 0, 0, 0), 1, 30, 1, 7, 11);
    const auto token = worker.prepareCarrierTokenForFrameIngress(session, value);
    QVERIFY(token);

    std::array<uint64_t, StreamWorker::kSubmissionPoolCapacity> submissions{};
    for (uint64_t& id : submissions) {
        id = worker.acquireEncodeSubmission(false, 0, muxer.getStream(0), nullptr, *token, 0);
        QVERIFY(id != 0);
    }
    QCOMPARE(worker.acquireEncodeSubmission(false, 0, muxer.getStream(0), nullptr, *token, 0),
             uint64_t(0));

    auto encoder = std::make_unique<CountingNativeEncoder>();
    CountingNativeEncoder* encoderProbe = encoder.get();
    worker.m_nativeEncoder = std::move(encoder);
    worker.setViewTrack(0);
    worker.m_latestFrame = makeSoftwareFrame();
    QVERIFY(worker.m_latestFrame);
    worker.m_latestFrameCarrierToken = token;
    worker.m_latestFrameTimecodeEvidence = value;
    worker.m_latestFrameTimecode100ns.store(123, std::memory_order_release);
    worker.processEncoderTick(nullptr, 0, 0, 0);
    QCOMPARE(encoderProbe->calls, 0);
    QVERIFY(worker.m_latestFrameTimecodeEvidence.has_value());

    for (uint64_t id : submissions)
        worker.finishEncodeSubmission(id);

    const auto mapping = worker.enqueueMuxFrameEvidence(7, 123, value, token);
    QVERIFY(mapping.id != 0);
    std::array<uint64_t, StreamWorker::kMuxCompletionPoolCapacity> completions{};
    for (uint64_t& id : completions) {
        id = worker.reserveMuxCompletion(0);
        QVERIFY(id != 0);
    }
    QCOMPARE(worker.reserveMuxCompletion(0), uint64_t(0));
    QVERIFY(worker.takeMuxFrameEvidence(7).has_value());
    for (uint64_t id : completions)
        worker.releaseMuxCompletionReservation(id);

    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::callbackPoolsRejectStaleExactCarrierBeforeInsertion() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const uint64_t oldSession = worker.beginCaptureSession();
    const auto oldToken =
        worker.prepareCarrierTokenForFrameIngress(oldSession, evidence(tcFrames(1, 0, 0, 0), 1));
    QVERIFY(oldToken);

    worker.endCaptureSession(oldSession);
    const uint64_t newSession = worker.beginCaptureSession();
    const auto newToken =
        worker.prepareCarrierTokenForFrameIngress(newSession, evidence(tcFrames(1, 0, 0, 0), 2));
    QVERIFY(newToken);
    QVERIFY(newToken->sessionIdentity != oldToken->sessionIdentity);
    QVERIFY(newToken->epoch != oldToken->epoch);

    QCOMPARE(worker.acquireEncodeSubmission(false, 0, nullptr, nullptr, *oldToken, 0), uint64_t(0));
    const uint64_t current =
        worker.acquireEncodeSubmission(false, 0, nullptr, nullptr, *newToken, 0);
    QVERIFY(current != 0);
    size_t index = 0;
    uint32_t generation = 0;
    QVERIFY(StreamWorker::decodePoolId(current, StreamWorker::kSubmissionPoolCapacity, &index,
                                       &generation));
    QCOMPARE(worker.m_submissionPool[index].carrierToken.sessionIdentity,
             newToken->sessionIdentity);
    QCOMPARE(worker.m_submissionPool[index].carrierToken.epoch, newToken->epoch);
    worker.finishEncodeSubmission(current);
}

void TestReplayManagerTimecode::callbackPoolGenerationRejectsAbaCompletion() {
    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const auto token = worker.snapshotActiveCarrierToken();
    QVERIFY(token);

    const uint64_t oldSubmission =
        worker.acquireEncodeSubmission(false, 0, nullptr, nullptr, *token, 0);
    QVERIFY(oldSubmission != 0);
    size_t submissionIndex = 0;
    uint32_t submissionGeneration = 0;
    QVERIFY(StreamWorker::decodePoolId(oldSubmission, StreamWorker::kSubmissionPoolCapacity,
                                       &submissionIndex, &submissionGeneration));
    worker.finishEncodeSubmission(oldSubmission);
    worker.m_nextSubmissionSlot = submissionIndex;
    const uint64_t newSubmission =
        worker.acquireEncodeSubmission(false, 0, nullptr, nullptr, *token, 0);
    QVERIFY(newSubmission != 0);
    QVERIFY(newSubmission != oldSubmission);
    worker.failEncodeSubmission(oldSubmission);
    QVERIFY(worker.m_submissionPool[submissionIndex].active);
    QCOMPARE(worker.m_submissionPool[submissionIndex].generation,
             uint32_t(submissionGeneration + 1));

    const uint64_t oldCompletion = worker.reserveMuxCompletion(newSubmission);
    QVERIFY(oldCompletion != 0);
    size_t completionIndex = 0;
    uint32_t completionGeneration = 0;
    QVERIFY(StreamWorker::decodePoolId(oldCompletion, StreamWorker::kMuxCompletionPoolCapacity,
                                       &completionIndex, &completionGeneration));
    worker.releaseMuxCompletionReservation(oldCompletion);
    worker.m_nextMuxCompletionSlot = completionIndex;
    const uint64_t newCompletion = worker.reserveMuxCompletion(newSubmission);
    QVERIFY(newCompletion != 0);
    QVERIFY(newCompletion != oldCompletion);
    worker.completeMuxWrite(oldCompletion, false);
    QVERIFY(worker.m_muxCompletionPool[completionIndex].active);
    QCOMPARE(worker.m_muxCompletionPool[completionIndex].generation,
             uint32_t(completionGeneration + 1));

    worker.releaseMuxCompletionReservation(newCompletion);
    worker.m_muxCompletionPool[completionIndex].generation = std::numeric_limits<uint32_t>::max();
    worker.m_nextMuxCompletionSlot = completionIndex;
    const uint64_t completionAfterExhaustion = worker.reserveMuxCompletion(newSubmission);
    QVERIFY(completionAfterExhaustion != 0);
    QVERIFY(completionAfterExhaustion != oldCompletion);
    size_t completionAfterExhaustionIndex = 0;
    uint32_t completionAfterExhaustionGeneration = 0;
    QVERIFY(StreamWorker::decodePoolId(
        completionAfterExhaustion, StreamWorker::kMuxCompletionPoolCapacity,
        &completionAfterExhaustionIndex, &completionAfterExhaustionGeneration));
    QVERIFY(completionAfterExhaustionIndex != completionIndex);
    worker.completeMuxWrite(oldCompletion, false);
    QVERIFY(worker.m_muxCompletionPool[completionAfterExhaustionIndex].active);
    worker.releaseMuxCompletionReservation(completionAfterExhaustion);

    worker.finishEncodeSubmission(newSubmission);
    worker.m_submissionPool[submissionIndex].generation = std::numeric_limits<uint32_t>::max();
    worker.m_nextSubmissionSlot = submissionIndex;
    const uint64_t submissionAfterExhaustion =
        worker.acquireEncodeSubmission(false, 0, nullptr, nullptr, *token, 0);
    QVERIFY(submissionAfterExhaustion != 0);
    QVERIFY(submissionAfterExhaustion != oldSubmission);
    size_t submissionAfterExhaustionIndex = 0;
    uint32_t submissionAfterExhaustionGeneration = 0;
    QVERIFY(StreamWorker::decodePoolId(
        submissionAfterExhaustion, StreamWorker::kSubmissionPoolCapacity,
        &submissionAfterExhaustionIndex, &submissionAfterExhaustionGeneration));
    QVERIFY(submissionAfterExhaustionIndex != submissionIndex);
    worker.failEncodeSubmission(oldSubmission);
    QVERIFY(worker.m_submissionPool[submissionAfterExhaustionIndex].active);
    worker.finishEncodeSubmission(submissionAfterExhaustion);
}

void TestReplayManagerTimecode::callbackDescriptorsAndSlotLifecycleAllocateNothing() {
    static_assert(std::is_trivially_copyable_v<NativeVideoEncoder::PacketCallback>);
    static_assert(std::is_trivially_copyable_v<Muxer::PacketWriteCallback>);
#if defined(OLR_GPU_PIPELINE_BUILD)
    static_assert(std::is_trivially_copyable_v<GpuEncodePump::JobCallbacks>);
#endif

    StreamWorker worker(QString(), 0, nullptr, nullptr, 64, 64, 30, 30, 1);
    const auto token = worker.snapshotActiveCarrierToken();
    QVERIFY(token);
    uint64_t checksum = 0;
    allocation_probe::count.store(0, std::memory_order_relaxed);
    allocation_probe::enabled.store(true, std::memory_order_release);
    for (int i = 0; i < 100'000; ++i) {
        const uint64_t submission =
            worker.acquireEncodeSubmission(false, 0, nullptr, nullptr, *token, 0);
        const uint64_t completion = worker.reserveMuxCompletion(submission);
        const auto packetCallback = worker.packetCallbackForSubmission(submission);
        const auto muxCallback = worker.muxCompletionCallback(completion);
#if defined(OLR_GPU_PIPELINE_BUILD)
        const auto gpuCallbacks = worker.gpuCallbacksForSubmission(submission);
        checksum |= gpuCallbacks.id;
#endif
        checksum |= packetCallback.id | muxCallback.id;
        worker.releaseMuxCompletionReservation(completion);
        worker.finishEncodeSubmission(submission);
    }
    allocation_probe::enabled.store(false, std::memory_order_release);

    QCOMPARE(allocation_probe::count.load(std::memory_order_relaxed), uint64_t(0));
    QVERIFY(checksum != 0);
}

void TestReplayManagerTimecode::nativeFailureAfterPacketDoesNotWritePartialOutput() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-native-partial-failure"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const uint64_t session = worker.beginCaptureSession();
    const TimecodeEvidence value = evidence(tcFrames(1, 0, 0, 0), 1, 30, 1, 7, 11);
    const auto token = worker.prepareCarrierTokenForFrameIngress(session, value);
    QVERIFY(token);
    worker.m_nativeEncoder = std::make_unique<PacketThenFailNativeEncoder>();
    worker.setViewTrack(0);
    worker.m_latestFrame = makeSoftwareFrame();
    QVERIFY(worker.m_latestFrame);
    worker.m_latestFrameCarrierToken = token;
    worker.m_latestFrameTimecodeEvidence = value;
    worker.m_latestFrameTimecode100ns.store(123, std::memory_order_release);

    worker.processEncoderTick(nullptr, 0, 0, 0);
    QTest::qWait(50);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));
    QVERIFY(worker.m_latestFrameTimecodeEvidence.has_value());

    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::nativeTwoPacketBatchRejectsBeforePartialCommit() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-native-two-packet-capacity"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));

    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const uint64_t session = worker.beginCaptureSession();
    const TimecodeEvidence value = evidence(tcFrames(1, 0, 0, 0), 1, 30, 1, 7, 11);
    const auto token = worker.prepareCarrierTokenForFrameIngress(session, value);
    QVERIFY(token);
    worker.m_nativeEncoder = std::make_unique<TwoPacketNativeEncoder>();
    worker.setViewTrack(0);
    worker.m_latestFrame = makeSoftwareFrame();
    QVERIFY(worker.m_latestFrame);
    worker.m_latestFrameCarrierToken = token;
    worker.m_latestFrameTimecodeEvidence = value;
    worker.m_latestFrameTimecode100ns.store(123, std::memory_order_release);

    std::array<uint64_t, StreamWorker::kMuxCompletionPoolCapacity - 1> heldCompletions{};
    for (uint64_t& id : heldCompletions) {
        id = worker.reserveMuxCompletion(0);
        QVERIFY(id != 0);
    }

    worker.processEncoderTick(nullptr, 0, 0, 0);
    QTest::qWait(100);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));

    for (uint64_t id : heldCompletions)
        worker.releaseMuxCompletionReservation(id);
    av_frame_free(&worker.m_latestFrame);
    muxer.close();
}

void TestReplayManagerTimecode::nativeMultiPtsBatchConsumesEveryDistinctEvidenceMapping() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-native-multi-pts"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));
    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const auto closeMuxer = qScopeGuard([&muxer] { muxer.close(); });
    const uint64_t session = worker.beginCaptureSession();
    const auto token = worker.snapshotCarrierTokenForSession(session);
    QVERIFY(token);
    const TimecodeEvidence previous = evidence(tcFrames(1, 0, 0, 0), 10, 30, 1, 7, 11);
    const TimecodeEvidence current = evidence(tcFrames(1, 0, 0, 1), 11, 30, 1, 7, 11);
    const auto previousMapping = worker.enqueueMuxFrameEvidence(10, 100, previous, token);
    const auto currentMapping = worker.enqueueMuxFrameEvidence(11, 200, current, token);
    QVERIFY(previousMapping.id != 0);
    QVERIFY(currentMapping.id != 0);
    bool havePacket = false;
    const uint64_t submission = worker.acquireEncodeSubmission(
        false, 0, muxer.getStream(0), &havePacket, *token, currentMapping.id);
    QVERIFY(submission != 0);

    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int, uint64_t, uint64_t, TimecodeEvidence value) { delivered.append(value); },
        Qt::QueuedConnection));
    worker.bufferEncodedPacket(submission, QByteArray::fromHex("000001b300100113"), 10, true);
    worker.bufferEncodedPacket(submission, QByteArray::fromHex("000001b300100114"), 11, true);
    worker.commitBufferedEncodeSubmission(submission);

    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 2, 2000);
    QCOMPARE(delivered[0].arrivalSessionFrame, int64_t(10));
    QCOMPARE(delivered[1].arrivalSessionFrame, int64_t(11));
    QCOMPARE(worker.m_muxFrameEvidence.size(), qsizetype(0));
    QVERIFY(havePacket);
}

void TestReplayManagerTimecode::nativeDuplicatePtsBatchSharesOneEvidenceMapping() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-native-duplicate-pts"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));
    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const auto closeMuxer = qScopeGuard([&muxer] { muxer.close(); });
    const uint64_t session = worker.beginCaptureSession();
    const auto token = worker.snapshotCarrierTokenForSession(session);
    QVERIFY(token);
    const TimecodeEvidence value = evidence(tcFrames(1, 0, 0, 2), 20, 30, 1, 7, 11);
    const auto mapping = worker.enqueueMuxFrameEvidence(20, 300, value, token);
    QVERIFY(mapping.id != 0);
    bool havePacket = false;
    const uint64_t submission = worker.acquireEncodeSubmission(false, 0, muxer.getStream(0),
                                                               &havePacket, *token, mapping.id);
    QVERIFY(submission != 0);

    QList<TimecodeEvidence> delivered;
    QVERIFY(QObject::connect(
        &worker, &StreamWorker::frameTimecode, this,
        [&delivered](int, uint64_t, uint64_t, TimecodeEvidence observed) {
            delivered.append(observed);
        },
        Qt::QueuedConnection));
    worker.bufferEncodedPacket(submission, QByteArray::fromHex("000001b300100113"), 20, true);
    worker.bufferEncodedPacket(submission, QByteArray::fromHex("000001b300100114"), 20, false);
    worker.commitBufferedEncodeSubmission(submission);

    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, 2000);
    QCOMPARE(delivered.front().arrivalSessionFrame, int64_t(20));
    QCOMPARE(worker.m_muxFrameEvidence.size(), qsizetype(0));
    QVERIFY(havePacket);
}

void TestReplayManagerTimecode::
    nativeMultiPtsBatchMissingEvidenceConsumesNothingAndCommitsNothing() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    Muxer muxer;
    muxer.setOutputDirectory(output.path());
    QVERIFY(muxer.init(QStringLiteral("timecode-native-missing-batch-evidence"), 1, 64, 64, 30,
                       {QStringLiteral("Program")}, 48000, 2));
    StreamWorker worker(QString(), 0, &muxer, nullptr, 64, 64, 30, 30, 1,
                        VideoCodecChoice::H264Hardware);
    const auto closeMuxer = qScopeGuard([&muxer] { muxer.close(); });
    const uint64_t session = worker.beginCaptureSession();
    const auto token = worker.snapshotCarrierTokenForSession(session);
    QVERIFY(token);
    const TimecodeEvidence value = evidence(tcFrames(1, 0, 0, 3), 30, 30, 1, 7, 11);
    const auto mapping = worker.enqueueMuxFrameEvidence(30, 400, value, token);
    QVERIFY(mapping.id != 0);
    bool havePacket = false;
    const uint64_t submission = worker.acquireEncodeSubmission(false, 0, muxer.getStream(0),
                                                               &havePacket, *token, mapping.id);
    QVERIFY(submission != 0);

    worker.bufferEncodedPacket(submission, QByteArray::fromHex("000001b300100113"), 30, true);
    worker.bufferEncodedPacket(submission, QByteArray::fromHex("000001b300100114"), 31, true);
    worker.commitBufferedEncodeSubmission(submission);

    QCOMPARE(worker.m_muxFrameEvidence.size(), qsizetype(1));
    const auto preserved = worker.takeMuxFrameEvidence(30, *token);
    QVERIFY(preserved.has_value());
    QCOMPARE(preserved->sourceTimecode100ns, int64_t(400));
    QVERIFY(!havePacket);
    QTest::qWait(100);
    QCOMPARE(muxer.minWrittenVideoPtsMs(), int64_t(-1));
}

void TestReplayManagerTimecode::boundedDriftReportsNonzeroUiBound() {
    ReplayManager manager;
    QVERIFY(feedEvidence(manager, 0, evidence(tcFrames(1, 0, 0, 0), 100)));
    QVERIFY(feedEvidence(manager, 1, evidence(tcFrames(1, 30, 0, 0), 54'103)));
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0, 10.0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 0, -10.0)));
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::Bounded);
    QCOMPARE(manager.sourcePhaseOffsetMs(1), int64_t(-100));
    QVERIFY(manager.sourcePhaseBoundMs(1) >= 45);
}

void TestReplayManagerTimecode::overConfidenceTimecodeDoesNotMoveServo() {
    ReplayManager manager;
    for (int i = 0; i < 20; ++i) {
        QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 40'000'000)));
    }
    QCOMPARE(manager.sourceServoTrimMs(1), -40);

    constexpr int64_t kPerSourceBoundUs = 60'001;
    QVERIFY(feedEvidence(manager, 0,
                         evidence(tcFrames(1, 0, 0, 0), 100, 30, 1, 1, 1, kPerSourceBoundUs)));
    QVERIFY(feedEvidence(manager, 1,
                         evidence(tcFrames(1, 0, 0, 0), 102, 30, 1, 1, 1, kPerSourceBoundUs)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 4'000'000)));
    QVERIFY(manager.sourcePhaseBoundMs(1) >
            int((ReplayManager::kMaxTimecodeCorrectionBoundUs + 999) / 1000));
    QCOMPARE(manager.sourceServoTrimMs(1), -40 + ReplayManager::kServoStepMs);
    for (int i = 0; i < 20; ++i)
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 4'000'000)));
    QCOMPARE(manager.sourceServoTrimMs(1), 0);
}

void TestReplayManagerTimecode::disconnectClearsTimecodeAnchor() {
    ReplayManager manager;
    QVERIFY(feedFrameTimecode(manager, 0, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(manager.sourcesFrameAligned(0, 1));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onSourcePhaseConnectionChanged",
                                      Qt::DirectConnection, Q_ARG(int, 1), Q_ARG(bool, false)));
    QVERIFY(!manager.sourcesFrameAligned(0, 1));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onSourcePhaseConnectionChanged",
                                      Qt::DirectConnection, Q_ARG(int, 1), Q_ARG(bool, true)));
    QVERIFY(feedFrameTimecode(manager, 1, tcFrames(1, 0, 0, 0), 103));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(-3));
}

void TestReplayManagerTimecode::urlReplacementClearsTimecodeAnchor() {
    ReplayManager manager;
    manager.setSourceUrls({QStringLiteral("ndi://a"), QStringLiteral("ndi://b")});
    QVERIFY(feedFrameTimecode(manager, 0, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(manager.sourcesFrameAligned(0, 1));
    manager.updateSourceUrl(1, QStringLiteral("ndi://replacement"));
    QVERIFY(!manager.sourcesFrameAligned(0, 1));
}

void TestReplayManagerTimecode::generationAndRateChangesReanchor() {
    ReplayManager manager;
    QVERIFY(feedFrameTimecode(manager, 0, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(manager.sourcesFrameAligned(0, 1));

    QVERIFY(feedEvidence(manager, 1, evidence(tcFrames(1, 0, 0, 0), 103, 30, 1, 2, 1)));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(-3));

    QVERIFY(feedEvidence(manager, 1, evidence(tcFrames(1, 0, 0, 0) * 2, 100, 60, 1, 2, 1)));
    QVERIFY(manager.sourcesFrameAligned(0, 1));

    QVERIFY(feedEvidence(manager, 1, evidence(tcFrames(1, 0, 0, 0) * 2, 104, 60, 1, 2, 2)));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(-4));
}

void TestReplayManagerTimecode::sourceFrameOffsetRoundsSymmetricallyAtHalfFrame() {
    const int64_t base30 = tcFrames(1, 0, 0, 0);
    const int64_t base60 = base30 * 2;

    ReplayManager positive;
    QVERIFY(feedEvidence(positive, 0, evidence(base30, 100, 30, 1)));
    QVERIFY(feedEvidence(positive, 1, evidence(base60 + 1, 100, 60, 1)));
    QCOMPARE(positive.sourceFrameOffset(0, 1), int64_t(1));

    ReplayManager negative;
    QVERIFY(feedEvidence(negative, 0, evidence(base30, 100, 30, 1)));
    QVERIFY(feedEvidence(negative, 1, evidence(base60 - 1, 100, 60, 1)));
    QCOMPARE(negative.sourceFrameOffset(0, 1), int64_t(-1));
}

void TestReplayManagerTimecode::discontinuityRequiresFreshAnchor() {
    ReplayManager manager;
    const int64_t baseTc = tcFrames(1, 0, 0, 0);
    QVERIFY(feedFrameTimecode(manager, 0, baseTc, 100));
    QVERIFY(feedFrameTimecode(manager, 1, baseTc, 100));
    QVERIFY(manager.sourcesFrameAligned(0, 1));

    TimecodeEvidence discontinuity = evidence(baseTc + 1, 101);
    discontinuity.discontinuity = true;
    QVERIFY(feedEvidence(manager, 1, discontinuity));
    QVERIFY(!manager.sourcesFrameAligned(0, 1));

    QVERIFY(feedEvidence(manager, 1, evidence(baseTc + 1, 101)));
    QVERIFY(manager.sourcesFrameAligned(0, 1));
}

void TestReplayManagerTimecode::legalRolloverSurvivesTypedPipeline() {
    ReplayManager manager;
    constexpr int64_t kLastFrameOfDay = 30LL * 24 * 60 * 60 - 1;
    QVERIFY(feedEvidence(manager, 0, evidence(kLastFrameOfDay - 1, 100)));
    QVERIFY(feedEvidence(manager, 1, evidence(kLastFrameOfDay - 1, 100)));
    QVERIFY(feedEvidence(manager, 0, evidence(0, 102)));
    QVERIFY(feedEvidence(manager, 1, evidence(0, 102)));
    QVERIFY(manager.sourcesFrameAligned(0, 1));
}

void TestReplayManagerTimecode::referenceIsHighestClockQualityTieLowestIndex() {
    ReplayManager manager;
    // Source 0: NDI-locked. Source 1: PCR-locked (higher quality). Source 2: PCR-locked
    // (ties source 1 on quality). The reference must be the highest ClockQuality, and
    // a quality tie breaks to the LOWEST index → source 1.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Ndi, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 1000000)));
    QVERIFY(feedStats(manager, 2, clockStats(ClockQuality::Pcr, true, 2000000)));
    QCOMPARE(manager.referenceSource(), 1);
}

void TestReplayManagerTimecode::timecodeAlignedSourceGradesFrameAccurate() {
    ReplayManager manager;
    // Both PCR-locked; source 1 is the reference (lowest index would be 0 on a tie,
    // so give source 0 the higher quality to make it the reference). A jam-synced TC
    // pair (equal TC lands on the same session frame) makes source 1 TC-aligned to the
    // reference → FrameAccurate.
    QVERIFY(feedFrameTimecode(manager, 0, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 5000000)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::FrameAccurate);
    QCOMPARE(manager.sourcePhaseBoundMs(1), 0);
    QCOMPARE(manager.sourceTier(0), ConfidenceTier::FrameAccurate); // reference vs itself
}

void TestReplayManagerTimecode::lockedNoTimecodeSourceGradesBoundedWithOffset() {
    ReplayManager manager;
    // Both PCR-locked, NO timecode → Bounded. measuredOffsetMs is the clockOffsetNs
    // difference (source - reference) / 1e6. Reference is source 0 (tie → lowest index).
    // Source 1 leads the reference by 7 ms (7e6 ns).
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 7000000)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::Bounded);
    QCOMPARE(manager.sourcePhaseOffsetMs(1), int64_t(7));
    QVERIFY(manager.sourcePhaseBoundMs(1) >= 4); // at least the base bound
}

void TestReplayManagerTimecode::arrivalOnlySourceGradesApproximate() {
    ReplayManager manager;
    // Source 0 is PCR-locked (reference). Source 1 is arrival-only, unlocked → Approximate.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Arrival, false, 0)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::Approximate);
    QCOMPARE(manager.sourcePhaseBoundMs(1), 40);
}

void TestReplayManagerTimecode::referenceSourceHasZeroPhase() {
    ReplayManager manager;
    // The reference's own offset is 0 by construction (it is differenced against itself),
    // even though its absolute clockOffsetNs is non-zero.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 9000000)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Ndi, true, 1000000)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourcePhaseOffsetMs(0), int64_t(0));
}

void TestReplayManagerTimecode::disconnectReselectsReferenceAwayFromDeadSource() {
    ReplayManager manager;
    // Source 0: NDI-locked. Source 1: PCR-locked (higher quality) → reference is 1.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Ndi, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 1000000)));
    QCOMPARE(manager.referenceSource(), 1);
    // Source 1 disconnects: it must drop from eligibility so the reference re-selects
    // to the still-live source 0 (the servo must never correct toward a dead reference).
    QVERIFY(QMetaObject::invokeMethod(&manager, "onSourcePhaseConnectionChanged",
                                      Qt::DirectConnection, Q_ARG(int, 1), Q_ARG(bool, false)));
    QCOMPARE(manager.referenceSource(), 0);
}

void TestReplayManagerTimecode::allSourcesDisconnectedClearServoBeforeReconnect() {
    ReplayManager manager;
    for (int i = 0; i < 20; ++i) {
        QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 40'000'000)));
    }
    QCOMPARE(manager.sourceServoTrimMs(1), -40);

    QVERIFY(QMetaObject::invokeMethod(&manager, "onSourcePhaseConnectionChanged",
                                      Qt::DirectConnection, Q_ARG(int, 1), Q_ARG(bool, false)));
    QVERIFY(manager.sourceServoTrimMs(1) < 0);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onSourcePhaseConnectionChanged",
                                      Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(bool, false)));

    QCOMPARE(manager.referenceSource(), -1);
    QCOMPARE(manager.sourceServoTrimMs(0), 0);
    QCOMPARE(manager.sourceServoTrimMs(1), 0);

    QVERIFY(QMetaObject::invokeMethod(&manager, "onSourcePhaseConnectionChanged",
                                      Qt::DirectConnection, Q_ARG(int, 1), Q_ARG(bool, true)));
    QCOMPARE(manager.sourceServoTrimMs(1), 0);
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 40'000'000)));
    QCOMPARE(manager.referenceSource(), 1);
    QCOMPARE(manager.sourceServoTrimMs(1), 0);
}

void TestReplayManagerTimecode::servoSignPullsLateSourceEarlier() {
    ReplayManager manager;
    // Reference = source 0 (tie -> lowest index). Source 1 is PCR-locked, NO timecode
    // -> Bounded, and LATE by +40 ms (clockOffsetNs greater than the reference). The
    // servo must pull a LATE (positive-phase) source EARLIER, i.e. a NEGATIVE servo trim
    // (a negative trim makes targetTimeMs larger -> newer frames -> advances the source).
    // Pulse repeatedly so the gentle ramp converges past the per-step cap.
    for (int i = 0; i < 40; ++i) {
        QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 40000000)));
    }
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::Bounded);
    QCOMPARE(manager.sourcePhaseOffsetMs(1), int64_t(40));
    // Converged: servo pulls toward -phase = -40 ms (within the cap).
    QCOMPARE(manager.sourceServoTrimMs(1), -40);
}

void TestReplayManagerTimecode::servoRampsTowardTargetNotFullJump() {
    ReplayManager manager;
    // A single large phase reading must NOT jump the servo straight to the target -
    // the ramp absorbs a Bounded signal that steps on re-anchor. After ONE pulse the
    // servo has moved by at most one ramp step, strictly less than the full -phase.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 40000000))); // +40 ms phase
    QCOMPARE(manager.referenceSource(), 0);
    const int servo1 = manager.sourceServoTrimMs(1);
    QVERIFY2(servo1 < 0, "ramps in the correcting (negative) direction");
    QVERIFY2(servo1 > -40, "does NOT jump the full -40 ms in one update");
    // A second pulse ramps further toward the target (monotone, still not overshooting).
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 40000000)));
    const int servo2 = manager.sourceServoTrimMs(1);
    QVERIFY2(servo2 < servo1, "second pulse ramps further toward the target");
    QVERIFY2(servo2 >= -40, "never overshoots the target");
}

void TestReplayManagerTimecode::servoNeverExceedsCap() {
    ReplayManager manager;
    // A huge measured phase (+5000 ms) must saturate at the cap, never beyond it,
    // no matter how many pulses ramp it. The cap protects the timeline.
    for (int i = 0; i < 500; ++i) {
        QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 5000000000LL)));
    }
    QCOMPARE(manager.referenceSource(), 0);
    const int servo = manager.sourceServoTrimMs(1);
    QVERIFY2(servo <= 0, "correcting direction for a late source");
    QVERIFY2(servo >= -ReplayManager::kMaxInterCamCorrectionMs, "never exceeds the negative cap");
    QCOMPARE(servo, -ReplayManager::kMaxInterCamCorrectionMs); // saturated AT the cap
}

void TestReplayManagerTimecode::referenceAndApproximateGetNoServo() {
    ReplayManager manager;
    // Source 0 PCR-locked (reference). Source 1 arrival-only/unlocked -> Approximate:
    // there is nothing reliable to lock to, so it gets ZERO servo even with a phase
    // reading. The reference itself always gets zero servo.
    for (int i = 0; i < 40; ++i) {
        QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Arrival, false, 40000000)));
    }
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::Approximate);
    QCOMPARE(manager.sourceServoTrimMs(0), 0); // reference: never servoed
    QCOMPARE(manager.sourceServoTrimMs(1), 0); // Approximate: no reliable lock
}

void TestReplayManagerTimecode::ineligibleClockRelaxesExistingServoTrim() {
    ReplayManager manager;
    for (int i = 0; i < 20; ++i) {
        QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 40'000'000)));
    }
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceServoTrimMs(1), -40);

    // Losing trustworthy clock evidence makes the correction target zero. The
    // existing trim must still pass through the common bounded relaxation ramp.
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Arrival, false, 40'000'000)));
    QCOMPARE(manager.sourceServoTrimMs(1), -40 + ReplayManager::kServoStepMs);
    for (int i = 0; i < 20; ++i)
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Arrival, false, 40'000'000)));
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::Approximate);
    QCOMPARE(manager.sourceServoTrimMs(1), 0);
}

void TestReplayManagerTimecode::singleSourceServoIsZero() {
    ReplayManager manager;
    // A lone source is its own reference -> zero phase -> zero servo (byte-identical
    // to today; the additive servo only ever touches Bounded/FrameAccurate followers).
    for (int i = 0; i < 10; ++i)
        QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 12345678)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceServoTrimMs(0), 0);
}

void TestReplayManagerTimecode::servoUsesExactTcOffsetWhenCommonTimecode() {
    ReplayManager manager;
    // Both sources carry COMMON timecode; source 1's equal-TC frame lands 2 session
    // frames LATE (102 vs reference's 100) -> frameOffset(0,1) = -2 frames = -66 ms @30.
    // Its CLOCK offset implies only -4 ms. The servo must lock to the EXACT TC offset
    // (drive toward -66, capped at -80), NOT ride the coarse clock signal (-4). Ramp
    // many pulses: a clock-driven servo would stall at -4; a TC-driven one keeps going.
    QVERIFY(feedFrameTimecode(manager, 0, tcFrames(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tcFrames(1, 0, 0, 0), 102));
    for (int i = 0; i < 40; ++i) {
        QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
        QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 4000000))); // clock +4ms
    }
    QCOMPARE(manager.referenceSource(), 0);
    // TC-driven target = frameOffset(0,1)*1000/30 = -2*33 = -66 ms; clock-driven would
    // be only -4. Converged well past the clock signal proves the exact-TC path is used.
    QCOMPARE(manager.sourceServoTrimMs(1), -66);
}

void TestReplayManagerTimecode::relayedStatsCarryTierPhaseAndReference() {
    ReplayManager manager;
    // Spy on the UI-facing relay: the estimator state must ride OUT on the same
    // sourceStatsUpdated signal (no new signal), so the tooltip sees it for free.
    QSignalSpy spy(&manager, &ReplayManager::sourceStatsUpdated);
    QVERIFY(spy.isValid());

    // Reference = source 0 (PCR). Source 1 PCR-locked, NO timecode -> Bounded, LATE +7 ms.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 7000000)));
    QCOMPARE(manager.referenceSource(), 0);

    // The LAST emission is source 1 (the second feedStats). Its relayed stats must be
    // stamped with the estimator's grading: Bounded(=1), phase +7 ms, a >=4 ms bound,
    // and isReference=false (it is not the reference).
    QVERIFY(spy.size() >= 2);
    const QList<QVariant> last = spy.last();
    QCOMPARE(last.at(0).toInt(), 1);
    const IngestStats relayed = last.at(1).value<IngestStats>();
    QCOMPARE(relayed.confidenceTier, int(ConfidenceTier::Bounded));
    QCOMPARE(relayed.interCamPhaseMs, int64_t(7));
    QVERIFY(relayed.interCamBoundMs >= 4);
    QCOMPARE(relayed.isReference, false);

    // The reference's own relayed stats: phase 0, isReference true.
    QSignalSpy spy2(&manager, &ReplayManager::sourceStatsUpdated);
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    const IngestStats refRelayed = spy2.last().at(1).value<IngestStats>();
    QCOMPARE(refRelayed.isReference, true);
    QCOMPARE(refRelayed.interCamPhaseMs, int64_t(0));
}

void TestReplayManagerTimecode::relayedStatsLeavePreExistingFieldsUntouched() {
    ReplayManager manager;
    QSignalSpy spy(&manager, &ReplayManager::sourceStatsUpdated);

    // A fully-populated snapshot: every pre-existing field must survive the relay
    // byte-identical; only the additive estimator fields are stamped.
    IngestStats in;
    in.kind = IngestStatsKind::Srt;
    in.recvTotal = 111;
    in.retransTotal = 22;
    in.lossTotal = 3;
    in.dropTotal = 4;
    in.bytesTotal = 5555;
    in.lastPacketAgeMs = 66;
    in.keyframeAgeMs = 77;
    in.decodeFailures = 8;
    in.clockPpm = 12.5;
    in.clockQuality = int(ClockQuality::Pcr);
    in.clockLocked = true;
    in.clockOffsetNs = 9000000;
    QVERIFY(feedStats(manager, 0, in));

    const IngestStats out = spy.last().at(1).value<IngestStats>();
    QCOMPARE(out.kind, in.kind);
    QCOMPARE(out.recvTotal, in.recvTotal);
    QCOMPARE(out.retransTotal, in.retransTotal);
    QCOMPARE(out.lossTotal, in.lossTotal);
    QCOMPARE(out.dropTotal, in.dropTotal);
    QCOMPARE(out.bytesTotal, in.bytesTotal);
    QCOMPARE(out.lastPacketAgeMs, in.lastPacketAgeMs);
    QCOMPARE(out.keyframeAgeMs, in.keyframeAgeMs);
    QCOMPARE(out.decodeFailures, in.decodeFailures);
    QCOMPARE(out.clockPpm, in.clockPpm);
    QCOMPARE(out.clockQuality, in.clockQuality);
    QCOMPARE(out.clockLocked, in.clockLocked);
    QCOMPARE(out.clockOffsetNs, in.clockOffsetNs);
}

void TestReplayManagerTimecode::referenceTierDefaultsToLocalMonotonic() {
    ReplayManager manager;
    // No recording started → no TimingReference is constructed yet. The UI-facing
    // accessors must still report the safe default: the local monotonic tier (0) and
    // not-external, so the session status surface reads "local monotonic" until a
    // recording opens (and, with the default LocalMonotonicReference, stays there).
    QCOMPARE(manager.referenceTier(), int(ReferenceTier::LocalMonotonic));
    QCOMPARE(manager.referenceTier(), 0);
    QVERIFY(!manager.referenceIsExternal());
}

QTEST_GUILESS_MAIN(TestReplayManagerTimecode)
#include "tst_replaymanager_timecode.moc"

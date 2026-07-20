#include "streamworker.h"
#include "ingest/ingestsession.h"
#include "ingest/rtmpprotocol.h"
#if defined(OLR_NATIVE_SRT_AVAILABLE)
#include "ingest/nativesrtingestsession.h"
#endif
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
#include "ingest/nativertmpingestsession.h"
#endif
#include "ingest/nativendiingestsession.h"
#include "timing/smpte12m.h"
#if defined(OLR_GPU_PIPELINE_BUILD)
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "recorder_engine/codec/gpuencodepump.h"
#if defined(__APPLE__)
#include "playback/gpu/appleiosurface.h"
#endif
#if defined(_WIN32)
#include "playback/output/win/d3d11gpusurface.h"
#include "playback/output/win/wingpuimportedge.h"
#endif
#endif
#include <QDebug>
#include <QDateTime>
#include <QScopeGuard>
#include <QUrl>
#include <QtGlobal>

#include <atomic>
#include <array>
#include <limits>
#include <memory>
#include <utility>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace {
uint64_t nextStreamWorkerInstanceIdentity() {
    static std::atomic<uint64_t> nextIdentity{1};
    const uint64_t identity = nextIdentity.fetch_add(1, std::memory_order_relaxed);
    if (identity == 0) qFatal("StreamWorker instance identity exhausted");
    return identity;
}

bool sameTimecodeIdentity(const TimecodeEvidence& a, const TimecodeEvidence& b) {
    return a.labelRate == b.labelRate && a.sourceGeneration == b.sourceGeneration &&
           a.timingGeneration == b.timingGeneration && a.provenance == b.provenance &&
           a.dropFrame == b.dropFrame && a.sessionRate == b.sessionRate;
}

#if defined(OLR_GPU_PIPELINE_BUILD)
bool sameTimecodeEvidence(const TimecodeEvidence& a, const TimecodeEvidence& b) {
    return a.frameOfDay == b.frameOfDay && a.labelRate == b.labelRate &&
           a.sourceGeneration == b.sourceGeneration && a.timingGeneration == b.timingGeneration &&
           a.provenance == b.provenance && a.dropFrame == b.dropFrame &&
           a.discontinuity == b.discontinuity && a.arrivalSessionFrame == b.arrivalSessionFrame &&
           a.sessionRate == b.sessionRate && a.quantizationBoundUs == b.quantizationBoundUs &&
           a.driftBoundUs == b.driftBoundUs;
}
#endif

QString ingestFailureKindForLog(IngestFailureKind failure) {
    switch (failure) {
    case IngestFailureKind::UnsupportedProfile:
        return QStringLiteral("unsupported profile");
    case IngestFailureKind::DecodeCapability:
        return QStringLiteral("decode capability failure");
    case IngestFailureKind::MalformedStream:
        return QStringLiteral("malformed stream");
    case IngestFailureKind::TransientNetwork:
        return QStringLiteral("transient network failure");
    case IngestFailureKind::None:
        break;
    }
    return QStringLiteral("unknown failure");
}
} // namespace

StreamWorker::StreamWorker(const QString& url, int sourceIndex, Muxer* muxer, RecordingClock* clock,
                           int targetWidth, int targetHeight, int targetFps, int targetFpsNum,
                           int targetFpsDen, VideoCodecChoice codec, QObject* parent)
    : QThread(parent), m_url(url), m_sourceIndex(sourceIndex),
      m_workerInstanceIdentity(nextStreamWorkerInstanceIdentity()), m_viewTrack(-1), m_muxer(muxer),
      m_sharedClock(clock) {
    m_videoCodec = codec;
    qRegisterMetaType<IngestStats>("IngestStats");
    qRegisterMetaType<TimecodeEvidence>("TimecodeEvidence");
    m_restartCapture = 0;
    m_internalFrameCount = 0;
    m_submissionPool = std::make_unique<EncodeSubmissionSlot[]>(kSubmissionPoolCapacity);
    m_muxCompletionPool = std::make_unique<MuxCompletionSlot[]>(kMuxCompletionPoolCapacity);
    m_activeCarrierToken = std::make_shared<const SourceCarrierToken>(SourceCarrierToken{0, 1});
    m_monotonic.start();
    if (targetWidth > 0) m_targetWidth = targetWidth;
    if (targetHeight > 0) m_targetHeight = targetHeight;
    if (targetFps > 0) m_targetFps = targetFps;
    // Advertised rational rate: explicit num/den, else the integer {targetFps, 1}.
    if (targetFpsNum > 0 && targetFpsDen > 0) {
        m_targetFpsNum = targetFpsNum;
        m_targetFpsDen = targetFpsDen;
    } else {
        m_targetFpsNum = m_targetFps;
        m_targetFpsDen = 1;
    }
}

StreamWorker::~StreamWorker() {
    stop();
    wait();
}

void StreamWorker::setConnected(bool c) {
    setConnectedImpl(c, 0);
}

void StreamWorker::setConnectedForSession(uint64_t sessionIdentity, bool connected) {
    setConnectedImpl(connected, sessionIdentity);
}

void StreamWorker::setConnectedImpl(bool c, uint64_t requiredSessionIdentity) {
    bool scheduleDrain = false;
    {
        std::lock_guard<std::mutex> lock(m_connectionTransitionMutex);
        std::unique_lock<std::mutex> epochLock(m_epochMutex);
        if (requiredSessionIdentity != 0 &&
            requiredSessionIdentity != m_activeCaptureSessionIdentity) {
            return;
        }
        const bool prev = m_connected.load(std::memory_order_relaxed);
        if (prev == c) return;
        if (!c) rotateCarrierLocked(false);
        m_connected.store(c, std::memory_order_release);
        m_pendingConnectionEmissions.push_back(c);
        if (!m_connectionDrainScheduled) {
            m_connectionDrainScheduled = true;
            scheduleDrain = true;
        }
    }
    if (scheduleDrain)
        QMetaObject::invokeMethod(
            this, [this] { drainConnectionEmissions(); }, Qt::QueuedConnection);
}

void StreamWorker::reportStatsForSession(uint64_t sessionIdentity, const IngestStats& stats) {
    {
        std::lock_guard<std::mutex> epochLock(m_epochMutex);
        if (sessionIdentity == 0 || sessionIdentity != m_activeCaptureSessionIdentity) return;
    }
    emit statsUpdated(m_sourceIndex, stats);
}

void StreamWorker::drainConnectionEmissions() {
    for (;;) {
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(m_connectionTransitionMutex);
            if (m_pendingConnectionEmissions.empty()) {
                m_connectionDrainScheduled = false;
                return;
            }
            connected = m_pendingConnectionEmissions.front();
            m_pendingConnectionEmissions.pop_front();
        }
        emit connectionChanged(m_sourceIndex, connected);
    }
}

uint64_t StreamWorker::beginCaptureSession() {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    uint64_t nextIdentity = m_nextCaptureSessionIdentity + 1;
    if (nextIdentity == 0) nextIdentity = 1;
    uint64_t epoch = m_carrierEpoch.load(std::memory_order_relaxed) + 1;
    if (epoch == 0) epoch = 1;
    auto token =
        std::make_shared<const SourceCarrierToken>(SourceCarrierToken{nextIdentity, epoch});
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    resetMuxFrameEvidenceLocked();
    m_nextCaptureSessionIdentity = nextIdentity;
    m_activeCaptureSessionIdentity = nextIdentity;
    m_carrierEpoch.store(epoch, std::memory_order_release);
    m_activeCarrierToken = std::move(token);
    return nextIdentity;
}

void StreamWorker::endCaptureSession(uint64_t sessionIdentity) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    if (m_activeCaptureSessionIdentity != sessionIdentity) return;
    uint64_t epoch = m_carrierEpoch.load(std::memory_order_relaxed) + 1;
    if (epoch == 0) epoch = 1;
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    resetMuxFrameEvidenceLocked();
    m_activeCaptureSessionIdentity = 0;
    m_activeCarrierToken = std::make_shared<const SourceCarrierToken>(SourceCarrierToken{0, epoch});
    m_carrierEpoch.store(epoch, std::memory_order_release);
}

std::shared_ptr<const StreamWorker::SourceCarrierToken>
StreamWorker::snapshotActiveCarrierToken() const {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    return m_activeCarrierToken;
}

std::shared_ptr<const StreamWorker::SourceCarrierToken>
StreamWorker::snapshotCarrierTokenForSession(uint64_t sessionIdentity) const {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    if (sessionIdentity == 0 || sessionIdentity != m_activeCaptureSessionIdentity) return {};
    return m_activeCarrierToken;
}

void StreamWorker::rotateCarrier(bool retainActiveSession) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    rotateCarrierLocked(retainActiveSession);
}

void StreamWorker::rotateCarrierLocked(bool retainActiveSession) {
    uint64_t epoch = m_carrierEpoch.load(std::memory_order_relaxed) + 1;
    if (epoch == 0) epoch = 1;
    const uint64_t sessionIdentity = retainActiveSession ? m_activeCaptureSessionIdentity : 0;
    auto token =
        std::make_shared<const SourceCarrierToken>(SourceCarrierToken{sessionIdentity, epoch});
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    resetMuxFrameEvidenceLocked();
    if (!retainActiveSession) m_activeCaptureSessionIdentity = 0;
    m_carrierEpoch.store(epoch, std::memory_order_release);
    m_activeCarrierToken = std::move(token);
}

std::shared_ptr<const StreamWorker::SourceCarrierToken>
StreamWorker::prepareCarrierTokenForFrameIngress(uint64_t sessionIdentity,
                                                 const std::optional<TimecodeEvidence>& evidence) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    return prepareCarrierTokenForFrameIngressLocked(sessionIdentity, evidence);
}

std::shared_ptr<const StreamWorker::SourceCarrierToken>
StreamWorker::prepareCarrierTokenForFrameIngressLocked(
    uint64_t sessionIdentity, const std::optional<TimecodeEvidence>& evidence) {
    if (sessionIdentity == 0 || sessionIdentity != m_activeCaptureSessionIdentity ||
        !m_activeCarrierToken) {
        return {};
    }

    const bool identityBoundary =
        evidence && (evidence->discontinuity ||
                     (m_muxFrameEvidenceIdentity &&
                      !sameTimecodeIdentity(*m_muxFrameEvidenceIdentity, *evidence)));
    if (identityBoundary) {
        uint64_t epoch = m_carrierEpoch.load(std::memory_order_relaxed) + 1;
        if (epoch == 0) epoch = 1;
        auto token = std::make_shared<const SourceCarrierToken>(
            SourceCarrierToken{m_activeCaptureSessionIdentity, epoch});
        resetMuxFrameEvidenceLocked();
        m_carrierEpoch.store(epoch, std::memory_order_release);
        m_activeCarrierToken = std::move(token);
    }
    if (evidence) m_muxFrameEvidenceIdentity = *evidence;
    return m_activeCarrierToken;
}

bool StreamWorker::carrierTokenIsCurrent(
    const std::shared_ptr<const SourceCarrierToken>& token) const {
    return token && carrierTokenIsCurrent(*token);
}

bool StreamWorker::carrierTokenIsCurrent(const SourceCarrierToken& token) const {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    return carrierTokenIsCurrentLocked(token);
}

bool StreamWorker::carrierTokenIsCurrentLocked(const SourceCarrierToken& token) const {
    return m_activeCarrierToken && token.epoch != 0 &&
           token.sessionIdentity == m_activeCaptureSessionIdentity &&
           token.sessionIdentity == m_activeCarrierToken->sessionIdentity &&
           token.epoch == m_activeCarrierToken->epoch &&
           token.epoch == m_carrierEpoch.load(std::memory_order_relaxed);
}

bool StreamWorker::packetCarrierGuardThunk(void* context, uint64_t sessionIdentity,
                                           uint64_t epoch) {
    return static_cast<StreamWorker*>(context)->carrierTokenIsCurrent(
        SourceCarrierToken{sessionIdentity, epoch});
}

Muxer::PacketCarrierGuard
StreamWorker::packetCarrierGuard(const SourceCarrierToken& token) noexcept {
    return Muxer::PacketCarrierGuard{this, token.sessionIdentity, token.epoch,
                                     &StreamWorker::packetCarrierGuardThunk};
}

qint64 StreamWorker::queuedFrameBytes(const QueuedFrame& frame) {
    qint64 bytes = 0;
    if (frame.frame && frame.frame->width > 0 && frame.frame->height > 0 &&
        frame.frame->format >= 0) {
        const int frameBytes =
            av_image_get_buffer_size(static_cast<AVPixelFormat>(frame.frame->format),
                                     frame.frame->width, frame.frame->height, 1);
        if (frameBytes > 0) bytes += frameBytes;
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    if (!frame.gpuFrame.isNull()) {
        const FramePayloadKey& key = frame.gpuFrame.metadata().key;
        const qint64 pixels = qint64(qMax(0, key.width)) * qMax(0, key.height);
        switch (key.format) {
        case FramePixelFormat::Rgba8:
            bytes += pixels * 4;
            break;
        case FramePixelFormat::Nv12:
        case FramePixelFormat::Yuv420p:
        default:
            bytes += pixels * 3 / 2;
            break;
        }
    }
#endif
    return bytes;
}

void StreamWorker::enqueueDecodedVideoFrame(DecodedVideoFrame decoded) {
    const auto active = snapshotActiveCarrierToken();
    if (!active || active->sessionIdentity == 0) {
        if (decoded.frame) av_frame_free(&decoded.frame);
        return;
    }
    enqueueDecodedVideoFrameForSession(std::move(decoded), active->sessionIdentity);
}

void StreamWorker::enqueueDecodedVideoFrameForSession(DecodedVideoFrame decoded,
                                                      uint64_t sessionIdentity) {
#if defined(OLR_GPU_PIPELINE_BUILD)
    if (!decoded.frame && decoded.gpuFrame.isNull()) return;
#else
    if (!decoded.frame) return;
#endif

    if (m_suppressEnqueue.load(std::memory_order_relaxed)) {
        if (decoded.frame) av_frame_free(&decoded.frame);
        return;
    }

    QueuedFrame qf;
    qf.frame = decoded.frame;
    qf.sourcePts = decoded.sourcePtsMs;
    qf.sourceTimecode100ns = decoded.sourceTimecode100ns;
    qf.timecodeEvidence = std::move(decoded.timecodeEvidence);
#if defined(OLR_GPU_PIPELINE_BUILD)
    qf.gpuFrame = std::move(decoded.gpuFrame);
    qf.gpuFenceValue = decoded.gpuFenceValue;
    const uint64_t gpuCarrierSessionIdentity = decoded.gpuCarrierSessionIdentity;
    const uint64_t gpuCarrierEpoch = decoded.gpuCarrierEpoch;
#endif

    // Carrier identity rotation and frame admission are one ordered ingress
    // transaction. Reset paths take the same epoch->evidence order, so an old
    // callback can neither rotate identity nor insert pixels after replacement.
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
#if defined(OLR_GPU_PIPELINE_BUILD)
    if (!qf.gpuFrame.isNull() &&
        ((gpuCarrierSessionIdentity == 0) != (gpuCarrierEpoch == 0) ||
         (gpuCarrierEpoch != 0 &&
          (!m_activeCarrierToken ||
           m_activeCarrierToken->sessionIdentity != gpuCarrierSessionIdentity ||
           m_activeCarrierToken->epoch != gpuCarrierEpoch)))) {
        if (qf.frame) av_frame_free(&qf.frame);
        return;
    }
#endif
    auto token = prepareCarrierTokenForFrameIngressLocked(sessionIdentity, qf.timecodeEvidence);
    if (!token || m_suppressEnqueue.load(std::memory_order_relaxed)) {
        if (qf.frame) av_frame_free(&qf.frame);
        return;
    }
    qf.carrierToken = std::move(token);
    QMutexLocker locker(&m_frameMutex);
    m_frameQueue.enqueue(std::move(qf));
    m_lastFrameEnqueueAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
    trimFrameQueueBackstopLocked(m_lastTickTargetMs.load(std::memory_order_relaxed));
}

std::optional<TimecodeEvidence>
StreamWorker::takeFrameTimecodeEvidenceForMux(std::optional<TimecodeEvidence>& selected,
                                              int64_t sessionFrameIndex) const {
    if (!selected) return std::nullopt;
    std::optional<TimecodeEvidence> evidence = std::move(selected);
    selected.reset();
    evidence->arrivalSessionFrame = sessionFrameIndex;
    evidence->sessionRate = FrameRateQ{m_targetFps, 1};
    return evidence;
}

StreamWorker::MuxFrameEvidenceSubmission StreamWorker::enqueueMuxFrameEvidence(
    int64_t ptsTicks, int64_t sourceTimecode100ns, const std::optional<TimecodeEvidence>& evidence,
    const std::shared_ptr<const SourceCarrierToken>& frameToken, bool allowSyntheticFrame) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    const uint64_t currentEpoch = m_carrierEpoch.load(std::memory_order_relaxed);
    const bool exactActiveCarrier =
        frameToken && m_activeCarrierToken &&
        frameToken->sessionIdentity == m_activeCaptureSessionIdentity &&
        frameToken->epoch == currentEpoch &&
        m_activeCarrierToken->sessionIdentity == frameToken->sessionIdentity &&
        m_activeCarrierToken->epoch == frameToken->epoch;
    const bool authorizedSynthetic = allowSyntheticFrame && exactActiveCarrier &&
                                     frameToken->sessionIdentity == 0 && !evidence &&
                                     sourceTimecode100ns < 0;
    const bool authorizedSource = exactActiveCarrier && frameToken->sessionIdentity != 0;
    if (!authorizedSource && !authorizedSynthetic) return {};
    // DecodedFrameEvidenceQueue treats its key as opaque. Both submission and native/GPU
    // output use the encoder's unchanged ptsTicks domain. The generation token lets a
    // completion already queued in Muxer revalidate after reset without taking this mutex.
    const SourceCarrierToken carrierToken = *frameToken;
    const uint64_t id = m_muxFrameEvidence.enqueue(DecodedFrameEvidence{
        ptsTicks, -1, sourceTimecode100ns, evidence,
        DecodedFrameEvidence::CarrierSessionIdentity{carrierToken.sessionIdentity},
        DecodedFrameEvidence::CarrierGeneration{carrierToken.epoch}});
    return MuxFrameEvidenceSubmission{id, carrierToken};
}

std::optional<DecodedFrameEvidence> StreamWorker::takeMuxFrameEvidence(int64_t ptsTicks) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    auto evidence = m_muxFrameEvidence.takeForOutputPts(ptsTicks);
    if (!evidence) return std::nullopt;
    const SourceCarrierToken token{evidence->carrierSessionIdentity, evidence->carrierGeneration};
    return carrierTokenIsCurrentLocked(token) ? std::move(evidence) : std::nullopt;
}

std::optional<DecodedFrameEvidence>
StreamWorker::takeMuxFrameEvidence(int64_t ptsTicks, const SourceCarrierToken& expectedToken) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    if (!carrierTokenIsCurrentLocked(expectedToken)) return std::nullopt;
    auto evidence = m_muxFrameEvidence.takeForOutputPts(ptsTicks);
    if (!evidence || evidence->carrierSessionIdentity != expectedToken.sessionIdentity ||
        evidence->carrierGeneration != expectedToken.epoch) {
        return std::nullopt;
    }
    return evidence;
}

void StreamWorker::discardMuxFrameEvidence(uint64_t submissionId) {
    std::lock_guard<std::mutex> lock(m_muxFrameEvidenceMutex);
    m_muxFrameEvidence.discard(submissionId);
}

void StreamWorker::clearMuxFrameEvidence() {
    rotateCarrier(true);
}

void StreamWorker::resetMuxFrameEvidenceLocked() {
    m_muxFrameEvidence.clear();
    m_muxFrameEvidenceIdentity.reset();
}

bool StreamWorker::muxFrameEvidenceIsCurrent(uint64_t epoch) const {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    return m_activeCarrierToken && epoch > 0 && m_activeCarrierToken->epoch == epoch &&
           m_carrierEpoch.load(std::memory_order_relaxed) == epoch;
}

void StreamWorker::emitFrameTimecodeEvidence(const TimecodeEvidence& evidence,
                                             uint64_t carrierEpoch) {
    emit frameTimecode(m_sourceIndex, m_workerInstanceIdentity, carrierEpoch, evidence);
}

qint64 StreamWorker::frameQueueBackstopBytes() const {
    bool ok = false;
    const int configuredMb = qEnvironmentVariableIntValue("OLR_FRAME_QUEUE_BACKSTOP_MB", &ok);
    if (ok && configuredMb > 0) return qint64(configuredMb) * 1024 * 1024;
#if defined(Q_OS_IOS)
    return 64LL * 1024 * 1024;
#else
    return 512LL * 1024 * 1024;
#endif
}

void StreamWorker::trimFrameQueueBackstopLocked(qint64 tickGateMs) {
    while (tickGateMs >= 0 && m_frameQueue.size() >= 2 &&
           m_frameQueue.at(1).sourcePts <= tickGateMs) {
        auto old = m_frameQueue.dequeue();
        av_frame_free(&old.frame);
    }

    const int backstopFrames = 10 * m_targetFps;
    while (m_frameQueue.size() > backstopFrames) {
        auto old = m_frameQueue.dequeue();
        av_frame_free(&old.frame);
    }

    const qint64 backstopBytes = frameQueueBackstopBytes();
    if (backstopBytes <= 0) return;

    qint64 queuedBytes = 0;
    for (const QueuedFrame& queued : m_frameQueue)
        queuedBytes += queuedFrameBytes(queued);

    while (m_frameQueue.size() > 1 && queuedBytes > backstopBytes) {
        const qint64 oldBytes = queuedFrameBytes(m_frameQueue.head());
        auto old = m_frameQueue.dequeue();
        av_frame_free(&old.frame);
        queuedBytes = qMax<qint64>(0, queuedBytes - oldBytes);
    }
}

#ifdef OLR_UNIT_TEST
const GpuEncodePump* StreamWorker::gpuEncodePumpForTest() const {
#ifdef OLR_GPU_PIPELINE_BUILD
    return m_gpuEncodePump.get();
#else
    return nullptr;
#endif
}

bool StreamWorker::preferGpuVideoFramesForIngestForTest() const {
#ifdef OLR_GPU_PIPELINE_BUILD
    return preferGpuVideoFramesForIngest();
#else
    return false;
#endif
}

bool StreamWorker::ensureGpuEncodePumpStartedForTest() {
#ifdef OLR_GPU_PIPELINE_BUILD
    return ensureGpuEncodePumpStarted();
#else
    return false;
#endif
}
#endif

#if defined(OLR_GPU_PIPELINE_BUILD)
bool StreamWorker::ensureGpuEncodePumpStarted() {
    if (!gpuPipelineEnabled() || !m_nativeEncoder) return false;
    if (m_gpuEncodePump) return true;

    m_gpuEncodeFence = GpuFence::create();
    m_gpuEncodePump = std::make_unique<GpuEncodePump>(m_nativeEncoder.get(), m_gpuEncodeFence, 4,
                                                      &m_nativeEncodeMutex);
    m_gpuEncodePump->start();
    return true;
}

bool StreamWorker::preferGpuVideoFramesForIngest() const {
#if defined(Q_OS_IOS)
    // iOS GPU playback is default-on, but recorder surface encode is still held off: a stalled
    // VideoToolbox surface encode can block live ingest. Keep recording on CPU frames for now.
    return false;
#elif defined(__APPLE__)
    return gpuPipelineEnabled() && gpuRecordSurfaceEncodeEnabled() &&
           m_videoCodec == VideoCodecChoice::H264Hardware &&
           !m_gpuEncodeCpuFallback.load(std::memory_order_acquire);
#else
    return gpuPipelineEnabled() && m_videoCodec == VideoCodecChoice::H264Hardware &&
           !m_gpuEncodeCpuFallback.load(std::memory_order_acquire);
#endif
}

bool StreamWorker::tryLatchGpuEncodeCpuFallback(const SourceCarrierToken& failureCarrier) {
    {
        std::lock_guard<std::mutex> epochLock(m_epochMutex);
        if (!carrierTokenIsCurrentLocked(failureCarrier) ||
            m_gpuEncodeCpuFallback.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        // Authorize, latch, and invalidate the failing carrier as one epoch
        // transaction. rotateCarrierLocked preserves epoch -> evidence order.
        rotateCarrierLocked(true);
    }
    // Cancellation can invoke failure callbacks. Keep it outside carrier locks;
    // their old immutable tokens now fail the token-scoped latch above.
    if (m_gpuEncodePump) m_gpuEncodePump->cancelPending();
    return true;
}

void StreamWorker::latchGpuEncodeCpuFallback() {
    SourceCarrierToken activeCarrier;
    {
        std::lock_guard<std::mutex> epochLock(m_epochMutex);
        if (!m_activeCarrierToken) return;
        activeCarrier = *m_activeCarrierToken;
    }
    tryLatchGpuEncodeCpuFallback(activeCarrier);
}

ImportedGpuVideoFrame
StreamWorker::importGpuVideoFrameForEncode(void* nativeDecodedImage, const FrameMetadata& metadata,
                                           [[maybe_unused]] bool latchFallbackOnFailure) {
    ImportedGpuVideoFrame imported;
#if defined(__APPLE__)
    auto surface = wrapAppleImageBuffer(nativeDecodedImage);
    if (!surface) return imported;

    const qint64 bytes = gpuSurfaceBytes(*surface);
    imported.frame = makeGpuFrameHandle(std::move(surface), nullptr, metadata, nullptr,
                                        GpuBudgetCharge(bytes, GpuBudgetTag::RecorderWrap));
    imported.fenceValue = 0;
#elif defined(_WIN32)
    if (!m_gpuEncodeImportEdge || m_gpuEncodeImportEdge->deviceLost()) {
        QString error;
        m_gpuEncodeImportEdge = WinGpuImportEdge::create(&error);
        if (!m_gpuEncodeImportEdge) {
            if (!error.isEmpty()) {
                qWarning() << "Source" << m_sourceIndex
                           << "Windows GPU video import unavailable:" << error;
            }
            if (latchFallbackOnFailure) latchGpuEncodeCpuFallback();
            return imported;
        }
    }

    auto surface = m_gpuEncodeImportEdge->tryImportSurface(nativeDecodedImage, metadata.key.width,
                                                           metadata.key.height);
    if (!surface) {
        if (latchFallbackOnFailure) latchGpuEncodeCpuFallback();
        return imported;
    }

    std::shared_ptr<GpuFence> fence = makeD3D11GpuFence(surface->device());
    if (!fence) {
        if (latchFallbackOnFailure) latchGpuEncodeCpuFallback();
        return imported;
    }
    const uint64_t fenceValue = fence ? fence->signal() : 0;
    const qint64 bytes = gpuSurfaceBytes(*surface);
    imported.frame = WinGpuImportEdge::makeGpuFrameHandleForTest(
        std::move(surface), metadata, fence, GpuBudgetCharge(bytes, GpuBudgetTag::RecorderWrap));
    imported.fenceValue = fenceValue;
#else
    Q_UNUSED(nativeDecodedImage);
    Q_UNUSED(metadata);
#endif
    return imported;
}

ImportedGpuVideoFrame StreamWorker::importGpuVideoFrameForSession(uint64_t sessionIdentity,
                                                                  void* nativeDecodedImage,
                                                                  const FrameMetadata& metadata) {
    SourceCarrierToken failureCarrier;
    {
        std::lock_guard<std::mutex> epochLock(m_epochMutex);
        if (sessionIdentity == 0 || sessionIdentity != m_activeCaptureSessionIdentity ||
            !m_activeCarrierToken) {
            return {};
        }
        failureCarrier = *m_activeCarrierToken;
    }
    // Import can enter platform GPU APIs, so carrier locks are not held across
    // the device operation. Revalidate before publishing the result or latching
    // fallback; the matching onVideoFrame callback revalidates again before its
    // queue mutation.
    ImportedGpuVideoFrame imported;
#ifdef OLR_UNIT_TEST
    if (m_gpuImportForTest)
        imported = m_gpuImportForTest(nativeDecodedImage, metadata);
    else
#endif
        imported = importGpuVideoFrameForEncode(nativeDecodedImage, metadata, false);
#ifdef OLR_UNIT_TEST
    auto afterImport = std::move(m_afterGpuImportForTest);
    if (afterImport) afterImport();
#endif
    {
        std::lock_guard<std::mutex> epochLock(m_epochMutex);
        if (sessionIdentity == 0 || sessionIdentity != m_activeCaptureSessionIdentity ||
            !carrierTokenIsCurrentLocked(failureCarrier)) {
            return {};
        }
    }
    if (imported.frame.isNull()) tryLatchGpuEncodeCpuFallback(failureCarrier);
    if (!imported.frame.isNull()) {
        imported.carrierSessionIdentity = failureCarrier.sessionIdentity;
        imported.carrierEpoch = failureCarrier.epoch;
    }
    return imported;
}
#endif

void debugTimestamp(const QString& prefix, int trackIndex) {
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    qDebug() << "[" << timeStr << "] [Track" << trackIndex << "]" << prefix;
}

void StreamWorker::stop() {
    rotateCarrier(false);
    m_restartCapture = 1;
    m_captureRunning = false;
    {
        QMutexLocker locker(&m_sessionMutex);
        if (m_activeSession) {
            m_activeSession->requestStop();
        }
    }
    this->quit();
}

void StreamWorker::run() {
    // 1. Setup the persistent encoder context (MPEG-2) or native encoder (H.264).
    if (!setupEncoder(&m_persistentEncCtx)) return;

    // 2. Enter the event loop. The thread stays alive, waiting for
    // signals (masterPulse) or concurrent tasks (captureLoop).
    exec();

    // exec() has returned, so no further queued pulse can run on this
    // thread.  Re-assert shutdown before joining the capture thread: a
    // pulse delivered between stop() and quit() taking effect could have
    // started captureLoop after stop() cleared the flags, which would
    // otherwise leave join() stuck forever.
    m_restartCapture = 1;
    m_captureRunning = false;
    // The capture thread is started and joined on this (the worker) thread
    // only, so there is no cross-thread race on m_captureThread itself.
    if (m_captureThread.joinable()) m_captureThread.join();

    // Cleanup when exec() returns (on stop)
#if defined(OLR_GPU_PIPELINE_BUILD)
    if (m_gpuEncodePump) m_gpuEncodePump->stop();
#endif
    avcodec_free_context(&m_persistentEncCtx);
    m_nativeEncoder.reset();
    av_frame_free(&m_latestFrame);

    while (!m_frameQueue.isEmpty()) {
        auto qf = m_frameQueue.dequeue();
        av_frame_free(&qf.frame);
    }
}

void StreamWorker::onMasterPulse(int64_t frameIndex, int64_t streamTimeMs) {
    m_internalFrameCount = frameIndex;

    // Snapshot BOTH trims once per pulse so video and audio of this tick use the
    // SAME combined offset even if the UI thread (operator trim) or ReplayManager's
    // phase servo (servo trim) changes them mid-pulse (keeps A/V locked). The operator
    // trim and the bounded inter-cam servo COMPOSE additively into one jitter-pull
    // offset; the servo defaults to 0, so this is byte-identical when no servo runs.
    const int64_t trimMs = m_trimOffsetMs.load(std::memory_order_relaxed) +
                           m_servoTrimOffsetMs.load(std::memory_order_relaxed);

    // Snapshot the jitter window once per pulse too, so video + audio of this tick
    // share one value even if a URL change flips it mid-pulse (keeps A/V locked).
    const int64_t jitterMs = m_activeJitterWindowMs.load(std::memory_order_relaxed);

    // Publish this tick's jitter-pull gate for the capture thread's
    // queue pre-drain (see captureLoop).
    m_lastTickTargetMs.store(
        qMax<int64_t>(0, (frameIndex * 1000) / m_targetFps - jitterMs - trimMs),
        std::memory_order_relaxed);

    if (!m_persistentEncCtx && !m_nativeEncoder) return;

    // Stall detection: if connected but no frames for too long, signal restart.
    const int64_t lastEnq = m_lastFrameEnqueueAtMs.load(std::memory_order_relaxed);
    if (m_captureRunning && m_connected && lastEnq >= 0 &&
        m_monotonic.elapsed() - lastEnq > m_stallTimeoutMs) {
        qDebug() << "Source" << m_sourceIndex << "No frames queued. Forcing restart...";
        m_restartCapture = 1;
    }

    // Start the dedicated capture thread exactly once, on the first pulse.
    // captureLoop() loops internally on reconnect/URL-change (m_restartCapture)
    // and only returns when m_captureRunning goes false, so it never needs
    // re-launching.  We start it on its own std::thread (not the shared
    // global pool) so N infinite captureLoops can't saturate that pool.
    // Both start (here) and join (in run() post-exec) happen on the worker
    // thread, so m_captureThread is never touched cross-thread.
    if (!m_captureThread.joinable()) {
        m_restartCapture = 0;
        m_captureRunning = true;
        m_captureThread = std::thread([this]() { this->captureLoop(); });
    }

    processEncoderTick(m_persistentEncCtx, streamTimeMs, trimMs, jitterMs);
}

uint64_t StreamWorker::poolId(size_t index, uint32_t generation) noexcept {
    return (uint64_t(generation) << 32) | uint64_t(index + 1);
}

bool StreamWorker::decodePoolId(uint64_t id, size_t capacity, size_t* index,
                                uint32_t* generation) noexcept {
    if (id == 0) return false;
    const uint64_t encodedIndex = id & 0xffffffffULL;
    if (encodedIndex == 0 || encodedIndex > capacity) return false;
    *index = size_t(encodedIndex - 1);
    *generation = uint32_t(id >> 32);
    return *generation != 0;
}

uint64_t StreamWorker::acquireEncodeSubmission(bool gpu, int track, AVStream* stream,
                                               bool* havePacket,
                                               const SourceCarrierToken& carrierToken,
                                               uint64_t evidenceSubmissionId, int64_t streamTimeMs,
                                               const QByteArray& metadata) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    if (!carrierTokenIsCurrentLocked(carrierToken)) return 0;
    std::lock_guard<std::mutex> poolLock(m_submissionPoolMutex);
    for (size_t offset = 0; offset < kSubmissionPoolCapacity; ++offset) {
        const size_t index = (m_nextSubmissionSlot + offset) % kSubmissionPoolCapacity;
        EncodeSubmissionSlot& slot = m_submissionPool[index];
        // A wrapped generation would make an ancient asynchronous ID valid
        // again. Permanently retire the fixed slot once its ID space is spent.
        if (slot.active || slot.generation == std::numeric_limits<uint32_t>::max()) continue;
        const uint32_t generation = slot.generation + 1;
        slot = EncodeSubmissionSlot{};
        slot.active = true;
        slot.generation = generation;
        slot.gpu = gpu;
        slot.track = track;
        slot.stream = stream;
        slot.havePacket = havePacket;
        slot.carrierToken = carrierToken;
        slot.evidenceSubmissionId = evidenceSubmissionId;
        slot.streamTimeMs = streamTimeMs;
        slot.metadata = metadata;
        m_nextSubmissionSlot = (index + 1) % kSubmissionPoolCapacity;
        return poolId(index, generation);
    }
    return 0;
}

bool StreamWorker::setEncodeSubmissionEvidenceId(uint64_t submissionId,
                                                 uint64_t evidenceSubmissionId) {
    if (evidenceSubmissionId == 0) return false;
    std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
    size_t index = 0;
    uint32_t generation = 0;
    if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) return false;
    EncodeSubmissionSlot& slot = m_submissionPool[index];
    if (!slot.active || slot.generation != generation || slot.evidenceSubmissionId != 0)
        return false;
    slot.evidenceSubmissionId = evidenceSubmissionId;
    return true;
}

uint64_t StreamWorker::reserveMuxCompletion(uint64_t encodeSubmissionId) {
    return reserveMuxCompletion(encodeSubmissionId, SourceCarrierToken{});
}

uint64_t StreamWorker::reserveMuxCompletion(uint64_t encodeSubmissionId,
                                            const SourceCarrierToken& carrierToken) {
    std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
    SourceCarrierToken immutableCarrier = carrierToken;
    if (encodeSubmissionId != 0) {
        size_t groupIndex = 0;
        uint32_t groupGeneration = 0;
        if (!decodePoolId(encodeSubmissionId, kSubmissionPoolCapacity, &groupIndex,
                          &groupGeneration) ||
            !m_submissionPool[groupIndex].active ||
            m_submissionPool[groupIndex].generation != groupGeneration) {
            return 0;
        }
        immutableCarrier = m_submissionPool[groupIndex].carrierToken;
    }
    for (size_t offset = 0; offset < kMuxCompletionPoolCapacity; ++offset) {
        const size_t index = (m_nextMuxCompletionSlot + offset) % kMuxCompletionPoolCapacity;
        MuxCompletionSlot& slot = m_muxCompletionPool[index];
        // Fail closed rather than wrap and revalidate an ancient completion.
        if (slot.active || slot.generation == std::numeric_limits<uint32_t>::max()) continue;
        const uint32_t generation = slot.generation + 1;
        slot = MuxCompletionSlot{};
        slot.active = true;
        slot.generation = generation;
        slot.encodeSubmissionId = encodeSubmissionId;
        slot.carrierToken = immutableCarrier;
        m_nextMuxCompletionSlot = (index + 1) % kMuxCompletionPoolCapacity;
        return poolId(index, generation);
    }
    return 0;
}

void StreamWorker::releaseMuxCompletionReservation(uint64_t completionId) {
    std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
    size_t index = 0;
    uint32_t generation = 0;
    if (!decodePoolId(completionId, kMuxCompletionPoolCapacity, &index, &generation)) return;
    MuxCompletionSlot& slot = m_muxCompletionPool[index];
    if (!slot.active || slot.generation != generation) return;
    const uint32_t keepGeneration = slot.generation;
    slot = MuxCompletionSlot{};
    slot.generation = keepGeneration;
}

bool StreamWorker::populateMuxCompletion(uint64_t completionId,
                                         const SourceCarrierToken& carrierToken,
                                         std::optional<TimecodeEvidence> evidence) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
    if (!carrierTokenIsCurrentLocked(carrierToken)) return false;
    std::lock_guard<std::mutex> poolLock(m_submissionPoolMutex);
    size_t index = 0;
    uint32_t generation = 0;
    if (!decodePoolId(completionId, kMuxCompletionPoolCapacity, &index, &generation)) return false;
    MuxCompletionSlot& slot = m_muxCompletionPool[index];
    if (!slot.active || slot.generation != generation) return false;
    if (slot.carrierToken.epoch != 0 &&
        (slot.carrierToken.sessionIdentity != carrierToken.sessionIdentity ||
         slot.carrierToken.epoch != carrierToken.epoch)) {
        return false;
    }
    if (slot.encodeSubmissionId != 0) {
        size_t groupIndex = 0;
        uint32_t groupGeneration = 0;
        if (!decodePoolId(slot.encodeSubmissionId, kSubmissionPoolCapacity, &groupIndex,
                          &groupGeneration))
            return false;
        EncodeSubmissionSlot& group = m_submissionPool[groupIndex];
        if (!group.active || group.generation != groupGeneration) return false;
        ++group.pendingWrites;
    }
    slot.carrierToken = carrierToken;
    slot.evidence = std::move(evidence);
    return true;
}

void StreamWorker::releaseEncodeSubmissionLocked(size_t index) {
    EncodeSubmissionSlot& slot = m_submissionPool[index];
    const uint32_t keepGeneration = slot.generation;
    slot = EncodeSubmissionSlot{};
    slot.generation = keepGeneration;
}

void StreamWorker::finishEncodeSubmission(uint64_t submissionId) {
    std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
    size_t index = 0;
    uint32_t generation = 0;
    if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) return;
    EncodeSubmissionSlot& slot = m_submissionPool[index];
    if (!slot.active || slot.generation != generation) return;
    slot.encoderFinished = true;
    if (slot.pendingWrites == 0) releaseEncodeSubmissionLocked(index);
}

void StreamWorker::failEncodeSubmission(uint64_t submissionId) {
    uint64_t evidenceSubmissionId = 0;
    bool triggerFallback = false;
    SourceCarrierToken failureCarrier;
    {
        std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
        size_t index = 0;
        uint32_t generation = 0;
        if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) return;
        EncodeSubmissionSlot& slot = m_submissionPool[index];
        if (!slot.active || slot.generation != generation) return;
        slot.encoderFinished = true;
        evidenceSubmissionId = slot.evidenceSubmissionId;
        failureCarrier = slot.carrierToken;
        if (slot.gpu && !slot.fallbackTriggered) {
            slot.fallbackTriggered = true;
            triggerFallback = true;
        }
        if (slot.pendingWrites == 0) releaseEncodeSubmissionLocked(index);
    }
    if (evidenceSubmissionId != 0) discardMuxFrameEvidence(evidenceSubmissionId);
#if defined(OLR_GPU_PIPELINE_BUILD)
    // A delayed failure from an invalidated carrier is cleanup, not evidence
    // that the replacement session's GPU path failed. Authorize the exact
    // immutable submission carrier before cancelling current work or rotating.
    if (triggerFallback) {
#ifdef OLR_UNIT_TEST
        runBeforeGpuFallbackTryForTest();
#endif
        tryLatchGpuEncodeCpuFallback(failureCarrier);
    }
#else
    Q_UNUSED(triggerFallback);
#endif
}

void StreamWorker::completeMuxWrite(uint64_t completionId, bool written) {
    std::optional<TimecodeEvidence> evidence;
    SourceCarrierToken carrierToken;
    bool emitSidecars = false;
    bool triggerFallback = false;
    int track = -1;
    int64_t streamTimeMs = 0;
    QByteArray metadata;
    {
        std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
        size_t index = 0;
        uint32_t generation = 0;
        if (!decodePoolId(completionId, kMuxCompletionPoolCapacity, &index, &generation)) return;
        MuxCompletionSlot& slot = m_muxCompletionPool[index];
        if (!slot.active || slot.generation != generation) return;
        evidence = std::move(slot.evidence);
        carrierToken = slot.carrierToken;
        const uint64_t encodeSubmissionId = slot.encodeSubmissionId;
        const uint32_t keepGeneration = slot.generation;
        slot = MuxCompletionSlot{};
        slot.generation = keepGeneration;

        if (encodeSubmissionId != 0) {
            size_t groupIndex = 0;
            uint32_t groupGeneration = 0;
            if (decodePoolId(encodeSubmissionId, kSubmissionPoolCapacity, &groupIndex,
                             &groupGeneration)) {
                EncodeSubmissionSlot& group = m_submissionPool[groupIndex];
                if (group.active && group.generation == groupGeneration) {
                    if (group.pendingWrites > 0) --group.pendingWrites;
                    if (written && !group.sidecarsEmitted) {
                        group.sidecarsEmitted = true;
                        emitSidecars = true;
                        track = group.track;
                        streamTimeMs = group.streamTimeMs;
                        metadata = std::move(group.metadata);
                    }
                    if (!written && group.gpu && !group.fallbackTriggered) {
                        group.fallbackTriggered = true;
                        triggerFallback = true;
                    }
                    if (group.encoderFinished && group.pendingWrites == 0)
                        releaseEncodeSubmissionLocked(groupIndex);
                }
            }
        }
    }

    const bool carrierAuthorized = carrierTokenIsCurrent(carrierToken);
    if (written && evidence && carrierAuthorized)
        emitFrameTimecodeEvidence(*evidence, carrierToken.epoch);
    if (emitSidecars && carrierAuthorized && !metadata.isEmpty() && m_muxer)
        m_muxer->writeMetadataPacket(track, streamTimeMs, metadata);
#if defined(OLR_GPU_PIPELINE_BUILD)
    if (triggerFallback) tryLatchGpuEncodeCpuFallback(carrierToken);
#else
    Q_UNUSED(triggerFallback);
#endif
}

void StreamWorker::bufferEncodedPacket(uint64_t submissionId, const QByteArray& data,
                                       int64_t ptsTicks, bool keyframe) {
    std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
    size_t index = 0;
    uint32_t generation = 0;
    if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) return;
    EncodeSubmissionSlot& slot = m_submissionPool[index];
    if (!slot.active || slot.generation != generation || slot.encoderFinished) return;
    if (slot.bufferedPacketCount >= slot.bufferedPackets.size()) {
        slot.packetOverflow = true;
        return;
    }
    BufferedEncodedPacket& packet = slot.bufferedPackets[slot.bufferedPacketCount++];
    packet.data = data;
    packet.ptsTicks = ptsTicks;
    packet.keyframe = keyframe;
}

bool StreamWorker::bufferedSubmissionReady(uint64_t submissionId) const {
    std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
    size_t index = 0;
    uint32_t generation = 0;
    if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) return false;
    const EncodeSubmissionSlot& slot = m_submissionPool[index];
    return slot.active && slot.generation == generation && !slot.packetOverflow;
}

bool StreamWorker::takeBufferedSubmissionPacket(uint64_t submissionId,
                                                BufferedEncodedPacket* packet) {
    if (!packet) return false;
    std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
    size_t index = 0;
    uint32_t generation = 0;
    if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) return false;
    EncodeSubmissionSlot& slot = m_submissionPool[index];
    if (!slot.active || slot.generation != generation || slot.packetOverflow ||
        slot.nextBufferedPacket >= slot.bufferedPacketCount) {
        return false;
    }
    *packet = std::move(slot.bufferedPackets[slot.nextBufferedPacket++]);
    return true;
}

void StreamWorker::commitBufferedEncodeSubmission(uint64_t submissionId) {
    std::array<BufferedEncodedPacket, kMaxPacketsPerSubmission> bufferedPackets;
    size_t packetCount = 0;
    int track = -1;
    AVStream* stream = nullptr;
    bool* havePacket = nullptr;
    bool gpu = false;
    SourceCarrierToken expectedCarrierToken;
    {
        std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
        size_t index = 0;
        uint32_t generation = 0;
        if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) return;
        EncodeSubmissionSlot& slot = m_submissionPool[index];
        if (!slot.active || slot.generation != generation || slot.encoderFinished ||
            slot.packetOverflow) {
            return;
        }
        packetCount = slot.bufferedPacketCount;
        track = slot.track;
        stream = slot.stream;
        havePacket = slot.havePacket;
        gpu = slot.gpu;
        expectedCarrierToken = slot.carrierToken;
        for (size_t i = 0; i < packetCount; ++i)
            bufferedPackets[i] = std::move(slot.bufferedPackets[i]);
        slot.nextBufferedPacket = packetCount;
    }

    if (packetCount == 0) {
        finishEncodeSubmission(submissionId);
        return;
    }

    std::array<AVPacket*, kMaxPacketsPerSubmission> avPackets{};
    auto freePackets = [&avPackets] {
        for (AVPacket*& packet : avPackets)
            av_packet_free(&packet);
    };
    for (size_t i = 0; i < packetCount; ++i) {
        const BufferedEncodedPacket& buffered = bufferedPackets[i];
        AVPacket*& packet = avPackets[i];
        packet = av_packet_alloc();
        if (!packet || buffered.data.size() > std::numeric_limits<int>::max() ||
            av_new_packet(packet, static_cast<int>(buffered.data.size())) < 0 || !stream ||
            !m_muxer) {
            freePackets();
            failEncodeSubmission(submissionId);
            return;
        }
        memcpy(packet->data, buffered.data.constData(), buffered.data.size());
        packet->stream_index = track;
        packet->pts = packet->dts =
            av_rescale_q(buffered.ptsTicks, AVRational{1, m_targetFps}, stream->time_base);
        packet->duration = av_rescale_q(1, AVRational{1, m_targetFps}, stream->time_base);
        if (buffered.keyframe) packet->flags |= AV_PKT_FLAG_KEY;
    }

    enum class Preparation { Ready, StaleCarrier, Failed };
    Preparation preparation = Preparation::Failed;
    std::array<uint64_t, kMaxPacketsPerSubmission> completionIds{};
    int64_t sourceTimecode100ns = -1;
    {
        // Batch authority and completion admission use the established
        // epoch->evidence->pool lock order. Evidence is consumed only after
        // every fixed completion slot is known to be available.
        std::lock_guard<std::mutex> epochLock(m_epochMutex);
        std::lock_guard<std::mutex> evidenceLock(m_muxFrameEvidenceMutex);
        std::lock_guard<std::mutex> poolLock(m_submissionPoolMutex);
        size_t submissionIndex = 0;
        uint32_t submissionGeneration = 0;
        if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &submissionIndex,
                          &submissionGeneration)) {
            preparation = Preparation::StaleCarrier;
        } else {
            EncodeSubmissionSlot& submission = m_submissionPool[submissionIndex];
            if (!submission.active || submission.generation != submissionGeneration ||
                !carrierTokenIsCurrentLocked(expectedCarrierToken)) {
                preparation = Preparation::StaleCarrier;
            } else {
                std::array<size_t, kMaxPacketsPerSubmission> completionIndices{};
                size_t available = 0;
                for (size_t offset = 0;
                     offset < kMuxCompletionPoolCapacity && available < packetCount; ++offset) {
                    const size_t index =
                        (m_nextMuxCompletionSlot + offset) % kMuxCompletionPoolCapacity;
                    const MuxCompletionSlot& candidate = m_muxCompletionPool[index];
                    if (candidate.active ||
                        candidate.generation == std::numeric_limits<uint32_t>::max()) {
                        continue;
                    }
                    completionIndices[available++] = index;
                }
                if (available == packetCount) {
                    std::array<int64_t, kMaxPacketsPerSubmission> uniquePts{};
                    std::array<uint64_t, kMaxPacketsPerSubmission> evidenceIds{};
                    std::array<size_t, kMaxPacketsPerSubmission> packetEvidenceIndices{};
                    std::array<size_t, kMaxPacketsPerSubmission> firstPacketIndices{};
                    size_t uniqueCount = 0;
                    bool allEvidencePresent = true;
                    for (size_t i = 0; i < packetCount && allEvidencePresent; ++i) {
                        size_t evidenceIndex = 0;
                        while (evidenceIndex < uniqueCount &&
                               uniquePts[evidenceIndex] != bufferedPackets[i].ptsTicks) {
                            ++evidenceIndex;
                        }
                        if (evidenceIndex == uniqueCount) {
                            const auto match =
                                m_muxFrameEvidence.findForOutputPts(bufferedPackets[i].ptsTicks);
                            if (!match ||
                                match->carrierSessionIdentity !=
                                    expectedCarrierToken.sessionIdentity ||
                                match->carrierGeneration != expectedCarrierToken.epoch) {
                                allEvidencePresent = false;
                                break;
                            }
                            uniquePts[uniqueCount] = bufferedPackets[i].ptsTicks;
                            evidenceIds[uniqueCount] = match->submissionId;
                            firstPacketIndices[uniqueCount] = i;
                            evidenceIndex = uniqueCount++;
                        }
                        packetEvidenceIndices[i] = evidenceIndex;
                    }
                    if (allEvidencePresent) {
                        std::array<std::optional<DecodedFrameEvidence>, kMaxPacketsPerSubmission>
                            frameEvidence;
                        for (size_t i = 0; i < uniqueCount; ++i)
                            frameEvidence[i] =
                                m_muxFrameEvidence.takeBySubmissionId(evidenceIds[i]);
                        sourceTimecode100ns =
                            frameEvidence[packetEvidenceIndices[0]]->sourceTimecode100ns;
                        for (size_t i = 0; i < packetCount; ++i) {
                            const size_t completionIndex = completionIndices[i];
                            MuxCompletionSlot& completion = m_muxCompletionPool[completionIndex];
                            const uint32_t generation = completion.generation + 1;
                            completion = MuxCompletionSlot{};
                            completion.active = true;
                            completion.generation = generation;
                            completion.carrierToken = expectedCarrierToken;
                            completion.encodeSubmissionId = submissionId;
                            const size_t evidenceIndex = packetEvidenceIndices[i];
                            if (firstPacketIndices[evidenceIndex] == i) {
                                completion.evidence =
                                    std::move(frameEvidence[evidenceIndex]->timecodeEvidence);
                            }
                            completionIds[i] = poolId(completionIndex, generation);
                        }
                        submission.evidenceResolved = true;
                        submission.pendingWrites += static_cast<uint32_t>(packetCount);
                        m_nextMuxCompletionSlot =
                            (completionIndices[packetCount - 1] + 1) % kMuxCompletionPoolCapacity;
                        preparation = Preparation::Ready;
                    } else {
                        // Delayed output can belong to a carrier invalidated before
                        // this newer submission. Consume only that old mapping;
                        // preserve the current submission's mapping for its later
                        // packet and never convert stale output into GPU fallback.
                        preparation = Preparation::StaleCarrier;
                    }
                }
            }
        }
    }

    if (preparation != Preparation::Ready) {
        freePackets();
        if (preparation == Preparation::StaleCarrier)
            finishEncodeSubmission(submissionId);
        else
            failEncodeSubmission(submissionId);
        return;
    }

    QString startTimecodeCandidate;
    if (sourceTimecode100ns >= 0) {
        const Smpte12mTimecode startTc =
            Smpte12m::from100ns(sourceTimecode100ns, Smpte12m::kTimecodeNominalFps);
        char buf[12];
        startTimecodeCandidate = QString::fromLatin1(Smpte12m::format(startTc, buf));
    }
    std::array<Muxer::PacketWriteRequest, kMaxPacketsPerSubmission> requests;
    const Muxer::PacketCarrierGuard guard = packetCarrierGuard(expectedCarrierToken);
    for (size_t i = 0; i < packetCount; ++i) {
        requests[i].packet = avPackets[i];
        requests[i].onWritten = muxCompletionCallback(completionIds[i]);
        requests[i].carrierGuard = guard;
        if (i == 0) requests[i].startTimecodeCandidate = startTimecodeCandidate;
    }
#ifdef OLR_UNIT_TEST
    runBeforeMuxPacketWriteForTest();
#endif
    const bool accepted = m_muxer->writePacketBatch(requests.data(), packetCount);
    freePackets();
    if (accepted && havePacket && !gpu) *havePacket = true;
    finishEncodeSubmission(submissionId);
}

void StreamWorker::handleEncodedPacket(uint64_t submissionId, const QByteArray& data,
                                       int64_t ptsTicks, bool keyframe) {
    int track = -1;
    AVStream* stream = nullptr;
    bool* havePacket = nullptr;
    SourceCarrierToken expectedCarrierToken;
    bool gpu = false;
    {
        std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
        size_t index = 0;
        uint32_t generation = 0;
        if (!decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) return;
        const EncodeSubmissionSlot& slot = m_submissionPool[index];
        if (!slot.active || slot.generation != generation) return;
        track = slot.track;
        stream = slot.stream;
        havePacket = slot.havePacket;
        expectedCarrierToken = slot.carrierToken;
        gpu = slot.gpu;
    }

    const uint64_t completionId = reserveMuxCompletion(submissionId);
    if (completionId == 0) {
        if (gpu) failEncodeSubmission(submissionId);
        return;
    }
    auto frameEvidence = takeMuxFrameEvidence(ptsTicks, expectedCarrierToken);
    bool evidenceWasAlreadyResolved = false;
    bool submissionStillActive = false;
    {
        std::lock_guard<std::mutex> lock(m_submissionPoolMutex);
        size_t index = 0;
        uint32_t generation = 0;
        if (decodePoolId(submissionId, kSubmissionPoolCapacity, &index, &generation)) {
            EncodeSubmissionSlot& slot = m_submissionPool[index];
            if (slot.active && slot.generation == generation) {
                submissionStillActive = true;
                evidenceWasAlreadyResolved = slot.evidenceResolved;
                if (frameEvidence) slot.evidenceResolved = true;
            }
        }
    }
    if (!submissionStillActive) {
        releaseMuxCompletionReservation(completionId);
        return;
    }
    // The first packet must resolve the exact input mapping. Later packets from
    // the same encoder submission share that authorization but carry no second
    // timecode observation.
    if (expectedCarrierToken.epoch > 0 && !frameEvidence && !evidenceWasAlreadyResolved) {
        releaseMuxCompletionReservation(completionId);
        if (gpu) failEncodeSubmission(submissionId);
        return;
    }
    std::optional<TimecodeEvidence> muxEvidence;
    if (frameEvidence) muxEvidence = std::move(frameEvidence->timecodeEvidence);
    const SourceCarrierToken completionToken =
        frameEvidence ? SourceCarrierToken{frameEvidence->carrierSessionIdentity,
                                           frameEvidence->carrierGeneration}
                      : expectedCarrierToken;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt || data.size() > std::numeric_limits<int>::max() ||
        av_new_packet(pkt, static_cast<int>(data.size())) < 0 || !stream || !m_muxer) {
        av_packet_free(&pkt);
        releaseMuxCompletionReservation(completionId);
        if (gpu) failEncodeSubmission(submissionId);
        return;
    }
    memcpy(pkt->data, data.constData(), data.size());
    pkt->stream_index = track;
    pkt->pts = pkt->dts = av_rescale_q(ptsTicks, AVRational{1, m_targetFps}, stream->time_base);
    pkt->duration = av_rescale_q(1, AVRational{1, m_targetFps}, stream->time_base);
    if (keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

    QString startTimecodeCandidate;
    if (frameEvidence && frameEvidence->sourceTimecode100ns >= 0) {
        const Smpte12mTimecode startTc =
            Smpte12m::from100ns(frameEvidence->sourceTimecode100ns, Smpte12m::kTimecodeNominalFps);
        char buf[12];
        startTimecodeCandidate = QString::fromLatin1(Smpte12m::format(startTc, buf));
    }
#ifdef OLR_UNIT_TEST
    runBeforeMuxPacketWriteForTest();
#endif
    if (!populateMuxCompletion(completionId, completionToken, std::move(muxEvidence))) {
        av_packet_free(&pkt);
        releaseMuxCompletionReservation(completionId);
        if (gpu) failEncodeSubmission(submissionId);
        return;
    }
    const bool accepted =
        m_muxer->writePacket(pkt, muxCompletionCallback(completionId), startTimecodeCandidate,
                             packetCarrierGuard(completionToken));
    if (accepted && havePacket) *havePacket = true;
    av_packet_free(&pkt);
}

void StreamWorker::encodedPacketThunk(void* context, uint64_t submissionId, const QByteArray& data,
                                      int64_t ptsTicks, bool keyframe) {
    static_cast<StreamWorker*>(context)->handleEncodedPacket(submissionId, data, ptsTicks,
                                                             keyframe);
}

void StreamWorker::bufferedPacketThunk(void* context, uint64_t submissionId, const QByteArray& data,
                                       int64_t ptsTicks, bool keyframe) {
    static_cast<StreamWorker*>(context)->bufferEncodedPacket(submissionId, data, ptsTicks,
                                                             keyframe);
}

void StreamWorker::encodeFailureThunk(void* context, uint64_t submissionId) {
    static_cast<StreamWorker*>(context)->failEncodeSubmission(submissionId);
}

void StreamWorker::encodeFinishedThunk(void* context, uint64_t submissionId) {
    static_cast<StreamWorker*>(context)->finishEncodeSubmission(submissionId);
}

void StreamWorker::bufferedEncodeFinishedThunk(void* context, uint64_t submissionId) {
    static_cast<StreamWorker*>(context)->commitBufferedEncodeSubmission(submissionId);
}

void StreamWorker::muxWriteCompletionThunk(void* context, uint64_t completionId, bool written) {
    static_cast<StreamWorker*>(context)->completeMuxWrite(completionId, written);
}

NativeVideoEncoder::PacketCallback
StreamWorker::packetCallbackForSubmission(uint64_t submissionId) noexcept {
    return NativeVideoEncoder::PacketCallback{this, submissionId,
                                              &StreamWorker::encodedPacketThunk};
}

NativeVideoEncoder::PacketCallback
StreamWorker::bufferedPacketCallbackForSubmission(uint64_t submissionId) noexcept {
    return NativeVideoEncoder::PacketCallback{this, submissionId,
                                              &StreamWorker::bufferedPacketThunk};
}

#if defined(OLR_GPU_PIPELINE_BUILD)
GpuEncodePump::JobCallbacks
StreamWorker::gpuCallbacksForSubmission(uint64_t submissionId) noexcept {
    return GpuEncodePump::JobCallbacks{this, submissionId, &StreamWorker::bufferedPacketThunk,
                                       &StreamWorker::encodeFailureThunk,
                                       &StreamWorker::bufferedEncodeFinishedThunk};
}
#endif

Muxer::PacketWriteCallback StreamWorker::muxCompletionCallback(uint64_t completionId) noexcept {
    return Muxer::PacketWriteCallback{this, completionId, &StreamWorker::muxWriteCompletionThunk};
}

void StreamWorker::processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs, int64_t trimMs,
                                      int64_t jitterMs) {
    AVPacket* outPkt = av_packet_alloc();
    bool havePacket = false;
    std::optional<DecodedFrameEvidence> softwareFrameEvidence;
    uint64_t softwareCompletionId = 0;
    int track = -1;
    const int64_t currentRecordingTimeMs = (m_internalFrameCount * 1000) / m_targetFps;

    AVFrame* pulled = nullptr;
    int64_t pulledTimecode100ns = -1;
    std::optional<TimecodeEvidence> pulledTimecodeEvidence;
    std::shared_ptr<const SourceCarrierToken> pulledCarrierToken;
    bool pulledAnyFrame = false;
#ifdef OLR_GPU_PIPELINE_BUILD
    FrameHandle pulledGpuFrame;
    uint64_t pulledGpuFenceValue = 0;
#endif
    const bool paintBlue = m_paintBlue.fetchAndStoreRelaxed(0) != 0;

    // The mutex only guards m_frameQueue (shared with the capture
    // thread).  m_latestFrame and the encoder are tick-thread-only,
    // so painting/encoding happens outside the lock.
    {
        QMutexLocker locker(&m_frameMutex);

        if (paintBlue) {
            while (!m_frameQueue.isEmpty()) {
                auto qf = m_frameQueue.dequeue();
                av_frame_free(&qf.frame);
            }
        }

        // ALWAYS do jitter pull to keep m_latestFrame fresh, even when
        // not assigned to a view.  This ensures frames are ready the
        // instant this source gets mapped to a view.
        int64_t targetTimeMs = currentRecordingTimeMs - jitterMs - trimMs;
        if (targetTimeMs < 0) targetTimeMs = 0;

        while (!m_frameQueue.isEmpty() && m_frameQueue.head().sourcePts <= targetTimeMs) {
            QueuedFrame top = m_frameQueue.dequeue();
            if (top.carrierToken &&
                top.carrierToken->epoch != m_carrierEpoch.load(std::memory_order_acquire)) {
                av_frame_free(&top.frame);
                continue;
            }
#ifdef OLR_GPU_PIPELINE_BUILD
            if (!top.frame && !top.gpuFrame.isNull() &&
                m_gpuEncodeCpuFallback.load(std::memory_order_acquire)) {
                continue;
            }
#endif
            if (pulled) av_frame_free(&pulled);
            pulled = top.frame;
            pulledTimecode100ns = top.sourceTimecode100ns;
            pulledTimecodeEvidence = std::move(top.timecodeEvidence);
            pulledCarrierToken = std::move(top.carrierToken);
            pulledAnyFrame = true;
#ifdef OLR_GPU_PIPELINE_BUILD
            pulledGpuFrame = top.gpuFrame;
            pulledGpuFenceValue = top.gpuFenceValue;
#endif
        }
    }

    if (paintBlue) {
        if (m_latestFrame && m_latestFrame->data[0]) {
            const size_t yBytes =
                static_cast<size_t>(m_latestFrame->linesize[0]) * m_latestFrame->height;
            const size_t uvBytes =
                static_cast<size_t>(m_latestFrame->linesize[1]) * (m_latestFrame->height / 2);
            memset(m_latestFrame->data[0], 128, yBytes);
            memset(m_latestFrame->data[1], 240,
                   uvBytes); // Cb: 240 = legal max chroma (255 is out-of-range)
            memset(m_latestFrame->data[2], 107,
                   static_cast<size_t>(m_latestFrame->linesize[2]) * (m_latestFrame->height / 2));
        }
        // A blue-painted frame carries no source timecode.
        m_latestFrameTimecode100ns.store(-1, std::memory_order_release);
        m_latestFrameTimecodeEvidence.reset();
        m_latestFrameCarrierToken.reset();
#ifdef OLR_GPU_PIPELINE_BUILD
        m_latestGpuFrame = FrameHandle{};
        m_latestGpuFenceValue = 0;
        m_latestGpuFrameTimecode100ns.store(-1, std::memory_order_release);
        std::atomic_store_explicit(&m_latestGpuFrameTimecodeEvidence,
                                   std::shared_ptr<const TimecodeEvidence>{},
                                   std::memory_order_release);
        m_latestGpuFrameCarrierToken.reset();
#endif
    }
    if (pulledAnyFrame) {
        if (pulled && m_latestFrame) {
            av_frame_unref(m_latestFrame);
            av_frame_move_ref(m_latestFrame, pulled);
            // The TC travels with the frame now held in m_latestFrame.
            m_latestFrameTimecode100ns.store(pulledTimecode100ns, std::memory_order_release);
            m_latestFrameTimecodeEvidence = pulledTimecodeEvidence;
            m_latestFrameCarrierToken = pulledCarrierToken;
        }
        if (pulled) av_frame_free(&pulled);
#ifdef OLR_GPU_PIPELINE_BUILD
        m_latestGpuFrame = std::move(pulledGpuFrame);
        m_latestGpuFenceValue = pulledGpuFenceValue;
        m_latestGpuFrameTimecode100ns.store(m_latestGpuFrame.isNull() ? -1 : pulledTimecode100ns,
                                            std::memory_order_release);
        m_latestGpuFrameCarrierToken = m_latestGpuFrame.isNull()
                                           ? std::shared_ptr<const SourceCarrierToken>{}
                                           : pulledCarrierToken;
        std::atomic_store_explicit(
            &m_latestGpuFrameTimecodeEvidence,
            m_latestGpuFrame.isNull() || !pulledTimecodeEvidence
                ? std::shared_ptr<const TimecodeEvidence>{}
                : std::make_shared<const TimecodeEvidence>(*pulledTimecodeEvidence),
            std::memory_order_release);
#endif
    }

    // Read the current view-track assignment (atomic, set by UIManager).
    // -1 = this source is not assigned to any view, skip encoding.
    track = m_viewTrack.load(std::memory_order_relaxed);

    const bool hasSyntheticCpuLatest =
        !m_latestFrameCarrierToken && !m_latestFrameTimecodeEvidence &&
        m_latestFrameTimecode100ns.load(std::memory_order_acquire) < 0;
    const bool hasCpuLatest =
        m_latestFrame && m_latestFrame->data[0] &&
        (carrierTokenIsCurrent(m_latestFrameCarrierToken) || hasSyntheticCpuLatest);
#ifdef OLR_GPU_PIPELINE_BUILD
    const auto gpuEvidenceForCarrierCheck =
        std::atomic_load_explicit(&m_latestGpuFrameTimecodeEvidence, std::memory_order_acquire);
    const bool hasSyntheticGpuLatest =
        !m_latestGpuFrameCarrierToken && !gpuEvidenceForCarrierCheck &&
        m_latestGpuFrameTimecode100ns.load(std::memory_order_acquire) < 0;
    const bool hasGpuLatest =
        !m_latestGpuFrame.isNull() && m_latestGpuFrame.isGpuBacked() &&
        (carrierTokenIsCurrent(m_latestGpuFrameCarrierToken) || hasSyntheticGpuLatest);
#else
    const bool hasGpuLatest = false;
#endif

    if (track >= 0 && (hasCpuLatest || hasGpuLatest)) {
        bool submittedGpuEncode = false;
#if defined(OLR_GPU_PIPELINE_BUILD)
        if (m_videoCodec == VideoCodecChoice::H264Hardware && m_gpuEncodePump && hasGpuLatest &&
            m_muxer) {
            AVStream* st = m_muxer->getStream(track);
            const int64_t sourceTimecode100ns =
                m_latestGpuFrameTimecode100ns.load(std::memory_order_acquire);
            const auto selectedGpuEvidence = std::atomic_load_explicit(
                &m_latestGpuFrameTimecodeEvidence, std::memory_order_acquire);
            QByteArray metaJson;
            {
                QMutexLocker locker(&m_metadataMutex);
                metaJson = m_sourceMetadataJson;
            }
            if (!m_gpuEncodeCpuFallback.load(std::memory_order_acquire)) {
                const auto submissionToken = m_latestGpuFrameCarrierToken
                                                 ? m_latestGpuFrameCarrierToken
                                                 : snapshotActiveCarrierToken();
                const uint64_t submissionId = acquireEncodeSubmission(
                    true, track, st, nullptr,
                    submissionToken ? *submissionToken : SourceCarrierToken{}, 0, streamTimeMs,
                    metaJson);
                if (submissionId == 0) {
                    qWarning() << "GPU encode callback pool exhausted; switching to CPU fallback";
                    if (submissionToken) tryLatchGpuEncodeCpuFallback(*submissionToken);
                } else {
                    std::optional<TimecodeEvidence> selectedEvidence =
                        selectedGpuEvidence ? std::optional<TimecodeEvidence>(*selectedGpuEvidence)
                                            : std::nullopt;
                    const auto muxEvidence =
                        takeFrameTimecodeEvidenceForMux(selectedEvidence, m_internalFrameCount);
#ifdef OLR_UNIT_TEST
                    runBeforeMuxEvidenceSubmissionForTest();
#endif
                    const MuxFrameEvidenceSubmission evidenceSubmission = enqueueMuxFrameEvidence(
                        m_internalFrameCount, sourceTimecode100ns, muxEvidence, submissionToken,
                        hasSyntheticGpuLatest);
                    if (evidenceSubmission.id == 0 ||
                        !setEncodeSubmissionEvidenceId(submissionId, evidenceSubmission.id)) {
                        if (evidenceSubmission.id != 0)
                            discardMuxFrameEvidence(evidenceSubmission.id);
                        finishEncodeSubmission(submissionId);
                        // A reset can invalidate the carrier between latest-frame
                        // validation and evidence insertion. That stale rejection
                        // must not disable GPU encode for the replacement carrier.
                        if (submissionToken) tryLatchGpuEncodeCpuFallback(*submissionToken);
                    } else {
                        submittedGpuEncode = m_gpuEncodePump->submit(
                            m_latestGpuFrame, m_latestGpuFenceValue, m_internalFrameCount,
                            m_latestGpuFrame.metadata().color,
                            gpuCallbacksForSubmission(submissionId));
                        if (submittedGpuEncode && selectedGpuEvidence) {
                            if (m_latestFrameTimecodeEvidence &&
                                sameTimecodeEvidence(*m_latestFrameTimecodeEvidence,
                                                     *selectedGpuEvidence)) {
                                m_latestFrameTimecodeEvidence.reset();
                            }
                            auto expected = selectedGpuEvidence;
                            std::atomic_compare_exchange_strong_explicit(
                                &m_latestGpuFrameTimecodeEvidence, &expected,
                                std::shared_ptr<const TimecodeEvidence>{},
                                std::memory_order_acq_rel, std::memory_order_acquire);
                        }
                        if (submittedGpuEncode)
                            m_latestGpuFrameTimecode100ns.store(-1, std::memory_order_release);
                    }
                }
            }
        }
#endif

        if (!submittedGpuEncode && hasCpuLatest && m_videoCodec == VideoCodecChoice::H264Hardware &&
            m_nativeEncoder) {
            // H.264 native-encode path: encode via NativeVideoEncoder and write
            // each output packet directly.
            AVStream* st = m_muxer->getStream(track);
            QString encErr;
            const auto submissionToken = m_latestFrameCarrierToken ? m_latestFrameCarrierToken
                                                                   : snapshotActiveCarrierToken();
            const uint64_t submissionId = acquireEncodeSubmission(
                false, track, st, &havePacket,
                submissionToken ? *submissionToken : SourceCarrierToken{}, 0);
            if (submissionId == 0) {
                qWarning() << "Native encode callback pool exhausted; retaining frame evidence";
            } else {
                auto muxEvidence = takeFrameTimecodeEvidenceForMux(m_latestFrameTimecodeEvidence,
                                                                   m_internalFrameCount);
                const int64_t sourceTimecode100ns =
                    m_latestFrameTimecode100ns.load(std::memory_order_acquire);
#ifdef OLR_UNIT_TEST
                runBeforeMuxEvidenceSubmissionForTest();
#endif
                const MuxFrameEvidenceSubmission evidenceSubmission =
                    enqueueMuxFrameEvidence(m_internalFrameCount, sourceTimecode100ns, muxEvidence,
                                            submissionToken, hasSyntheticCpuLatest);
                if (evidenceSubmission.id == 0 ||
                    !setEncodeSubmissionEvidenceId(submissionId, evidenceSubmission.id)) {
                    if (evidenceSubmission.id != 0) discardMuxFrameEvidence(evidenceSubmission.id);
                    finishEncodeSubmission(submissionId);
                    if (muxEvidence && !m_latestFrameTimecodeEvidence)
                        m_latestFrameTimecodeEvidence = std::move(muxEvidence);
                } else {
                    bool encoded = false;
                    {
                        std::lock_guard<std::mutex> encoderLock(m_nativeEncodeMutex);
                        encoded = m_nativeEncoder->encode(
                            m_latestFrame, m_internalFrameCount,
                            bufferedPacketCallbackForSubmission(submissionId), &encErr);
                    }
                    const bool packetBatchValid = encoded && bufferedSubmissionReady(submissionId);
                    if (packetBatchValid) {
                        commitBufferedEncodeSubmission(submissionId);
                        m_latestFrameTimecode100ns.store(-1, std::memory_order_release);
                    } else {
                        finishEncodeSubmission(submissionId);
                        if (encoded) qWarning() << "Native encoder exceeded bounded packet batch";
                        discardMuxFrameEvidence(evidenceSubmission.id);
                        if (muxEvidence && !m_latestFrameTimecodeEvidence)
                            m_latestFrameTimecodeEvidence = std::move(muxEvidence);
                    }
                }
            }
        } else if (!submittedGpuEncode && hasCpuLatest && encCtx) {
            // MPEG-2 software-encode path. Evidence is registered against the
            // input frame PTS and recovered by the encoder's actual output packet
            // PTS before rescaling; delayed/B-frame output cannot consume a newer
            // tick's evidence.
            // Set PTS on the FRAME, not the packet (avcodec_receive_packet
            // overwrites the packet entirely).
            m_latestFrame->pts = m_internalFrameCount;

            auto selectedEvidence = takeFrameTimecodeEvidenceForMux(m_latestFrameTimecodeEvidence,
                                                                    m_internalFrameCount);
            const int64_t sourceTimecode100ns =
                m_latestFrameTimecode100ns.load(std::memory_order_acquire);
#ifdef OLR_UNIT_TEST
            runBeforeMuxEvidenceSubmissionForTest();
#endif
            const auto submissionToken = m_latestFrameCarrierToken ? m_latestFrameCarrierToken
                                                                   : snapshotActiveCarrierToken();
            const MuxFrameEvidenceSubmission evidenceSubmission =
                enqueueMuxFrameEvidence(m_internalFrameCount, sourceTimecode100ns, selectedEvidence,
                                        submissionToken, hasSyntheticCpuLatest);
            if (evidenceSubmission.id == 0) {
                if (selectedEvidence && !m_latestFrameTimecodeEvidence)
                    m_latestFrameTimecodeEvidence = std::move(selectedEvidence);
            } else {
                const int sendResult = avcodec_send_frame(encCtx, m_latestFrame);
                if (sendResult == 0) {
                    m_latestFrameTimecode100ns.store(-1, std::memory_order_release);
                    if (avcodec_receive_packet(encCtx, outPkt) == 0) {
                        softwareCompletionId = reserveMuxCompletion(0);
                        if (softwareCompletionId == 0) {
                            qWarning()
                                << "Mux completion pool exhausted; retaining packet evidence";
                        } else {
                            softwareFrameEvidence = takeMuxFrameEvidence(outPkt->pts);
                        }
                        // As with native/GPU output, an evidence-free frame still has
                        // a mapping. If reset removed it, this delayed packet belongs
                        // to an obsolete carrier and must not reach the muxer.
                        if (softwareFrameEvidence) {
                            outPkt->stream_index = track;
                            outPkt->duration = 1;
                            AVStream* st = m_muxer->getStream(track);
                            if (st) {
                                av_packet_rescale_ts(outPkt, encCtx->time_base, st->time_base);
                                havePacket = true;
                            }
                        }
                        if (!havePacket && softwareCompletionId != 0) {
                            releaseMuxCompletionReservation(softwareCompletionId);
                            softwareCompletionId = 0;
                        }
                    }
                } else {
                    discardMuxFrameEvidence(evidenceSubmission.id);
                    if (selectedEvidence && !m_latestFrameTimecodeEvidence)
                        m_latestFrameTimecodeEvidence = std::move(selectedEvidence);
                }
            }
        }
    }

    if (havePacket) {
        // For MPEG-2, the packet is in outPkt and has not been written yet.
        // For H.264, packets were written inline in the callback above.
        if (m_videoCodec != VideoCodecChoice::H264Hardware && encCtx) {
            std::optional<TimecodeEvidence> muxEvidence;
            if (softwareFrameEvidence)
                muxEvidence = std::move(softwareFrameEvidence->timecodeEvidence);
            const uint64_t evidenceEpoch =
                softwareFrameEvidence ? softwareFrameEvidence->carrierGeneration : 0;
            const SourceCarrierToken evidenceToken =
                softwareFrameEvidence
                    ? SourceCarrierToken{softwareFrameEvidence->carrierSessionIdentity,
                                         softwareFrameEvidence->carrierGeneration}
                    : SourceCarrierToken{};
            QString startTimecodeCandidate;
            if (softwareFrameEvidence && softwareFrameEvidence->sourceTimecode100ns >= 0) {
                const Smpte12mTimecode startTc = Smpte12m::from100ns(
                    softwareFrameEvidence->sourceTimecode100ns, Smpte12m::kTimecodeNominalFps);
                char buf[12];
                startTimecodeCandidate = QString::fromLatin1(Smpte12m::format(startTc, buf));
            }
#ifdef OLR_UNIT_TEST
            runBeforeMuxPacketWriteForTest();
#endif
            if (evidenceEpoch > 0 && !carrierTokenIsCurrent(evidenceToken)) {
                havePacket = false;
                releaseMuxCompletionReservation(softwareCompletionId);
                softwareCompletionId = 0;
            } else if (!populateMuxCompletion(softwareCompletionId, evidenceToken,
                                              std::move(muxEvidence))) {
                havePacket = false;
                releaseMuxCompletionReservation(softwareCompletionId);
                softwareCompletionId = 0;
            } else {
                const bool accepted =
                    m_muxer->writePacket(outPkt, muxCompletionCallback(softwareCompletionId),
                                         startTimecodeCandidate, packetCarrierGuard(evidenceToken));
                softwareCompletionId = 0; // The callback owns the populated slot, even on reject.
                if (accepted) {
                    m_latestFrameTimecode100ns.store(-1, std::memory_order_release);
                } else {
                    havePacket = false;
                }
            }
        }

        // Write the per-frame source metadata to the paired subtitle track
        QByteArray metaJson;
        {
            QMutexLocker locker(&m_metadataMutex);
            metaJson = m_sourceMetadataJson;
        }
        if (havePacket && !metaJson.isEmpty()) {
            m_muxer->writeMetadataPacket(track, streamTimeMs, metaJson);
        }
    }
    if (softwareCompletionId != 0) releaseMuxCompletionReservation(softwareCompletionId);
    av_packet_free(&outPkt);

    // Write this tick's worth of audio for the assigned view track
    // (sample-accurate cursor, silence-filled where capture had nothing).
    writeAudioForTick(currentRecordingTimeMs, track, trimMs, jitterMs);
}

void StreamWorker::captureLoop() {
    while (m_captureRunning) {
        // If a restart was requested (e.g. changeSource), acknowledge it
        // and loop back to re-read the URL instead of exiting.
        m_restartCapture = 0;

        QString currentUrl;
        {
            QMutexLocker locker(&m_urlMutex);
            currentUrl = m_url;
        }

        {
            // Right-size the jitter window for this source's transport. SRT
            // pre-buffers via TSBPD, so it needs only a small floor.
            int srtFloor = kSrtJitterFloorMs;
            const int envFloor = qEnvironmentVariableIntValue("OLR_SRT_JITTER_MS");
            if (envFloor > 0) srtFloor = envFloor;
            m_activeJitterWindowMs.store(
                jitterWindowMs(QUrl(currentUrl).scheme().toLower(), srtFloor, kJitterBufferMs),
                std::memory_order_relaxed);
        }

        // If URL is empty, don't attempt to connect. Just idle until
        // a new URL is set via changeSource() which sets m_restartCapture.
        if (currentUrl.trimmed().isEmpty()) {
            setConnected(false);
            while (m_captureRunning && !m_restartCapture) {
                QThread::msleep(100);
            }
            continue;
        }

        qDebug() << "Source" << m_sourceIndex
                 << "Attempting connection to:" << RtmpUrlParts::redactedForLog(QUrl(currentUrl));
        setConnected(false);
        const uint64_t captureSessionIdentity = beginCaptureSession();
        const auto endSession = qScopeGuard(
            [this, captureSessionIdentity] { endCaptureSession(captureSessionIdentity); });

        IngestCallbacks callbacks;
        callbacks.shouldStop = [this]() {
            return !m_captureRunning.load(std::memory_order_relaxed) ||
                   m_restartCapture.loadRelaxed() != 0;
        };
        callbacks.recordingClockMs = [this]() -> int64_t {
            return m_sharedClock ? m_sharedClock->elapsedMs() : -1;
        };
        callbacks.logInfo = [this](const QString& message) {
            qDebug() << "Source" << m_sourceIndex << message;
        };
#if defined(OLR_GPU_PIPELINE_BUILD)
        callbacks.preferGpuVideoFrames = preferGpuVideoFramesForIngest();
        callbacks.shouldPreferGpuVideoFrames = [this]() { return preferGpuVideoFramesForIngest(); };
        callbacks.importGpuVideoFrame = [this,
                                         captureSessionIdentity](void* nativeDecodedImage,
                                                                 const FrameMetadata& metadata) {
            return importGpuVideoFrameForSession(captureSessionIdentity, nativeDecodedImage,
                                                 metadata);
        };
#endif
        callbacks.onVideoFrame = [this, captureSessionIdentity](DecodedVideoFrame decoded) {
            enqueueDecodedVideoFrameForSession(std::move(decoded), captureSessionIdentity);
        };
        callbacks.onAudioChunk = [this, captureSessionIdentity](DecodedAudioChunk chunk) {
            const qsizetype sampleCount = chunk.pcmS16Stereo.size() / kAudioBytesPerSample;
            if (sampleCount > std::numeric_limits<int>::max()) {
                return;
            }
            enqueueAudioForSession(captureSessionIdentity, chunk.startSample,
                                   reinterpret_cast<const uint8_t*>(chunk.pcmS16Stereo.constData()),
                                   static_cast<int>(sampleCount));
        };
        callbacks.setConnected = [this, captureSessionIdentity](bool connected) {
            setConnectedForSession(captureSessionIdentity, connected);
        };
        callbacks.reportStats = [this, captureSessionIdentity](const IngestStats& stats) {
            reportStatsForSession(captureSessionIdentity, stats);
        };

        const QUrl sourceUrl(currentUrl);
        bool nativeSrtAvailable = false;
#if defined(OLR_NATIVE_SRT_AVAILABLE)
        nativeSrtAvailable = NativeSrtIngestSession::supportsUrl(sourceUrl);
#endif
        bool nativeRtmpAvailable = false;
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
        nativeRtmpAvailable = NativeRtmpIngestSession::supportsUrl(sourceUrl);
#endif
        const bool nativeNdiAvailable = NativeNdiIngestSession::supportsUrl(sourceUrl) &&
                                        NativeNdiIngestSession::runtimeAvailable();
        IngestBackendOptions backendOptions = ingestBackendOptionsFromEnvironment(
            sourceUrl, nativeSrtAvailable, nativeRtmpAvailable, nativeNdiAvailable);
        const IngestBackendKind backendKind = selectIngestBackend(sourceUrl, backendOptions);
        const bool nativeRtmpAttempt = backendKind == IngestBackendKind::NativeRtmp;
        if (m_clockOwnerUrl != currentUrl || m_clockOwnerBackend != backendKind) {
            m_srtSourceClock.reset();
            m_rtmpSourceClock.reset();
            m_ndiSourceClock.reset();
            m_clockOwnerUrl = currentUrl;
            m_clockOwnerBackend = backendKind;
        }

        std::unique_ptr<IngestSession> session;
#if defined(OLR_NATIVE_SRT_AVAILABLE)
        if (backendKind == IngestBackendKind::NativeSrt) {
            session = std::make_unique<NativeSrtIngestSession>(
                m_sourceIndex, m_targetWidth, m_targetHeight, &m_captureRunning, &m_srtSourceClock);
        }
#endif
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
        if (backendKind == IngestBackendKind::NativeRtmp) {
            session = std::make_unique<NativeRtmpIngestSession>(m_sourceIndex, m_targetWidth,
                                                                m_targetHeight, &m_captureRunning,
                                                                &m_rtmpSourceClock);
        }
#endif
        if (backendKind == IngestBackendKind::NativeNdi) {
            session = std::make_unique<NativeNdiIngestSession>(
                m_sourceIndex, m_targetWidth, m_targetHeight, &m_captureRunning, &m_ndiSourceClock);
        }
        if (!session) {
            const QString scheme = sourceUrl.scheme().toLower();
            if (scheme == QStringLiteral("srt") || scheme == QStringLiteral("rtmp") ||
                scheme == QStringLiteral("rtmps") || scheme == QStringLiteral("ndi")) {
                qWarning() << "Source" << m_sourceIndex << "native" << scheme
                           << "ingest is unavailable for this URL - the native backend does not"
                           << "support these URL options (for example SRT encryption or listener"
                           << "mode), the NDI runtime is missing, or it is not"
                           << "built on this platform. Source disabled.";
            } else {
                qWarning() << "Source" << m_sourceIndex << "unsupported ingest scheme" << scheme
                           << "- OpenLiveReplay ingests only srt://, rtmp://, rtmps://, ndi:";
            }
            setConnectedForSession(captureSessionIdentity, false);
            m_captureRunning = false;
            break;
        }

        {
            QMutexLocker locker(&m_sessionMutex);
            m_activeSession = session.get();
        }

        if (!session->open(sourceUrl, callbacks)) {
            {
                QMutexLocker locker(&m_sessionMutex);
                if (m_activeSession == session.get()) {
                    m_activeSession = nullptr;
                }
            }
            const IngestFailureKind failureKind = session->lastFailureKind();
            if (nativeRtmpAttempt && shouldStopNativeRtmpAfterFailure(failureKind)) {
                qDebug() << "Source" << m_sourceIndex << "Native RTMP failed with"
                         << ingestFailureKindForLog(failureKind)
                         << "; stopping capture for this URL.";
                m_captureRunning = false;
                break;
            } else {
                qDebug() << "Source" << m_sourceIndex << "Connect failed. Retrying in"
                         << (m_connectBackoffMs / 1000.0) << "s...";
            }
            const int steps = qMax(1, m_connectBackoffMs / 100);
            for (int i = 0; i < steps && m_captureRunning && !m_restartCapture; ++i) {
                QThread::msleep(100);
            }
            if (!m_restartCapture) {
                m_connectBackoffMs = qMin(10000, m_connectBackoffMs * 2);
            }
            continue;
        }
        setConnectedForSession(captureSessionIdentity, true);
        m_connectBackoffMs = 1000;
        m_lastFrameEnqueueAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
        // A real source connected: stragglers from any previously-cleared
        // source are gone, so resume enqueuing frames for this new URL.
        m_suppressEnqueue.store(false, std::memory_order_relaxed);

        if (!m_sharedClock) {
            qDebug() << "Source" << m_sourceIndex << "No shared clock. Restarting...";
            m_restartCapture = 1;
            m_captureRunning = false;
            setConnectedForSession(captureSessionIdentity, false);
            break;
        }

        session->run();
        {
            QMutexLocker locker(&m_sessionMutex);
            if (m_activeSession == session.get()) {
                m_activeSession = nullptr;
            }
        }

        setConnectedForSession(captureSessionIdentity, false);
        const IngestFailureKind failureKind = session->lastFailureKind();
        if (nativeRtmpAttempt && shouldStopNativeRtmpAfterFailure(failureKind)) {
            qDebug() << "Source" << m_sourceIndex << "Native RTMP failed with"
                     << ingestFailureKindForLog(failureKind) << "; stopping capture for this URL.";
            m_captureRunning = false;
            break;
        }
    }

    m_captureRunning = false;
}

bool StreamWorker::setupEncoder(AVCodecContext** encCtx) {
    if (m_videoCodec == VideoCodecChoice::H264Hardware) {
        QString err;
        m_nativeEncoder = NativeVideoEncoder::create(
            {m_targetWidth, m_targetHeight, m_targetFpsNum, m_targetFpsDen, 30'000'000}, &err);
        if (!m_nativeEncoder) {
            qWarning() << "Source" << m_sourceIndex
                       << "H.264 hardware encoder unavailable (hardware-only):" << err;
            return false;
        }
#if defined(OLR_GPU_PIPELINE_BUILD)
        if (preferGpuVideoFramesForIngest()) {
            ensureGpuEncodePumpStarted();
        }
#endif
        // Allocate the reusable frame buffer (same as MPEG-2 path).
        m_latestFrame = av_frame_alloc();
        if (!m_latestFrame) return false;
        m_latestFrame->format = AV_PIX_FMT_YUV420P;
        m_latestFrame->width = m_targetWidth;
        m_latestFrame->height = m_targetHeight;
        if (av_frame_get_buffer(m_latestFrame, 0) < 0) {
            av_frame_free(&m_latestFrame);
            return false;
        }
        memset(m_latestFrame->data[0], 128,
               static_cast<size_t>(m_latestFrame->linesize[0]) * m_latestFrame->height);
        memset(m_latestFrame->data[1], 128,
               static_cast<size_t>(m_latestFrame->linesize[1]) * (m_latestFrame->height / 2));
        memset(m_latestFrame->data[2], 128,
               static_cast<size_t>(m_latestFrame->linesize[2]) * (m_latestFrame->height / 2));
        // Leave *encCtx null — H.264 path uses m_nativeEncoder.
        return true;
    }
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!encoder) return false;

    // MPEG-2's 12-bit horizontal_size_value/vertical_size_value cannot encode a
    // dimension that is a multiple of 4096 (the field would be 0).  avcodec_open2
    // would otherwise fail with a cryptic message AFTER the muxer header is
    // already on disk, leaving a stub .mkv.  Fail early with a clear diagnostic.
    if (m_targetWidth % 4096 == 0 || m_targetHeight % 4096 == 0) {
        qWarning() << "Source" << m_sourceIndex
                   << "MPEG-2 cannot encode a dimension that is a multiple of 4096"
                   << "(" << m_targetWidth << "x" << m_targetHeight
                   << ") — pick e.g. 3840x2160 instead of 4096x2160.";
        return false;
    }

    *encCtx = avcodec_alloc_context3(encoder);
    if (!*encCtx) return false;

    (*encCtx)->width = m_targetWidth;
    (*encCtx)->height = m_targetHeight;

    // MPEG-2 can only signal a small set of frame rates in its sequence header:
    // the integer 24/25/30/50/60 and the 1000/1001 variants (24000/1001,
    // 30000/1001, 60000/1001). For any other rate the encoder silently writes the
    // NEAREST representable rate into the elementary stream, so the file would
    // carry contradictory rates and ES-rate-trusting tools mis-time the video.
    // Warn so the operator can pick a standard rate.
    const bool representable =
        (m_targetFpsDen == 1 &&
         (m_targetFpsNum == 24 || m_targetFpsNum == 25 || m_targetFpsNum == 30 ||
          m_targetFpsNum == 50 || m_targetFpsNum == 60)) ||
        (m_targetFpsDen == 1001 &&
         (m_targetFpsNum == 24000 || m_targetFpsNum == 30000 || m_targetFpsNum == 60000));
    if (!representable) {
        qWarning() << "Source" << m_sourceIndex << "rate" << m_targetFpsNum << "/" << m_targetFpsDen
                   << "is not an exact MPEG-2 rate; the elementary stream will"
                   << "carry the nearest representable rate (use 24/25/30/50/60 or"
                   << "their 1000/1001 variants to avoid a container/ES rate mismatch).";
    }

    // The coding time_base stays on the integer-fps ms grid: the output packet PTS
    // is rescaled from this time_base to the muxer (av_packet_rescale_ts below), and
    // it MUST track the integer-fps audio/metadata cadence (m_targetFps), so the
    // muxed video does not drift against audio for 29.97/59.94. The TRUE rational
    // rate is signalled only via framerate (the field mpeg2video writes into the
    // sequence-header frame_rate_code) and via the container avg/r_frame_rate; it
    // must NOT leak into the coded PTS.
    (*encCtx)->time_base = {1, m_targetFps}; // Integer-fps coding clock (ms-anchored)
    (*encCtx)->framerate = {m_targetFpsNum, m_targetFpsDen}; // True rational ES rate

    (*encCtx)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*encCtx)->gop_size = 1; // Keep Intra-only for seeking
    (*encCtx)->bit_rate = 30000000;

    m_latestFrame = av_frame_alloc();
    if (!m_latestFrame) return false;
    m_latestFrame->format = AV_PIX_FMT_YUV420P;
    m_latestFrame->width = m_targetWidth;
    m_latestFrame->height = m_targetHeight;
    if (av_frame_get_buffer(m_latestFrame, 0) < 0) return false; // Allocate actual pixel memory

    // Paint blue frame
    // Y plane (Brightness) - set to medium
    memset(m_latestFrame->data[0], 128, m_latestFrame->linesize[0] * m_latestFrame->height);
    // U plane (Blue Chrominance) - set to max
    memset(m_latestFrame->data[1], 255, m_latestFrame->linesize[1] * (m_latestFrame->height / 2));
    // V plane (Red Chrominance) - set to low
    memset(m_latestFrame->data[2], 107, m_latestFrame->linesize[2] * (m_latestFrame->height / 2));

    return avcodec_open2(*encCtx, encoder, nullptr) >= 0;
}

void StreamWorker::changeSource(const QString& newUrl) {
    {
        QMutexLocker locker(&m_urlMutex);
        if (m_url == newUrl) return; // No change
        m_url = newUrl;
    }
    rotateCarrier(false);

    if (newUrl.trimmed().isEmpty()) {
        m_paintBlue = 1;
        // Suppress capture-side enqueues until a real (non-empty) URL
        // connects again.  This stops a late straggler — a frame already
        // decoded from the now-cleared source — from being enqueued after
        // the tick clears the queue and paints blue, which would otherwise
        // overwrite the blue frame and then re-encode that stale frame
        // every tick forever (empty URL produces no fresh frames).
        m_suppressEnqueue.store(true, std::memory_order_relaxed);
    }

    m_restartCapture = 1;
}

// ─── Audio FIFO ─────────────────────────────────────────────────────────

void StreamWorker::enqueueAudio(int64_t startSample, const uint8_t* data, int numSamples) {
    if (numSamples <= 0) return;
    const qint64 numBytes = qint64(numSamples) * kAudioBytesPerSample;
    QMutexLocker locker(&m_audioFifoMutex);

    if (m_audioFifoStartSample < 0 || m_audioFifo.isEmpty()) {
        if (startSample < 0) return; // continuation data with no stream yet
        m_audioFifoStartSample = startSample;
        m_audioFifo.append(reinterpret_cast<const char*>(data), numBytes);
    } else {
        const int64_t expected = m_audioFifoStartSample + m_audioFifo.size() / kAudioBytesPerSample;
        const int64_t delta = (startSample < 0) ? 0 : startSample - expected;
        const int64_t jitterTol = kAudioSampleRate / 100; // 10 ms

        if (qAbs(delta) <= jitterTol) {
            // Continuous (within PTS rounding jitter): plain append
            m_audioFifo.append(reinterpret_cast<const char*>(data), numBytes);
        } else if (delta > 0) {
            // Gap (packet loss / reconnect): zero-fill so the track
            // stays sample-contiguous
            const int64_t maxFill = int64_t(kAudioSampleRate) * 10;
            if (delta > maxFill) {
                // Huge jump: restart the FIFO at the new position
                m_audioFifo.clear();
                m_audioFifoStartSample = startSample;
            } else {
                m_audioFifo.append(QByteArray(int(delta * kAudioBytesPerSample), '\0'));
            }
            m_audioFifo.append(reinterpret_cast<const char*>(data), numBytes);
        } else {
            // Overlap: drop the part we already have
            const int64_t drop = -delta;
            if (drop >= numSamples) return;
            m_audioFifo.append(reinterpret_cast<const char*>(data) + drop * kAudioBytesPerSample,
                               numBytes - drop * kAudioBytesPerSample);
        }
    }

    // Cap the FIFO at ~10 s
    const int maxBytes = kAudioSampleRate * 10 * kAudioBytesPerSample;
    if (m_audioFifo.size() > maxBytes) {
        const qsizetype excess = m_audioFifo.size() - maxBytes;
        m_audioFifo.remove(0, excess);
        m_audioFifoStartSample += excess / kAudioBytesPerSample;
    }
}

void StreamWorker::enqueueAudioForSession(uint64_t sessionIdentity, int64_t startSample,
                                          const uint8_t* data, int numSamples) {
    std::lock_guard<std::mutex> epochLock(m_epochMutex);
    if (sessionIdentity == 0 || sessionIdentity != m_activeCaptureSessionIdentity) return;
    enqueueAudio(startSample, data, numSamples);
}

void StreamWorker::writeAudioForTick(int64_t recordingTimeMs, int track, int64_t trimMs,
                                     int64_t jitterMs) {
    // Audio shares the video path's jitter delay so both land on the
    // same timeline: a video frame written at file-time T shows source
    // content from T - jitter.  The cursor runs on the FILE timeline;
    // the FIFO holds source-timeline samples, so file position P maps
    // to FIFO position P - jitter.
    const int64_t jitterSamples = jitterMs * kAudioSampleRate / 1000;
    const int64_t trimSamples = trimMs * kAudioSampleRate / 1000;
    const int64_t targetEnd = recordingTimeMs * kAudioSampleRate / 1000;
    if (targetEnd <= 0) return;

    if (track < 0) {
        // Not mapped to a view: discard consumed FIFO data and keep the
        // cursor pinned to "now" so mapping in resumes at current time.
        m_audioWriteCursor = targetEnd;
        m_audioSourceCursor = targetEnd - jitterSamples - trimSamples;
        m_audioServoTrimSamples = trimSamples;
        m_audioServoJitterSamples = jitterSamples;
        QMutexLocker locker(&m_audioFifoMutex);
        if (m_audioFifoStartSample >= 0) {
            const int64_t dropSamples = m_audioSourceCursor - m_audioFifoStartSample;
            if (dropSamples > 0) {
                const int dropBytes =
                    int(qMin<int64_t>(dropSamples * kAudioBytesPerSample, m_audioFifo.size()));
                m_audioFifo.remove(0, dropBytes);
                m_audioFifoStartSample += dropBytes / kAudioBytesPerSample;
            }
        }
        return;
    }

    if (m_audioWriteCursor < 0) {
        m_audioWriteCursor = qMax<int64_t>(0, targetEnd - kAudioSampleRate / m_targetFps);
    }
    if (targetEnd <= m_audioWriteCursor) return;

    // Catch up at most 1 s per tick: the track stays contiguous, a large
    // backlog (stalled event loop) just drains over several ticks.
    const int64_t n = qMin<int64_t>(targetEnd - m_audioWriteCursor, kAudioSampleRate);
    const int64_t start = m_audioWriteCursor;                            // file timeline
    const int64_t nominalSrcStart = start - jitterSamples - trimSamples; // source timeline
    if (m_audioSourceCursor < 0 || m_audioServoTrimSamples != trimSamples ||
        m_audioServoJitterSamples != jitterSamples) {
        m_audioSourceCursor = nominalSrcStart;
        m_audioServoTrimSamples = trimSamples;
        m_audioServoJitterSamples = jitterSamples;
    }
    const int64_t srcStart = m_audioSourceCursor;
    const int64_t srcAdvance = n;

    QByteArray chunk(int(n * kAudioBytesPerSample), '\0');
    {
        QMutexLocker locker(&m_audioFifoMutex);
        if (m_audioFifoStartSample >= 0 && !m_audioFifo.isEmpty()) {
            const int64_t fifoStart = m_audioFifoStartSample;
            const int64_t fifoEnd = fifoStart + m_audioFifo.size() / kAudioBytesPerSample;
            const int64_t copyFrom = qMax(srcStart, fifoStart);
            const int64_t copyTo = qMin(srcStart + n, fifoEnd);
            if (copyTo > copyFrom) {
                memcpy(chunk.data() + (copyFrom - srcStart) * kAudioBytesPerSample,
                       m_audioFifo.constData() + (copyFrom - fifoStart) * kAudioBytesPerSample,
                       size_t((copyTo - copyFrom) * kAudioBytesPerSample));
            }
            // Trim everything we just consumed (or skipped past)
            const int64_t dropSamples = (srcStart + srcAdvance) - fifoStart;
            if (dropSamples > 0) {
                const int dropBytes =
                    int(qMin<int64_t>(dropSamples * kAudioBytesPerSample, m_audioFifo.size()));
                m_audioFifo.remove(0, dropBytes);
                m_audioFifoStartSample += dropBytes / kAudioBytesPerSample;
            }
        }
    }
    m_audioSourceCursor = srcStart + srcAdvance;
    m_audioWriteCursor = start + n;

    const int audioTrackIdx = m_muxer->audioTrackOffset() + track;
    AVStream* st = m_muxer->getStream(audioTrackIdx);
    if (!st) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;
    if (chunk.size() <= std::numeric_limits<int>::max() &&
        av_new_packet(pkt, static_cast<int>(chunk.size())) == 0) {
        memcpy(pkt->data, chunk.constData(), static_cast<size_t>(chunk.size()));
        pkt->stream_index = audioTrackIdx;
        pkt->pts = av_rescale_q(start, {1, kAudioSampleRate}, st->time_base);
        pkt->dts = pkt->pts;
        pkt->duration = av_rescale_q(n, {1, kAudioSampleRate}, st->time_base);
        m_muxer->writePacket(pkt);
    }
    av_packet_free(&pkt);
}

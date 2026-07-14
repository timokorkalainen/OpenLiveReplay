#ifndef NATIVERTMPINGESTSESSION_H
#define NATIVERTMPINGESTSESSION_H

#include "nativeaacdecoder.h"
#include "decodedframeevidencequeue.h"
#include "h26xaccessunit.h"
#include "h26xseitimecode.h"
#include "ingestsession.h"
#include "nativevideodecoder.h"
#include "rtmpprotocol.h"
#include "recorder_engine/timing/sourceclock.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QUrl>

#include <atomic>
#include <cstdint>
#include <memory>

class QTcpSocket;

class NativeRtmpIngestSession final : public IngestSession {
#if defined(QT_TESTLIB_LIB)
    friend class TestIngestBackendSelector;
    friend class TestIngestTimecodeEvidence;
#endif
public:
    NativeRtmpIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                            std::atomic<bool>* captureRunning);
    NativeRtmpIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                            std::atomic<bool>* captureRunning, AnchoredSourceClock* sourceClock);
    ~NativeRtmpIngestSession() override;

    static bool supportsUrl(const QUrl& url);

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;
    IngestFailureKind lastFailureKind() const override { return m_lastFailureKind; }

    // Nominal fps used only to convert an extracted/AMF SMPTE 12M timecode into a
    // 100 ns offset since midnight. RTMP/FLV carries no fps on this path. This is an
    // ALIAS of the shared Smpte12m::kTimecodeNominalFps so SRT and RTMP producers and
    // the TimecodeAligner consumer reference ONE source of truth and provably agree.
    // This affects only the TC mapping — A/V sync uses the FLV PLL clock, never this.
    static constexpr int kTimecodeNominalFps = Smpte12m::kTimecodeNominalFps;

private:
    int m_sourceIndex = -1;
    int m_outputWidth = 1920;
    int m_outputHeight = 1080;
    std::atomic<bool>* m_captureRunning = nullptr;

    std::atomic<bool> m_stopRequested{false};
    QUrl m_url;
    IngestCallbacks m_callbacks;
    QElapsedTimer m_monotonic;
    std::unique_ptr<QTcpSocket> m_socket;
    std::unique_ptr<NativeVideoDecoder> m_videoDecoder;
    std::unique_ptr<NativeAacDecoder> m_audioDecoder;
    RtmpAvcConfig m_avcConfig;
    RtmpHevcConfig m_hevcConfig;
    RtmpAacConfig m_aacConfig;
    NativeVideoCodec m_videoCodec = NativeVideoCodec::Unknown;
    QByteArray m_activeVideoConfiguration;
    bool m_keepSurfaceDecodeActive = false;
    int m_outputChunkSize = 128;
    int m_streamId = 1;
    AnchoredSourceClock m_ownedClock{ClockQuality::FlvPll};
    AnchoredSourceClock* m_clock = &m_ownedClock;
    bool m_externalClock = false;
    uint64_t m_sourceGeneration = 0;
    H26xTimingContext m_timingContext;
    H26xSeiTimecodeState m_timecodeState;
    int64_t m_pendingVideoTimecode100ns = -1;
    std::optional<TimecodeEvidence> m_pendingTimecodeEvidence;
    DecodedFrameEvidenceQueue m_decodedFrameEvidence;
    int64_t m_amfTimecode100ns = -1;
    int64_t m_amfFrameOfDay = -1;
    FrameRateQ m_amfFrameRate;
    uint64_t m_amfTimingGeneration = 0;
    int64_t m_amfAnchorPtsMs = -1;
    int64_t m_amfLastPtsMs = -1;
    int64_t m_amfLastFrameOfDay = -1;
    QByteArray m_lastAmfMetadataPayload;
    bool m_hasAppliedAmfMetadata = false;
    uint64_t m_amfMetadataParseCount = 0;
    uint64_t m_amfMetadataApplyCount = 0;
    int64_t m_prevAudioPtsMs = -1;
    int64_t m_lastPacketAtMs = -1;
    int64_t m_lastKeyframeAtMs = -1;
    int64_t m_lastStatsAtMs = -1;
    quint64 m_decodeFailures = 0;
    quint64 m_receivedChunkBytes = 0;
    quint64 m_nextAcknowledgementAt = 0;
    quint32 m_acknowledgementWindowSize = 0;
    bool m_seenSupportedVideo = false;
    bool m_seenSupportedAudio = false;
    bool m_reconnectRequested = false;
    int64_t m_openedAtMs = -1;
    QString m_unsupportedReason;
    IngestFailureKind m_lastFailureKind = IngestFailureKind::None;
    RtmpChunkParser m_chunkParser;
    QList<RtmpMessage> m_pendingMessages;

    bool connectAndPlay(QString* error);
    void closeSocket();
    bool performHandshake(QString* error);
    bool sendConnectCommand(QString* error);
    static RtmpConnectCodecProfile connectCodecProfile();
    bool waitForCommandResult(double transactionId, RtmpMessage* result, QString* error);
    bool sendCreateStreamCommand(QString* error);
    bool sendPlayCommand(QString* error);
    bool readMessage(RtmpMessage* message, QString* error);
    bool sendMessage(int chunkStreamId, int messageType, int messageStreamId, qint64 timestampMs,
                     const QByteArray& payload, QString* error);
    bool readFully(char* data, qsizetype size, QString* error);
    bool writeFully(const QByteArray& bytes, QString* error);
    void configureAcknowledgementWindow(quint32 windowSize);
    bool noteIncomingChunkBytes(qint64 byteCount, quint32* acknowledgementSequence);
    bool acknowledgeIncomingBytes(qint64 byteCount, QString* error);
    bool shouldStop() const;
    void log(const QString& message) const;
    void maybeReportStats();
    void processMessage(const RtmpMessage& message);
    void processVideoMessage(qint64 timestampMs, const QByteArray& payload);
    void processAudioMessage(qint64 timestampMs, const QByteArray& payload);
    void resetVideoState();
    bool parseAvcSequenceHeader(const QByteArray& payload, QString* error);
    bool parseAacSequenceHeader(const QByteArray& payload, QString* error);
    int64_t sourcePtsMsForVideo(qint64 dtsMs, qint64 ptsMs);
    int64_t sourcePtsMsForAudio(qint64 ptsMs);
    // Build one frame-local observation from standard SEI or, when no stronger
    // codec rate exists, the validated advancing metadata anchor.
    void updatePendingVideoTimecode(const QByteArray& annexB, NativeVideoCodec codec,
                                    int64_t sourcePtsMs = 0, int64_t presentationPtsMs = -1);
    // Legacy recording-tag seam; alignment evidence requires applyAmfMetadata().
    void applyAmfTimecodeString(const QString& text);
    void applyAmfMetadata(const QString& timecode, double frameRate);
};

#endif // NATIVERTMPINGESTSESSION_H

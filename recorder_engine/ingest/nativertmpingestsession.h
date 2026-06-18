#ifndef NATIVERTMPINGESTSESSION_H
#define NATIVERTMPINGESTSESSION_H

#include "nativeaacdecoder.h"
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
    // 100 ns offset since midnight. RTMP/FLV carries no fps on this path; 30 matches
    // the engine's default target rate and the SRT session's kTimecodeNominalFps so
    // SRT and RTMP producers agree (see NativeSrtIngestSession::kTimecodeNominalFps).
    // This affects only the TC mapping — A/V sync uses the FLV PLL clock, never this.
    static constexpr int kTimecodeNominalFps = 30;

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
    int m_outputChunkSize = 128;
    int m_streamId = 1;
    AnchoredSourceClock m_ownedClock{ClockQuality::FlvPll};
    AnchoredSourceClock* m_clock = &m_ownedClock;
    bool m_externalClock = false;
    // SMPTE 12M timecode (100 ns since midnight) stamped onto the emitted
    // DecodedVideoFrame. -1 = the current frame carries no timecode (the common
    // case). Reset per access unit (to the AMF fallback, or -1) so an SEI TC never
    // bleeds across frames; an SEI TC overrides the AMF fallback for its frame.
    int64_t m_pendingVideoTimecode100ns = -1;
    // AMF onMetaData timecode (100 ns since midnight), -1 = none. A sticky fallback
    // used for frames whose access unit carries no SEI timecode, until the next SEI
    // TC appears. Set best-effort from a malformed-tolerant string parse.
    int64_t m_amfTimecode100ns = -1;
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
    // Reset m_pendingVideoTimecode100ns to the AMF fallback (m_amfTimecode100ns, or
    // -1), then (if the access unit carries a SMPTE 12M SEI) overwrite it with that
    // TC. Called once per access unit so an SEI TC never bleeds across frames and an
    // SEI TC always wins over the AMF fallback for its own frame.
    void updatePendingVideoTimecode(const QByteArray& annexB, NativeVideoCodec codec);
    // Parse an AMF onMetaData timecode string (best-effort, malformed -> ignored)
    // and remember it as the sticky AMF fallback (m_amfTimecode100ns).
    void applyAmfTimecodeString(const QString& text);
};

#endif // NATIVERTMPINGESTSESSION_H

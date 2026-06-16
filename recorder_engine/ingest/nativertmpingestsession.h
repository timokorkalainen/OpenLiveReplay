#ifndef NATIVERTMPINGESTSESSION_H
#define NATIVERTMPINGESTSESSION_H

#include "audiotoolboxaacdecoder.h"
#include "h26xaccessunit.h"
#include "ingestsession.h"
#include "rtmpprotocol.h"
#include "videotoolboxdecoder.h"

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
    ~NativeRtmpIngestSession() override;

    static bool supportsUrl(const QUrl& url);

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;
    IngestFailureKind lastFailureKind() const override { return m_lastFailureKind; }

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
    std::unique_ptr<VideoToolboxDecoder> m_videoDecoder;
    std::unique_ptr<AudioToolboxAacDecoder> m_audioDecoder;
    RtmpAvcConfig m_avcConfig;
    RtmpHevcConfig m_hevcConfig;
    RtmpAacConfig m_aacConfig;
    NativeVideoCodec m_videoCodec = NativeVideoCodec::Unknown;
    int m_outputChunkSize = 128;
    int m_streamId = 1;
    int64_t m_firstDtsMs = -1;
    int64_t m_prevDtsMs = -1;
    int64_t m_anchorStreamTimeMs = -1;
    int64_t m_firstAudioPtsMs = -1;
    int64_t m_prevAudioPtsMs = -1;
    int64_t m_audioAnchorStreamTimeMs = -1;
    int64_t m_lastPacketAtMs = -1;
    bool m_seenSupportedVideo = false;
    bool m_seenSupportedAudio = false;
    int64_t m_openedAtMs = -1;
    QString m_unsupportedReason;
    IngestFailureKind m_lastFailureKind = IngestFailureKind::None;
    RtmpChunkParser m_chunkParser;
    QList<RtmpMessage> m_pendingMessages;

    bool connectAndPlay(QString* error);
    bool performHandshake(QString* error);
    bool sendConnectCommand(QString* error);
    bool waitForCommandResult(double transactionId, RtmpMessage* result, QString* error);
    bool sendCreateStreamCommand(QString* error);
    bool sendPlayCommand(QString* error);
    bool readMessage(RtmpMessage* message, QString* error);
    bool sendMessage(int chunkStreamId, int messageType, int messageStreamId, qint64 timestampMs,
                     const QByteArray& payload, QString* error);
    bool readFully(char* data, qsizetype size, QString* error);
    bool writeFully(const QByteArray& bytes, QString* error);
    bool shouldStop() const;
    void log(const QString& message) const;
    void processMessage(const RtmpMessage& message);
    void processVideoMessage(qint64 timestampMs, const QByteArray& payload);
    void processAudioMessage(qint64 timestampMs, const QByteArray& payload);
    void resetVideoState();
    bool parseAvcSequenceHeader(const QByteArray& payload, QString* error);
    bool parseAacSequenceHeader(const QByteArray& payload, QString* error);
    int64_t sourcePtsMsForVideo(qint64 dtsMs, qint64 ptsMs);
    int64_t sourcePtsMsForAudio(qint64 ptsMs);
};

#endif // NATIVERTMPINGESTSESSION_H

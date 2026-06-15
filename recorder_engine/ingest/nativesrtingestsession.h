#ifndef NATIVESRTINGESTSESSION_H
#define NATIVESRTINGESTSESSION_H

#include "audiotoolboxaacdecoder.h"
#include "h26xaccessunit.h"
#include "ingestsession.h"
#include "mpegtsparser.h"
#include "videotoolboxdecoder.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QUrl>

#include <atomic>
#include <cstdint>
#include <memory>

class NativeSrtIngestSession final : public IngestSession {
public:
    NativeSrtIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning);
    ~NativeSrtIngestSession() override;

    static bool supportsUrl(const QUrl& url);

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;

private:
    int m_sourceIndex = -1;
    int m_outputWidth = 1920;
    int m_outputHeight = 1080;
    std::atomic<bool>* m_captureRunning = nullptr;

    std::atomic<bool> m_stopRequested{false};
    QUrl m_url;
    IngestCallbacks m_callbacks;
    QElapsedTimer m_monotonic;
    MpegTsParser m_tsParser;
    NativeVideoCodec m_activeCodec = NativeVideoCodec::Unknown;
    std::unique_ptr<H26xAccessUnitSplitter> m_splitter;
    std::unique_ptr<VideoToolboxDecoder> m_decoder;
    std::unique_ptr<AudioToolboxAacDecoder> m_audioDecoder;
    QByteArray m_tsBuffer;
    QByteArray m_audioRemainder;
    int m_socket = -1;
    bool m_srtLibraryStarted = false;
    int64_t m_firstDts90k = -1;
    int64_t m_prevDts90k = -1;
    int64_t m_anchorStreamTimeMs = -1;
    int64_t m_firstAudioPts90k = -1;
    int64_t m_prevAudioPts90k = -1;
    int64_t m_audioAnchorStreamTimeMs = -1;
    int64_t m_audioRemainderPts90k = -1;
    int64_t m_lastPacketAtMs = -1;
    bool m_loggedLatmUnsupported = false;

    bool openSocket(QString* error);
    void closeSocket();
    bool shouldStop() const;
    void log(const QString& message) const;
    void processReceivedBytes(const char* data, int size);
    void processPesPacket(const PesPacket& pes);
    void processAudioPesPacket(const PesPacket& pes);
    int64_t sourcePtsMsForUnit(const CompressedAccessUnit& unit);
    int64_t sourcePtsMsForAudio(qint64 pts90k);
};

#endif // NATIVESRTINGESTSESSION_H

#ifndef NATIVESRTINGESTSESSION_H
#define NATIVESRTINGESTSESSION_H

#include "nativeaacdecoder.h"
#include "h26xaccessunit.h"
#include "h26xseitimecode.h"
#include "ingestsession.h"
#include "mpegtsparser.h"
#include "nativesrturloptions.h"
#include "nativevideodecoder.h"
#include "recorder_engine/timing/sourceclock.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QUrl>

#include <atomic>
#include <cstdint>
#include <memory>

struct NativeSrtSockaddr;

class NativeSrtIngestSession final : public IngestSession {
#if defined(QT_TESTLIB_LIB)
    friend class TestIngestBackendSelector;
    friend class TestSrtIngestTeardown;
#endif
public:
    NativeSrtIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning);
    NativeSrtIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning, AnchoredSourceClock* sourceClock);
    ~NativeSrtIngestSession() override;

    static bool supportsUrl(const QUrl& url);
    static QByteArray streamIdForSocketOption(const QUrl& url);
    // Parsed SRT connection mode for this URL (caller/listener/rendezvous). Static
    // so the mode selection can be unit-tested without an instance.
    static NativeSrtMode connectionModeForUrl(const QUrl& url);

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;

    // Map a 90 kHz stream timestamp onto the recording timeline using the shared
    // A/V anchor (anchorTs90k <-> anchorStreamMs). Pure: returns -1 if any input is
    // negative. Static so it can be unit-tested without an instance.
    static int64_t sourcePtsMsFromAnchor(qint64 pts90k, int64_t anchorTs90k,
                                         int64_t anchorStreamMs);

    // Nominal fps used only to convert an extracted SMPTE 12M timecode into a 100 ns
    // offset since midnight. The SRT session itself carries no fps (the constructor
    // takes only output width/height). This is an ALIAS of the shared
    // Smpte12m::kTimecodeNominalFps so producers (SRT/RTMP) and the consumer
    // (TimecodeAligner) reference ONE source of truth and provably agree. This
    // affects only the TC mapping — A/V sync uses the 90 kHz stream clock, never this.
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
    MpegTsParser m_tsParser;
    NativeVideoCodec m_activeCodec = NativeVideoCodec::Unknown;
    std::unique_ptr<H26xAccessUnitSplitter> m_splitter;
    std::unique_ptr<NativeVideoDecoder> m_decoder;
    std::unique_ptr<NativeAacDecoder> m_audioDecoder;
    QByteArray m_tsBuffer;
    QByteArray m_audioRemainder;
    int m_socket = -1;
    // Listener mode only: the bound/listening socket that accepts the inbound
    // caller. m_socket then becomes the accepted data socket used by srt_recv.
    int m_listenSocket = -1;
    bool m_srtLibraryStarted = false;
    int64_t m_statRetrans = -1;
    int64_t m_statLossTotal = -1;
    int64_t m_statDropTotal = -1;
    int64_t m_statRecvTotal = -1;
    int64_t m_lastStatsAtMs = -1;
    // Single shared A/V anchor for this source. PCR/video own re-anchoring; audio
    // follows the same recovered 90 kHz sender clock.
    AnchoredSourceClock m_ownedClock{ClockQuality::Pcr, 90};
    AnchoredSourceClock* m_clock = &m_ownedClock;
    bool m_externalClock = false;
    // Per-stream previous timestamps, for discontinuity (jump) detection only.
    int64_t m_prevDts90k = -1;
    int64_t m_prevAudioPts90k = -1;
    int64_t m_prevRawPcr90k = -1;
    int64_t m_prevRawVideoDts90k = -1;
    int64_t m_prevRawAudioPts90k = -1;
    int64_t m_pcrWrapOffset90k = 0;
    int64_t m_videoWrapOffset90k = 0;
    int64_t m_audioWrapOffset90k = 0;
    bool m_forceNextPcrObserve = false;
    int64_t m_audioRemainderPts90k = -1;
    // SMPTE 12M timecode (100 ns since midnight) extracted from the current access
    // unit's SEI, stamped onto the emitted DecodedVideoFrame. -1 = the current AU
    // carries no timecode SEI (the common case). Reset to -1 per access unit so a
    // frame without a TC SEI never inherits a previous frame's timecode.
    int64_t m_pendingVideoTimecode100ns = -1;
    int64_t m_lastPacketAtMs = -1;
    int64_t m_lastDecodeErrorLogMs = -1;
    bool m_loggedLatmUnsupported = false;

    bool openSocket(QString* error);
    bool openSocketToAddress(const NativeSrtSockaddr& address, QString* error);
    bool connectAndAwait(const NativeSrtSockaddr& address, QString* error);
    bool acceptListenerConnection(const NativeSrtSockaddr& address, QString* error);
    void closeSocket();
    bool shouldStop() const;
    void log(const QString& message) const;
    void processReceivedBytes(const char* data, int size);
    void processPesPacket(const PesPacket& pes);
    void processAudioPesPacket(const PesPacket& pes);
    int64_t unwrapPcr90k(int64_t raw90k);
    int64_t unwrapVideo90k(int64_t raw90k);
    int64_t unwrapAudio90k(int64_t raw90k);
    int64_t sourcePtsMsForUnit(const CompressedAccessUnit& unit);
    int64_t sourcePtsMsForAudio(qint64 pts90k);
    // Reset m_pendingVideoTimecode100ns to -1, then (if the access unit carries a
    // SMPTE 12M timecode SEI) set it to that TC as 100 ns since midnight. Called once
    // per access unit so timecode never bleeds across frames.
    void updatePendingVideoTimecode(const CompressedAccessUnit& unit);
};

#endif // NATIVESRTINGESTSESSION_H

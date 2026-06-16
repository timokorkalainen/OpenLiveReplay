#ifndef MPEGTSPARSER_H
#define MPEGTSPARSER_H

#include "pespacket.h"

#include <QHash>
#include <QList>
#include <QSet>

class MpegTsParser {
public:
    // Per-packet side info surfaced to the caller. pcr90k >= 0 iff THIS packet
    // carried a PCR (33-bit 90 kHz base) on the program's PCR PID; discontinuity
    // iff the PCR PID's adaptation field set the discontinuity_indicator.
    struct TsPacketInfo {
        qint64 pcr90k = -1;
        bool discontinuity = false;
    };

    bool pushTsPacket(const QByteArray& packet, QList<PesPacket>* completedPes,
                      TsPacketInfo* info = nullptr);

    quint16 pmtPid() const { return m_pmtPid; }
    quint16 videoPid() const { return m_videoPid; }
    NativeVideoCodec videoCodec() const { return m_videoCodec; }
    quint16 audioPid() const { return m_audioPid; }

private:
    struct PesAssembly {
        NativeElementaryStreamKind kind = NativeElementaryStreamKind::Unknown;
        NativeVideoCodec videoCodec = NativeVideoCodec::Unknown;
        QByteArray bytes;
        int expectedSize = -1;
    };

    quint16 m_pmtPid = 0xffff;
    quint16 m_pcrPid = 0xffff;
    quint16 m_videoPid = 0xffff;
    NativeVideoCodec m_videoCodec = NativeVideoCodec::Unknown;
    quint16 m_audioPid = 0xffff;
    NativeElementaryStreamKind m_audioKind = NativeElementaryStreamKind::Unknown;
    QHash<quint16, PesAssembly> m_pes;
    QHash<quint16, quint8> m_lastContinuityCounter;
    QSet<quint16> m_waitingForPayloadStart;

    void parsePat(const QByteArray& payload, bool payloadStart);
    void parsePmt(const QByteArray& payload, bool payloadStart);
    void pushPesPayload(quint16 pid, bool payloadStart, const QByteArray& payload,
                        QList<PesPacket>* completedPes);
    bool flushPes(quint16 pid, QList<PesPacket>* completedPes);
    void updateExpectedPesSize(PesAssembly* assembly);
    bool acceptContinuity(quint16 pid, quint8 continuityCounter, bool hasPayload,
                          bool payloadStart, bool discontinuity);
};

#endif // MPEGTSPARSER_H

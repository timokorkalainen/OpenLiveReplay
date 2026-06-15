#include "mpegtsparser.h"

#include <QtGlobal>

namespace {

quint8 byteAt(const QByteArray& bytes, int offset)
{
    return quint8(uchar(bytes[offset]));
}

quint16 read16(const uchar* p)
{
    return quint16((quint16(p[0]) << 8) | quint16(p[1]));
}

qint64 readPts90k(const uchar* p)
{
    return ((qint64((p[0] >> 1) & 0x07)) << 30)
        | ((qint64(read16(p + 1) >> 1) & 0x7fff) << 15)
        | (qint64(read16(p + 3) >> 1) & 0x7fff);
}

int sectionStartOffset(const QByteArray& payload, bool payloadStart)
{
    if (!payloadStart || payload.isEmpty()) {
        return -1;
    }

    const int pointerField = byteAt(payload, 0);
    const int sectionOffset = 1 + pointerField;
    if (sectionOffset >= payload.size()) {
        return -1;
    }
    return sectionOffset;
}

int sectionPayloadEnd(const QByteArray& payload, int sectionOffset)
{
    if (sectionOffset < 0 || sectionOffset + 3 > payload.size()) {
        return -1;
    }

    const int sectionLength = ((byteAt(payload, sectionOffset + 1) & 0x0f) << 8)
        | byteAt(payload, sectionOffset + 2);
    if (sectionLength < 4) {
        return -1;
    }

    const int sectionEndWithCrc = sectionOffset + 3 + sectionLength;
    if (sectionEndWithCrc > payload.size()) {
        return -1;
    }
    return sectionEndWithCrc - 4;
}

NativeElementaryStreamKind kindForPid(quint16 pid, quint16 videoPid, quint16 audioPid,
                                      NativeElementaryStreamKind audioKind)
{
    if (pid == videoPid) {
        return NativeElementaryStreamKind::Video;
    }
    if (pid == audioPid) {
        return audioKind;
    }
    return NativeElementaryStreamKind::Unknown;
}

} // namespace

bool MpegTsParser::pushTsPacket(const QByteArray& packet, QList<PesPacket>* completedPes)
{
    if (packet.size() != 188 || byteAt(packet, 0) != 0x47) {
        return false;
    }

    const bool payloadStart = (byteAt(packet, 1) & 0x40) != 0;
    const quint16 pid = quint16(((byteAt(packet, 1) & 0x1f) << 8) | byteAt(packet, 2));
    const quint8 adaptationFieldControl = (byteAt(packet, 3) >> 4) & 0x03;
    const quint8 continuityCounter = byteAt(packet, 3) & 0x0f;
    const bool hasPayload = adaptationFieldControl == 1 || adaptationFieldControl == 3;
    bool discontinuity = false;

    int offset = 4;
    if (adaptationFieldControl == 0) {
        return false;
    }
    if (adaptationFieldControl == 2 || adaptationFieldControl == 3) {
        if (offset >= packet.size()) {
            return false;
        }
        const int adaptationLength = byteAt(packet, offset);
        if (offset + 1 + adaptationLength > packet.size()) {
            return false;
        }
        if (adaptationLength > 0) {
            discontinuity = (byteAt(packet, offset + 1) & 0x80) != 0;
        }
        offset += 1 + adaptationLength;
    }

    if (!acceptContinuity(pid, continuityCounter, hasPayload, payloadStart, discontinuity)) {
        return true;
    }
    if (!hasPayload) {
        return true;
    }

    const QByteArray payload = packet.mid(offset);
    if (pid == 0x0000) {
        parsePat(payload, payloadStart);
    } else if (pid == m_pmtPid) {
        parsePmt(payload, payloadStart);
    } else if (pid == m_videoPid || pid == m_audioPid) {
        pushPesPayload(pid, payloadStart, payload, completedPes);
    }

    return true;
}

void MpegTsParser::parsePat(const QByteArray& payload, bool payloadStart)
{
    int off = sectionStartOffset(payload, payloadStart);
    if (off < 0 || off + 8 > payload.size() || byteAt(payload, off) != 0x00) {
        return;
    }

    const int end = sectionPayloadEnd(payload, off);
    if (end < 0 || off + 8 > end) {
        return;
    }

    off += 8;
    while (off + 4 <= end) {
        const uchar* programData = reinterpret_cast<const uchar*>(payload.constData() + off);
        const quint16 programNumber = read16(programData);
        const quint16 pid = quint16(((byteAt(payload, off + 2) & 0x1f) << 8)
                                    | byteAt(payload, off + 3));
        if (programNumber != 0) {
            m_pmtPid = pid;
            return;
        }
        off += 4;
    }
}

void MpegTsParser::parsePmt(const QByteArray& payload, bool payloadStart)
{
    int off = sectionStartOffset(payload, payloadStart);
    if (off < 0 || off + 12 > payload.size() || byteAt(payload, off) != 0x02) {
        return;
    }

    const int end = sectionPayloadEnd(payload, off);
    if (end < 0 || off + 12 > end) {
        return;
    }

    const int programInfoLength = ((byteAt(payload, off + 10) & 0x0f) << 8)
        | byteAt(payload, off + 11);
    int es = off + 12 + programInfoLength;
    if (es > end) {
        return;
    }

    quint16 candidateVideoPid = 0xffff;
    NativeVideoCodec candidateVideoCodec = NativeVideoCodec::Unknown;
    quint16 candidateAudioPid = 0xffff;
    NativeElementaryStreamKind candidateAudioKind = NativeElementaryStreamKind::Unknown;

    while (es + 5 <= end) {
        const quint8 streamType = byteAt(payload, es);
        const quint16 pid = quint16(((byteAt(payload, es + 1) & 0x1f) << 8)
                                    | byteAt(payload, es + 2));
        const int esInfoLength = ((byteAt(payload, es + 3) & 0x0f) << 8)
            | byteAt(payload, es + 4);
        if (es + 5 + esInfoLength > end) {
            return;
        }

        if (streamType == 0x1b) {
            candidateVideoPid = pid;
            candidateVideoCodec = NativeVideoCodec::H264;
        } else if (streamType == 0x24) {
            candidateVideoPid = pid;
            candidateVideoCodec = NativeVideoCodec::Hevc;
        } else if (streamType == 0x0f) {
            candidateAudioPid = pid;
            candidateAudioKind = NativeElementaryStreamKind::AudioAac;
        } else if (streamType == 0x11) {
            candidateAudioPid = pid;
            candidateAudioKind = NativeElementaryStreamKind::AudioAacLatm;
        }

        es += 5 + esInfoLength;
    }

    m_videoPid = candidateVideoPid;
    m_videoCodec = candidateVideoCodec;
    m_audioPid = candidateAudioPid;
    m_audioKind = candidateAudioKind;
}

void MpegTsParser::pushPesPayload(quint16 pid, bool payloadStart, const QByteArray& payload,
                                  QList<PesPacket>* completedPes)
{
    if (payloadStart) {
        flushPes(pid, completedPes);
        m_pes.remove(pid);
    } else if (!m_pes.contains(pid)) {
        return;
    }

    PesAssembly& assembly = m_pes[pid];
    assembly.kind = kindForPid(pid, m_videoPid, m_audioPid, m_audioKind);
    assembly.videoCodec = (pid == m_videoPid) ? m_videoCodec : NativeVideoCodec::Unknown;
    assembly.bytes.append(payload);
    updateExpectedPesSize(&assembly);

    if (assembly.expectedSize >= 0 && assembly.bytes.size() >= assembly.expectedSize) {
        if (assembly.bytes.size() > assembly.expectedSize) {
            assembly.bytes.truncate(assembly.expectedSize);
        }
        flushPes(pid, completedPes);
        m_pes.remove(pid);
    }
}

bool MpegTsParser::flushPes(quint16 pid, QList<PesPacket>* completedPes)
{
    if (!completedPes || !m_pes.contains(pid)) {
        return false;
    }

    PesAssembly assembly = m_pes.value(pid);
    if (assembly.bytes.size() < 9) {
        return false;
    }

    const uchar* p = reinterpret_cast<const uchar*>(assembly.bytes.constData());
    if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01) {
        return false;
    }

    const int headerDataLength = p[8];
    const int payloadOffset = 9 + headerDataLength;
    if (payloadOffset > assembly.bytes.size()) {
        return false;
    }

    PesPacket pes;
    pes.pid = pid;
    pes.kind = assembly.kind;
    pes.videoCodec = assembly.videoCodec;

    const quint8 ptsDtsFlags = (p[7] >> 6) & 0x03;
    if ((ptsDtsFlags == 0x02 || ptsDtsFlags == 0x03) && headerDataLength >= 5) {
        pes.pts90k = readPts90k(p + 9);
    }
    if (ptsDtsFlags == 0x03 && headerDataLength >= 10) {
        pes.dts90k = readPts90k(p + 14);
    }

    pes.payload = assembly.bytes.mid(payloadOffset);
    completedPes->append(pes);
    return true;
}

void MpegTsParser::updateExpectedPesSize(PesAssembly* assembly)
{
    if (!assembly || assembly->expectedSize >= 0 || assembly->bytes.size() < 6) {
        return;
    }

    const uchar* p = reinterpret_cast<const uchar*>(assembly->bytes.constData());
    if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01) {
        return;
    }

    const int pesPacketLength = read16(p + 4);
    if (pesPacketLength > 0) {
        assembly->expectedSize = 6 + pesPacketLength;
    }
}

bool MpegTsParser::acceptContinuity(quint16 pid, quint8 continuityCounter, bool hasPayload,
                                    bool payloadStart, bool discontinuity)
{
    if (discontinuity) {
        m_lastContinuityCounter.remove(pid);
        m_pes.remove(pid);
        if (pid == m_videoPid || pid == m_audioPid) {
            m_waitingForPayloadStart.insert(pid);
        }
    }

    if (!hasPayload) {
        return true;
    }

    const auto lastIt = m_lastContinuityCounter.constFind(pid);
    if (lastIt != m_lastContinuityCounter.constEnd()) {
        if (continuityCounter == *lastIt) {
            return false;
        }

        const quint8 expected = quint8((*lastIt + 1) & 0x0f);
        if (continuityCounter != expected) {
            m_pes.remove(pid);
            if (pid == m_videoPid || pid == m_audioPid) {
                m_waitingForPayloadStart.insert(pid);
            }
            m_lastContinuityCounter.insert(pid, continuityCounter);
            if (!payloadStart) {
                return false;
            }
        } else {
            m_lastContinuityCounter.insert(pid, continuityCounter);
        }
    } else {
        m_lastContinuityCounter.insert(pid, continuityCounter);
    }

    if (payloadStart) {
        m_waitingForPayloadStart.remove(pid);
    } else if (m_waitingForPayloadStart.contains(pid)) {
        return false;
    }

    return true;
}

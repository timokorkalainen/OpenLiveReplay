#include "playback/output/iotargets/st2110framer.h"

namespace {

quint32 wrapRtpTimestamp(qint64 ticks) {
    return quint32(ticks & qint64(0xFFFFFFFFu));
}

} // namespace

St2110FrameFramer::St2110FrameFramer(FrameRate rate, quint32 ssrc, quint8 payloadType)
    : m_rate(rate), m_ssrc(ssrc), m_payloadType(payloadType) {}

St2110VideoFrame St2110FrameFramer::frameVideo(const QByteArray& essence, qint64 outputFrameIndex,
                                               qint64 programmeTimecode100ns) const {
    St2110VideoFrame frame;
    frame.essence = essence;
    frame.payloadType = m_payloadType;
    frame.ssrc = m_ssrc;
    frame.markerLast = true;
    frame.rtpTimestamp90k =
        programmeTimecode100ns >= 0
            ? wrapRtpTimestamp((programmeTimecode100ns * qint64(90000)) / qint64(10000000))
            : rtpTimestampForFrame(outputFrameIndex);
    return frame;
}

quint32 St2110FrameFramer::rtpTimestampForFrame(qint64 outputFrameIndex) const {
    if (!m_rate.isValid() || outputFrameIndex <= 0) return 0;
    const qint64 ticks =
        (qint64(90000) * qint64(m_rate.denominator) * outputFrameIndex) / m_rate.numerator;
    return wrapRtpTimestamp(ticks);
}

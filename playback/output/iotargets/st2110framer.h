#ifndef ST2110FRAMER_H
#define ST2110FRAMER_H

#include "playback/framerate.h"

#include <QByteArray>
#include <QtGlobal>

struct St2110VideoFrame {
    QByteArray essence;
    quint32 rtpTimestamp90k = 0;
    quint8 payloadType = 96;
    quint32 ssrc = 0;
    bool markerLast = true;
};

class St2110FrameFramer {
public:
    St2110FrameFramer(FrameRate rate, quint32 ssrc, quint8 payloadType = 96);

    St2110VideoFrame frameVideo(const QByteArray& essence, qint64 outputFrameIndex,
                                qint64 programmeTimecode100ns) const;
    quint32 rtpTimestampForFrame(qint64 outputFrameIndex) const;

private:
    FrameRate m_rate;
    quint32 m_ssrc = 0;
    quint8 m_payloadType = 96;
};

#endif // ST2110FRAMER_H

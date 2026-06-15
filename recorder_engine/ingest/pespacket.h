#ifndef PESPACKET_H
#define PESPACKET_H

#include <QByteArray>
#include <QtGlobal>

enum class NativeVideoCodec {
    Unknown,
    H264,
    Hevc,
};

enum class NativeElementaryStreamKind {
    Unknown,
    Video,
    AudioAac,
    AudioAacLatm,
};

struct PesPacket {
    quint16 pid = 0;
    NativeElementaryStreamKind kind = NativeElementaryStreamKind::Unknown;
    NativeVideoCodec videoCodec = NativeVideoCodec::Unknown;
    qint64 pts90k = -1;
    qint64 dts90k = -1;
    QByteArray payload;
};

#endif // PESPACKET_H

#ifndef H26XACCESSUNIT_H
#define H26XACCESSUNIT_H

#include "pespacket.h"

#include <QList>

struct H26xParameterSets {
    QList<QByteArray> h264Sps;
    QList<QByteArray> h264Pps;
    QList<QByteArray> hevcVps;
    QList<QByteArray> hevcSps;
    QList<QByteArray> hevcPps;
};

struct CompressedAccessUnit {
    NativeVideoCodec codec = NativeVideoCodec::Unknown;
    qint64 pts90k = -1;
    qint64 dts90k = -1;
    QByteArray annexB;
    H26xParameterSets parameterSets;
};

class H26xAccessUnitSplitter {
public:
    explicit H26xAccessUnitSplitter(NativeVideoCodec codec);

    QList<CompressedAccessUnit> pushPesPayload(const QByteArray& payload, qint64 pts90k, qint64 dts90k);
    H26xParameterSets parameterSets() const { return m_parameterSets; }

private:
    NativeVideoCodec m_codec = NativeVideoCodec::Unknown;
    H26xParameterSets m_parameterSets;

    void inspectNal(const QByteArray& nal);
};

#endif // H26XACCESSUNIT_H

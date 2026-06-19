#include "recorder_engine/codec/avcc.h"

namespace {
void appendSizedNal(QByteArray* out, const QByteArray& nal) {
    const int size = nal.size();
    out->append(char((size >> 8) & 0xff));
    out->append(char(size & 0xff));
    out->append(nal);
}
} // namespace

bool parseAvcc(const QByteArray& avcc,
               QList<QByteArray>* sps,
               QList<QByteArray>* pps) {
    if (!sps || !pps) return false;
    sps->clear();
    pps->clear();
    // Minimum viable avcC: 6 bytes header + 2 SPS len + 1 PPS count + 2 PPS len
    const int size = avcc.size();
    if (size < 8) return false;
    const auto* d = reinterpret_cast<const uint8_t*>(avcc.constData());
    // Byte [0]: configurationVersion (skip); [1..3]: profile/compat/level (skip)
    // Byte [4]: 0xFF — lengthSizeMinusOne in low 2 bits (must be 3)
    // Byte [5]: 0xE0|numSPS
    int offset = 5;
    const int numSps = d[offset] & 0x1f;
    offset++;
    for (int i = 0; i < numSps; ++i) {
        if (offset + 2 > size) return false;
        const int len = (d[offset] << 8) | d[offset + 1];
        offset += 2;
        if (len <= 0 || offset + len > size) return false;
        sps->append(QByteArray(reinterpret_cast<const char*>(d + offset), len));
        offset += len;
    }
    if (offset + 1 > size) return false;
    const int numPps = d[offset];
    offset++;
    for (int i = 0; i < numPps; ++i) {
        if (offset + 2 > size) return false;
        const int len = (d[offset] << 8) | d[offset + 1];
        offset += 2;
        if (len <= 0 || offset + len > size) return false;
        pps->append(QByteArray(reinterpret_cast<const char*>(d + offset), len));
        offset += len;
    }
    return !sps->isEmpty() && !pps->isEmpty();
}

QByteArray buildAvcCFromParameterSets(const QList<QByteArray>& sps,
                                      const QList<QByteArray>& pps) {
    if (sps.isEmpty() || pps.isEmpty()) return {};
    const QByteArray& firstSps = sps.first();
    if (firstSps.size() < 4) return {};

    QByteArray avcc;
    avcc.append(char(0x01));               // configurationVersion
    avcc.append(firstSps.at(1));           // AVCProfileIndication
    avcc.append(firstSps.at(2));           // profile_compatibility
    avcc.append(firstSps.at(3));           // AVCLevelIndication
    avcc.append(char(0xff));               // 111111 + lengthSizeMinusOne(3)
    avcc.append(char(0xe0 | (sps.size() & 0x1f))); // 111 + numOfSequenceParameterSets
    for (const QByteArray& s : sps) appendSizedNal(&avcc, s);
    avcc.append(char(pps.size() & 0xff));  // numOfPictureParameterSets
    for (const QByteArray& p : pps) appendSizedNal(&avcc, p);
    return avcc;
}

#include "recorder_engine/codec/avcc.h"

namespace {
void appendSizedNal(QByteArray* out, const QByteArray& nal) {
    const int size = nal.size();
    out->append(char((size >> 8) & 0xff));
    out->append(char(size & 0xff));
    out->append(nal);
}
} // namespace

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

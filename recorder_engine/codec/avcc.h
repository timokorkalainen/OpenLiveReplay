#ifndef OLR_AVCC_H
#define OLR_AVCC_H

#include <QByteArray>
#include <QList>

// Build an AVCDecoderConfigurationRecord ("avcC") from H.264 SPS/PPS NAL
// payloads (no start codes / length prefixes — raw NAL bytes). Used as the
// Matroska CodecPrivate for a hardware-encoded H.264 track. Returns an empty
// QByteArray when sps/pps is empty or the first SPS is shorter than 4 bytes
// (profile/compat/level are read from SPS[1..3]).
QByteArray buildAvcCFromParameterSets(const QList<QByteArray>& sps,
                                      const QList<QByteArray>& pps);

#endif // OLR_AVCC_H

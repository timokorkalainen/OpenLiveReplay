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

// Parse an AVCDecoderConfigurationRecord ("avcC") blob into raw SPS and PPS
// NAL payloads (no start codes, no length prefixes). Returns true on success.
// avcC layout: [0]=configVersion [1..3]=profile/compat/level [4]=0xFF
// [5]=0xE0|numSPS then numSPS*(2-byte BE len + payload)
// then 1 byte numPPS then numPPS*(2-byte BE len + payload).
bool parseAvcc(const QByteArray& avcc, QList<QByteArray>* sps, QList<QByteArray>* pps);

#endif // OLR_AVCC_H

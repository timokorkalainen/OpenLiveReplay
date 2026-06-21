#ifndef OLR_COLORVUI_H
#define OLR_COLORVUI_H

#include "playback/output/colormetadata.h"
#include "recorder_engine/ingest/pespacket.h"

#include <QByteArray>

struct VuiColorInfo {
    bool present = false;
    ColorRange range = ColorRange::Video;
    ColorPrimaries primaries = ColorPrimaries::Unspecified;
    ColorTransfer transfer = ColorTransfer::Unspecified;
    ColorMatrix matrix = ColorMatrix::Bt709;
};

// Parse colour fields from a raw SPS NAL payload: the NAL header byte is
// included, but no start code or length prefix is present.
VuiColorInfo parseSpsColorVui(NativeVideoCodec codec, const QByteArray& nal);

#endif // OLR_COLORVUI_H

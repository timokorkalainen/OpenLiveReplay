#include "playback/output/colormetadatapolicy.h"

#include "recorder_engine/ingest/colorvui.h"

namespace {

ColorMatrix matrixFromAv(int code) {
    switch (code) {
    case 1:
        return ColorMatrix::Bt709;
    case 5:
    case 6:
        return ColorMatrix::Bt601;
    case 9:
    case 10:
        return ColorMatrix::Bt2020;
    default:
        return ColorMatrix::Bt709;
    }
}

ColorPrimaries primariesFromAv(int code) {
    switch (code) {
    case 1:
        return ColorPrimaries::Bt709;
    case 5:
    case 6:
        return ColorPrimaries::Bt601;
    case 9:
        return ColorPrimaries::Bt2020;
    default:
        return ColorPrimaries::Unspecified;
    }
}

ColorTransfer transferFromAv(int code) {
    switch (code) {
    case 1:
    case 14:
    case 15:
        return ColorTransfer::Bt709;
    case 5:
    case 6:
    case 8:
        return ColorTransfer::Bt601;
    case 16:
        return ColorTransfer::Bt2020;
    default:
        return ColorTransfer::Unspecified;
    }
}

} // namespace

ColorMetadata defaultColorMetadataForHeight(int height) {
    ColorMetadata m;
    const bool hd = height > kDefaultBt709HeightThreshold;
    m.matrix = hd ? ColorMatrix::Bt709 : ColorMatrix::Bt601;
    m.primaries = hd ? ColorPrimaries::Bt709 : ColorPrimaries::Bt601;
    m.transfer = hd ? ColorTransfer::Bt709 : ColorTransfer::Bt601;
    m.range = ColorRange::Video;
    m.chromaFormat = ChromaFormat::Yuv420;
    m.bitDepth = 8;
    return m;
}

ColorMetadata resolveColorMetadata(const VuiColorInfo& info, int height, int avColorspace,
                                   int avColorRange, int avColorPrimaries, int avColorTransfer) {
    ColorMetadata m = defaultColorMetadataForHeight(height);
    const bool blanketUnspecified =
        avColorspace == 2 && avColorRange == 2 && avColorPrimaries == 2 && avColorTransfer == 2;

    if (avColorspace != 2) m.matrix = matrixFromAv(avColorspace);
    if (avColorPrimaries != 2) {
        const ColorPrimaries p = primariesFromAv(avColorPrimaries);
        if (p != ColorPrimaries::Unspecified) m.primaries = p;
    }
    if (avColorTransfer != 2) {
        const ColorTransfer t = transferFromAv(avColorTransfer);
        if (t != ColorTransfer::Unspecified) m.transfer = t;
    }
    if (avColorRange == 2 && !blanketUnspecified) {
        m.range = ColorRange::Full;
    } else if (avColorRange == 1) {
        m.range = ColorRange::Video;
    }

    if (info.present) {
        m.matrix = info.matrix;
        m.range = info.range;
        if (info.primaries != ColorPrimaries::Unspecified) m.primaries = info.primaries;
        if (info.transfer != ColorTransfer::Unspecified) m.transfer = info.transfer;
    }
    return m;
}

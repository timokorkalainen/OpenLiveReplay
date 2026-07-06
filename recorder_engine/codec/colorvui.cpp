#include "recorder_engine/codec/colorvui.h"

VuiColorCodePoints vuiColorCodePointsFor(const ColorMetadata& color) {
    VuiColorCodePoints v;
    switch (color.primaries) {
    case ColorPrimaries::Bt709:
        v.colourPrimaries = 1;
        break;
    case ColorPrimaries::Bt601:
        v.colourPrimaries = 6;
        break;
    case ColorPrimaries::Bt2020:
        v.colourPrimaries = 9;
        break;
    case ColorPrimaries::Unspecified:
        v.colourPrimaries = 2;
        break;
    }
    switch (color.transfer) {
    case ColorTransfer::Bt709:
        v.transferCharacteristics = 1;
        break;
    case ColorTransfer::Bt601:
        v.transferCharacteristics = 6;
        break;
    case ColorTransfer::Bt2020:
        v.transferCharacteristics = 14;
        break;
    case ColorTransfer::Unspecified:
        v.transferCharacteristics = 2;
        break;
    }
    switch (color.matrix) {
    case ColorMatrix::Bt709:
        v.matrixCoefficients = 1;
        break;
    case ColorMatrix::Bt601:
        v.matrixCoefficients = 6;
        break;
    case ColorMatrix::Bt2020:
        v.matrixCoefficients = 9;
        break;
    }
    v.fullRange = color.range == ColorRange::Full;
    return v;
}

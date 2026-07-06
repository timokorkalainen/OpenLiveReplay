#ifndef RECORDER_ENGINE_CODEC_COLORVUI_H
#define RECORDER_ENGINE_CODEC_COLORVUI_H

#include "playback/output/colormetadata.h"

struct VuiColorCodePoints {
    int colourPrimaries = 1;
    int transferCharacteristics = 1;
    int matrixCoefficients = 1;
    bool fullRange = false;
};

VuiColorCodePoints vuiColorCodePointsFor(const ColorMetadata& color);

#endif // RECORDER_ENGINE_CODEC_COLORVUI_H

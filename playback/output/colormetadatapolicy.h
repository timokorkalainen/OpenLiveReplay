#ifndef COLORMETADATAPOLICY_H
#define COLORMETADATAPOLICY_H

#include "playback/output/colormetadata.h"

struct VuiColorInfo;

constexpr int kDefaultBt709HeightThreshold = 576;

ColorMetadata defaultColorMetadataForHeight(int height);

ColorMetadata resolveColorMetadata(const VuiColorInfo& info, int height, int avColorspace,
                                   int avColorRange, int avColorPrimaries, int avColorTransfer);

#endif // COLORMETADATAPOLICY_H

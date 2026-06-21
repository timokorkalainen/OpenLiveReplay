#ifndef OLR_COLORTAGS_APPLE_H
#define OLR_COLORTAGS_APPLE_H

#ifdef __APPLE__

#include "recorder_engine/ingest/colorvui.h"

#include <CoreMedia/CoreMedia.h>

VuiColorInfo colorVuiFromFormatDescription(CMFormatDescriptionRef fmt);

#endif // __APPLE__

#endif // OLR_COLORTAGS_APPLE_H

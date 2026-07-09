#ifndef OLR_GPUFRAMEREADBACKTELEMETRY_H
#define OLR_GPUFRAMEREADBACKTELEMETRY_H

#include "playback/output/framepixelformat.h"

#include <QtGlobal>

qint64 gpuFrameReadToCpuCount();
void gpuResetFrameReadToCpuCount();
void gpuRecordFrameReadToCpuReadback();

#endif // OLR_GPUFRAMEREADBACKTELEMETRY_H

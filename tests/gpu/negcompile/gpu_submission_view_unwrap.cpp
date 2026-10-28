#include "playback/gpu/gpuopscope.h"

#include <memory>

void* forbiddenGpuSubmissionViewUnwrap(const GpuSurfacePack<1>& submittedView) {
    return submittedView.owners()[0]->nativeHandle();
}

#ifndef OLR_GPUPIPELINECONFIG_H
#define OLR_GPUPIPELINECONFIG_H

// The single runtime capability gate for the GPU-resident pipeline. It is off
// by default; callers must also verify that the platform RHI context exists.
bool gpuPipelineEnabled();

// Test/micro-stress knobs used only by GPU paths.
int gpuForcedPerTrackBudget();
bool gpuConsumeInjectedAllocFailure();
void gpuSetInjectedAllocFailures(int count);

#endif // OLR_GPUPIPELINECONFIG_H

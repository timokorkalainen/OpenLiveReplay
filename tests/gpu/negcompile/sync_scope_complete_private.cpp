#include "playback/gpu/gpusurfacelease.h"

// Must NOT compile: complete() is private; withRead() discharges the scope itself.
void misuse() {
    GpuSyncReadScope scope;
    scope.complete();
}

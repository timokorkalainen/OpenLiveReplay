#include "playback/gpu/gpusurface.h"

// GpuSurface declares only pure virtuals and an inline defaulted destructor, so
// it needs no out-of-line definitions here; the vtable/typeinfo are weak-emitted
// wherever the interface is used.

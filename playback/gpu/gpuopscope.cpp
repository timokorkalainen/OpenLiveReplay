#include "playback/gpu/gpuopscope.h"

#include <cassert>

GpuOpScope::GpuOpScope(std::shared_ptr<GpuFence> fence, GpuRetireRegistry& registry)
    : m_fence(std::move(fence)), m_registry(registry) {
    assert(m_fence && "GpuOpScope requires a real fence");
}

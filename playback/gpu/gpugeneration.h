#ifndef OLR_GPUGENERATION_H
#define OLR_GPUGENERATION_H

#include <atomic>
#include <cstdint>

class GpuGenerationCounter {
public:
    static GpuGenerationCounter& instance();

    uint64_t current() const;
    uint64_t bump();
    void reset();

private:
    GpuGenerationCounter() = default;

    std::atomic<uint64_t> m_generation{0};
};

#endif // OLR_GPUGENERATION_H

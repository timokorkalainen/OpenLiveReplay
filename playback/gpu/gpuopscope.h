#ifndef OLR_GPU_OP_SCOPE_H
#define OLR_GPU_OP_SCOPE_H

#include <QVector>
#include <QtGlobal>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

class GpuFence;
class GpuRetireRegistry;
class GpuSurface;

enum class GpuSubmitOutcome { NotSubmitted, Submitted, SubmittedWithError };

class GpuOpScope final {
public:
    GpuOpScope(std::shared_ptr<GpuFence> fence, GpuRetireRegistry& registry);
    ~GpuOpScope();

    GpuOpScope(const GpuOpScope&) = delete;
    GpuOpScope& operator=(const GpuOpScope&) = delete;
    GpuOpScope(GpuOpScope&&) = delete;
    GpuOpScope& operator=(GpuOpScope&&) = delete;

    bool track(std::shared_ptr<GpuSurface> surface);
    bool cancel();
    uint64_t fenceValue() const { return m_fenceValue; }

    template <typename SubmitFn>
    bool submit(SubmitFn&& submitFn) {
        if (m_state != State::Open || !m_fence) return false;
        const GpuSubmitOutcome outcome = std::invoke(std::forward<SubmitFn>(submitFn));
        if (outcome == GpuSubmitOutcome::NotSubmitted) {
            cancel();
            return false;
        }
        return finalizeSubmitted(outcome);
    }

    static uint64_t spillAllocationCount();

private:
    enum class State { Open, Canceled, Finalized };
    static constexpr qsizetype kInlineSurfaceCapacity = 4;

    bool finalizeSubmitted(GpuSubmitOutcome outcome);
    bool contains(const GpuSurface* surface) const;
    void retireTracked(uint64_t fenceValue);
    void clearTracked();

    std::shared_ptr<GpuFence> m_fence;
    GpuRetireRegistry& m_registry;
    std::array<std::shared_ptr<GpuSurface>, kInlineSurfaceCapacity> m_inline;
    qsizetype m_inlineCount = 0;
    QVector<std::shared_ptr<GpuSurface>> m_overflow;
    State m_state = State::Open;
    uint64_t m_fenceValue = 0;
};

#endif // OLR_GPU_OP_SCOPE_H

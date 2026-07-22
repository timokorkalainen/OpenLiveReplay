#ifndef OLR_GPURHICONTEXT_H
#define OLR_GPURHICONTEXT_H

#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusubmission.h"
#include "playback/output/framehandle.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

class GpuFence;
class GpuRhiContext;
class QSemaphore;
class QRhi;

struct GpuReadbackResult {
    CpuPlanes planes;
    GpuSubmitOutcome outcome = GpuSubmitOutcome::NotSubmitted;

    static GpuReadbackResult fromException(GpuSubmitOutcome progress) noexcept {
        return {{},
                progress == GpuSubmitOutcome::NotSubmitted ? GpuSubmitOutcome::NotSubmitted
                                                           : GpuSubmitOutcome::SubmittedWithError};
    }
};

GpuReadbackResult submitGpuReadback(const std::shared_ptr<GpuRhiContext>& rhi,
                                    const std::shared_ptr<GpuSurface>& surface,
                                    FramePixelFormat target) noexcept;

namespace gpuReadbackDetail {
template <typename Fn>
GpuReadbackResult invoke(Fn&& fn) noexcept {
    GpuReadbackResult result;
    try {
        result.planes = std::invoke(std::forward<Fn>(fn), result.outcome);
    } catch (...) {
        return GpuReadbackResult::fromException(result.outcome);
    }
    return result;
}
} // namespace gpuReadbackDetail

#ifdef OLR_UNIT_TEST
enum class GpuReadbackTestBehavior : uint8_t {
    InvalidBeforeSubmission,
    ThrowBeforeSubmission,
    ThrowAfterSubmission
};
#endif

#if defined(OLR_UNIT_TEST) && defined(_WIN32)
struct D3D11RemovalObservationForTest {
    int64_t hresult = 0;
    uint64_t generation = 0;
};
#endif

// Owns the platform QRhi on a dedicated render thread. QRhi and imported GPU
// textures are thread-affine, so all RHI work funnels through this context.
class GpuRhiContext : public std::enable_shared_from_this<GpuRhiContext> {
public:
    static std::shared_ptr<GpuRhiContext> create();
    static std::shared_ptr<GpuRhiContext> createNullForTest();
    static std::shared_ptr<GpuRhiContext> createWarpForTest();
#ifdef OLR_UNIT_TEST
    static std::shared_ptr<GpuRhiContext> createInvalidForTest();
    static std::shared_ptr<GpuRhiContext>
    createReadbackForTest(GpuReadbackTestBehavior behavior,
                          std::shared_ptr<GpuFence> readbackFence = {}) {
        std::shared_ptr<GpuRhiContext> context = createNullForTest();
        if (!context) context = createWarpForTest();
        if (!context) context = createInvalidForTest();
        if (context) {
            context->m_readbackTestBehavior = behavior;
            if (readbackFence) context->m_readbackFence = std::move(readbackFence);
        }
        return context;
    }
    static std::shared_ptr<GpuRhiContext>
    createWithReadbackFenceForTest(std::shared_ptr<GpuFence> readbackFence) {
        std::shared_ptr<GpuRhiContext> context = create();
        if (context && readbackFence) context->m_readbackFence = std::move(readbackFence);
        return context;
    }
    static std::shared_ptr<GpuRhiContext> createReadbackFenceFailureForTest();
    int rhiReadbackCountForTest() const;
#ifdef _WIN32
    static uint64_t captureD3D11RemovalAuthorityForTest();
    static D3D11RemovalObservationForTest observeD3D11RemovalForTest(void* device,
                                                                     uint64_t deviceAuthorityEpoch);
    bool queueBlockingRenderJobForTest(const std::shared_ptr<QSemaphore>& entered,
                                       const std::shared_ptr<QSemaphore>& release,
                                       const std::shared_ptr<QSemaphore>& exited);
    static uint64_t quarantinedContextCountForTest();
    void injectPollOnlyDeviceLostForTest();
#endif
#endif
    ~GpuRhiContext();

    GpuRhiContext(const GpuRhiContext&) = delete;
    GpuRhiContext& operator=(const GpuRhiContext&) = delete;

    bool isValid() const;
    bool isNullBackend() const;
    bool isGpuBacked() const { return isValid() && !isNullBackend(); }
#ifdef __APPLE__
    // Immutable compatibility of the fence minted from this context's actual
    // QRhi Metal command queue. Empty for Null/invalid contexts or when native
    // queue/fence initialization failed.
    GpuSurfaceCompatibility surfaceCompatibility() const noexcept;
#endif

    bool invokeOnRenderThread(const std::function<void(QRhi*)>& job) const;
    // Run a UIKit/CAMetalLayer-touching present block on the platform's present
    // thread. iOS marshals to the main queue; macOS and stubs run inline.
    void presentOnMainThread(const std::function<void()>& block);
    bool deviceLost() const;
    // Rare-path authoritative poll used to upgrade a tokenless submission
    // failure before recovery decides whether dead-fence waits are legal.
    bool pollDeviceLoss() const;
    void injectDeviceLostForTest();
    std::shared_ptr<GpuFence> createFence() const;

private:
    class Impl;

    explicit GpuRhiContext(std::unique_ptr<Impl> impl,
                           std::function<std::shared_ptr<GpuFence>()> readbackFenceFactory = {},
                           std::shared_ptr<std::atomic<int>> injectedFactoryCalls = {});

    GpuReadbackResult importAndReadback(const GpuScopedNativeSurface& surface,
                                        FramePixelFormat target) noexcept;
    std::shared_ptr<GpuFence> readbackFence() const noexcept { return m_readbackFence; }

    friend GpuReadbackResult submitGpuReadback(const std::shared_ptr<GpuRhiContext>& rhi,
                                               const std::shared_ptr<GpuSurface>& surface,
                                               FramePixelFormat target) noexcept;
#ifdef OLR_UNIT_TEST
    friend struct GpuRhiContextTestAuthority;
#endif

#ifdef OLR_UNIT_TEST
    std::optional<GpuReadbackResult> injectedReadbackForTest() const noexcept {
        if (!m_readbackTestBehavior) return std::nullopt;
        return gpuReadbackDetail::invoke([&](GpuSubmitOutcome& outcome) -> CpuPlanes {
            switch (*m_readbackTestBehavior) {
            case GpuReadbackTestBehavior::InvalidBeforeSubmission:
                return {};
            case GpuReadbackTestBehavior::ThrowBeforeSubmission:
                throw 0;
            case GpuReadbackTestBehavior::ThrowAfterSubmission:
                outcome = GpuSubmitOutcome::Submitted;
                throw 0;
            }
            return {};
        });
    }
#endif

    std::unique_ptr<Impl> m_impl;
    std::shared_ptr<GpuFence> m_readbackFence;
#ifdef OLR_UNIT_TEST
    std::optional<GpuReadbackTestBehavior> m_readbackTestBehavior;
    int m_readbackFenceInitializationAttemptsForTest = 0;
    std::shared_ptr<std::atomic<int>> m_injectedReadbackFenceFactoryCallsForTest;
    std::atomic<bool> m_lastReadbackHadNativeHandleForTest{false};
    std::atomic<uint32_t> m_lastReadbackSubresourceForTest{0};
#endif
};

#ifdef OLR_UNIT_TEST
struct GpuRhiContextTestAuthority {
    static GpuReadbackResult importAndReadback(const std::shared_ptr<GpuRhiContext>& context,
                                               const std::shared_ptr<GpuSurface>& surface,
                                               FramePixelFormat target) noexcept {
        if (!context) return {};
        return submitGpuReadback(context, surface, target);
    }

    static std::shared_ptr<GpuFence>
    readbackFenceForTest(const std::shared_ptr<GpuRhiContext>& context) {
        if (!context) return nullptr;
        return context->m_readbackFence;
    }

    static int
    readbackFenceInitializationAttemptsForTest(const std::shared_ptr<GpuRhiContext>& context) {
        return context ? context->m_readbackFenceInitializationAttemptsForTest : 0;
    }

    static int
    injectedReadbackFenceFactoryCallsForTest(const std::shared_ptr<GpuRhiContext>& context) {
        return context && context->m_injectedReadbackFenceFactoryCallsForTest
                   ? context->m_injectedReadbackFenceFactoryCallsForTest->load(
                         std::memory_order_acquire)
                   : 0;
    }

    static bool lastReadbackHadNativeHandleForTest(const std::shared_ptr<GpuRhiContext>& context) {
        return context &&
               context->m_lastReadbackHadNativeHandleForTest.load(std::memory_order_acquire);
    }

    static uint32_t lastReadbackSubresourceForTest(const std::shared_ptr<GpuRhiContext>& context) {
        return context ? context->m_lastReadbackSubresourceForTest.load(std::memory_order_acquire)
                       : 0;
    }
};
#endif

#endif // OLR_GPURHICONTEXT_H

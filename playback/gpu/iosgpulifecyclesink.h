#ifndef OLR_IOSGPULIFECYCLESINK_H
#define OLR_IOSGPULIFECYCLESINK_H

#include <atomic>
#include <cstdint>

// iOS background/foreground GPU lifecycle sink. Platform-neutral: no UIKit types.
// Background records a device-loss epoch so workers release GPU surfaces through
// the same sanitize/rebuild path used for real device loss; foreground only
// clears suspension and lets the worker reacquire on its normal thread.
class IosGpuLifecycleSink {
public:
    virtual ~IosGpuLifecycleSink();
    virtual void onEnterBackground() = 0;
    virtual void onEnterForeground() = 0;
    virtual bool isSuspended() const = 0;
};

class DefaultIosGpuLifecycleSink : public IosGpuLifecycleSink {
public:
    void onEnterBackground() override;
    void onEnterForeground() override;

    bool isSuspended() const override;
    uint64_t generationAtLastBackground() const;

private:
    std::atomic<bool> m_suspended{false};
    std::atomic<uint64_t> m_bgGeneration{0};
};

void setIosGpuLifecycleSink(IosGpuLifecycleSink* sink);
IosGpuLifecycleSink* iosGpuLifecycleSink();

#endif // OLR_IOSGPULIFECYCLESINK_H

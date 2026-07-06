#ifndef OLR_OLRRHI_H
#define OLR_OLRRHI_H

#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QRhi;
class QRhiCommandBuffer;

// Thin RAII bring-up for the portable RHI spine. The Null backend is
// deterministic and headless, which keeps CI independent of a physical GPU.
// QRhi is single-threaded; one OlrRhi is owned by one render thread.
class OlrRhi {
public:
    enum class Backend { Null };

    static std::unique_ptr<OlrRhi> create(Backend backend, QString* error);

    virtual ~OlrRhi();

    OlrRhi(const OlrRhi&) = delete;
    OlrRhi& operator=(const OlrRhi&) = delete;

    QRhi* rhi() const { return m_rhi.get(); }

    bool runOffscreenFrame(const std::function<void(QRhiCommandBuffer*)>& record, QString* error);

    // Once a backend reports a removed/reset device, the instance stays lost.
    // Recovery is a fresh OlrRhi::create(), never a clear-in-place.
    bool deviceLost() const;
    void injectDeviceLostForTest();

protected:
    OlrRhi() = default;

    std::unique_ptr<QRhi> m_rhi;
    std::atomic<bool> m_deviceLost{false};
};

#endif // OLR_OLRRHI_H

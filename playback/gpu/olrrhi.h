#ifndef OLR_OLRRHI_H
#define OLR_OLRRHI_H

#include <QString>

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

protected:
    OlrRhi() = default;

    std::unique_ptr<QRhi> m_rhi;
};

#endif // OLR_OLRRHI_H

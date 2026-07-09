#include "playback/gpu/olrrhi.h"

#include <rhi/qrhi.h>

std::unique_ptr<OlrRhi> OlrRhi::create(Backend backend, QString* error) {
    auto self = std::unique_ptr<OlrRhi>(new OlrRhi());

    QRhi* raw = nullptr;
    switch (backend) {
    case Backend::Null: {
        QRhiNullInitParams params;
        raw = QRhi::create(QRhi::Null, &params);
        break;
    }
    }

    if (!raw) {
        if (error) {
            *error = QStringLiteral("QRhi::create failed for the requested backend");
        }
        return nullptr;
    }

    self->m_rhi.reset(raw);
    return self;
}

OlrRhi::~OlrRhi() = default;

bool OlrRhi::deviceLost() const {
    return m_deviceLost.load(std::memory_order_acquire);
}

void OlrRhi::injectDeviceLostForTest() {
    // Deterministic device-loss source for CI. The latch mirrors a removed
    // device: recovery is a fresh OlrRhi::create(), not a clear-in-place.
    m_deviceLost.store(true, std::memory_order_release);
}

bool OlrRhi::runOffscreenFrame(const std::function<void(QRhiCommandBuffer*)>& record,
                               QString* error) {
    if (!m_rhi) {
        if (error) {
            *error = QStringLiteral("OlrRhi has no QRhi");
        }
        return false;
    }

    QRhiCommandBuffer* cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb) {
        if (error) {
            *error = QStringLiteral("beginOffscreenFrame failed");
        }
        return false;
    }

    if (record) {
        record(cb);
    }

    if (m_rhi->endOffscreenFrame() != QRhi::FrameOpSuccess) {
        if (error) {
            *error = QStringLiteral("endOffscreenFrame failed");
        }
        return false;
    }

    return true;
}

// Real OMT output backend seam. The OMT SDK is not vendored: this translation
// unit is compiled only with OLR_WITH_OMT=ON and an integrator-provided
// OMT_SDK_DIR. With the flag off, omtsink.cpp provides StubOmtSenderBackend.
#ifdef OLR_WITH_OMT_BUILD

#include "playback/output/iotargets/omtsink.h"

namespace {

class SdkOmtSenderBackend final : public IOmtSenderBackend {
public:
    bool isRuntimeAvailable() const override { return false; }
    bool createSender(const QString&, FrameRate) override { return false; }
    void destroySender() override {}
    bool sendFrame(const OutputBusFrame&) override { return false; }
};

} // namespace

std::unique_ptr<IOmtSenderBackend> makeOmtSenderBackend() {
    return std::make_unique<SdkOmtSenderBackend>();
}

#endif // OLR_WITH_OMT_BUILD

// Real AJA NTV2 output backend seam. The NTV2 SDK is not vendored: this
// translation unit is compiled only with OLR_WITH_AJA=ON and an
// integrator-provided AJA_NTV2_DIR. With the flag off, ajasink.cpp provides
// StubAjaSenderBackend instead.
#ifdef OLR_WITH_AJA_BUILD

#include "playback/output/iotargets/ajasink.h"

namespace {

class SdkAjaSenderBackend final : public IAjaSenderBackend {
public:
    bool isRuntimeAvailable() const override { return false; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return false; }
    void closeDevice() override {}
    bool transferFrame(const OutputBusFrame&) override { return false; }
};

} // namespace

std::unique_ptr<IAjaSenderBackend> makeAjaSenderBackend() {
    return std::make_unique<SdkAjaSenderBackend>();
}

#endif // OLR_WITH_AJA_BUILD

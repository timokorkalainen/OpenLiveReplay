// Real Blackmagic DeckLink output backend seam. The DeckLink SDK is not vendored:
// this translation unit is compiled only with OLR_WITH_DECKLINK=ON and an
// integrator-provided DECKLINK_SDK_DIR. With the flag off, decklinksink.cpp
// provides StubDeckLinkSenderBackend instead.
#ifdef OLR_WITH_DECKLINK_BUILD

#include "playback/output/iotargets/decklinksink.h"

namespace {

class SdkDeckLinkSenderBackend final : public IDeckLinkSenderBackend {
public:
    bool isRuntimeAvailable() const override { return false; }
    bool deviceSupportsGpuTextureInput() const override { return false; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return false; }
    void closeDevice() override {}
    bool scheduleFrame(OutputBusFrame) override { return false; }
    bool scheduleGpuFrame(OutputBusFrame) override { return false; }
};

} // namespace

std::unique_ptr<IDeckLinkSenderBackend> makeDeckLinkSenderBackend() {
    return std::make_unique<SdkDeckLinkSenderBackend>();
}

#endif // OLR_WITH_DECKLINK_BUILD

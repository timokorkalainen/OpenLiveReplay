#include "recorder_engine/ingest/colortags_apple.h"

#ifdef __APPLE__

namespace {

CFStringRef attachmentString(CMFormatDescriptionRef fmt, CFStringRef key) {
    const void* value = CMFormatDescriptionGetExtension(fmt, key);
    return value && CFGetTypeID(value) == CFStringGetTypeID() ? static_cast<CFStringRef>(value)
                                                              : nullptr;
}

ColorMatrix mapMatrix(CFStringRef value, bool* known) {
    *known = true;
    if (CFEqual(value, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2))
        return ColorMatrix::Bt709;
    if (CFEqual(value, kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4))
        return ColorMatrix::Bt601;
    if (CFEqual(value, kCMFormatDescriptionYCbCrMatrix_SMPTE_240M_1995))
        return ColorMatrix::Bt709;
    if (CFEqual(value, kCMFormatDescriptionYCbCrMatrix_ITU_R_2020))
        return ColorMatrix::Bt2020;
    *known = false;
    return ColorMatrix::Bt709;
}

ColorPrimaries mapPrimaries(CFStringRef value, bool* known) {
    *known = true;
    if (CFEqual(value, kCMFormatDescriptionColorPrimaries_ITU_R_709_2))
        return ColorPrimaries::Bt709;
    if (CFEqual(value, kCMFormatDescriptionColorPrimaries_SMPTE_C))
        return ColorPrimaries::Bt601;
    if (CFEqual(value, kCMFormatDescriptionColorPrimaries_EBU_3213))
        return ColorPrimaries::Bt601;
    if (CFEqual(value, kCMFormatDescriptionColorPrimaries_ITU_R_2020))
        return ColorPrimaries::Bt2020;
    *known = false;
    return ColorPrimaries::Unspecified;
}

ColorTransfer mapTransfer(CFStringRef value, bool* known) {
    *known = true;
    if (CFEqual(value, kCMFormatDescriptionTransferFunction_ITU_R_709_2))
        return ColorTransfer::Bt709;
    if (CFEqual(value, kCMFormatDescriptionTransferFunction_SMPTE_240M_1995))
        return ColorTransfer::Bt709;
    if (CFEqual(value, kCMFormatDescriptionTransferFunction_ITU_R_2020))
        return ColorTransfer::Bt2020;
    *known = false;
    return ColorTransfer::Unspecified;
}

} // namespace

VuiColorInfo colorVuiFromFormatDescription(CMFormatDescriptionRef fmt) {
    VuiColorInfo out;
    if (!fmt) return out;

    bool any = false;
    if (CFStringRef matrix = attachmentString(fmt, kCMFormatDescriptionExtension_YCbCrMatrix)) {
        bool known = false;
        const ColorMatrix value = mapMatrix(matrix, &known);
        if (known) {
            out.matrix = value;
            any = true;
        }
    }
    if (CFStringRef primaries =
            attachmentString(fmt, kCMFormatDescriptionExtension_ColorPrimaries)) {
        bool known = false;
        const ColorPrimaries value = mapPrimaries(primaries, &known);
        if (known) {
            out.primaries = value;
            any = true;
        }
    }
    if (CFStringRef transfer =
            attachmentString(fmt, kCMFormatDescriptionExtension_TransferFunction)) {
        bool known = false;
        const ColorTransfer value = mapTransfer(transfer, &known);
        if (known) {
            out.transfer = value;
            any = true;
        }
    }
    const void* fullRange =
        CMFormatDescriptionGetExtension(fmt, kCMFormatDescriptionExtension_FullRangeVideo);
    if (fullRange && CFGetTypeID(fullRange) == CFBooleanGetTypeID()) {
        out.range =
            CFBooleanGetValue(static_cast<CFBooleanRef>(fullRange)) ? ColorRange::Full
                                                                    : ColorRange::Video;
        any = true;
    }
    out.present = any;
    return out;
}

#endif // __APPLE__

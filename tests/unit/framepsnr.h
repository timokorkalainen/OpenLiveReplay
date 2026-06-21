#ifndef OLR_TEST_FRAMEPSNR_H
#define OLR_TEST_FRAMEPSNR_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// Peak signal-to-noise ratio (dB) of an 8-bit luma plane against a reference, honoring
// per-row strides (decoded frames carry padded linesize). Broadcast convention: Y-only PSNR.
// Returns +inf for identical planes (MSE == 0), 0 for an empty region.
inline double psnrY8(const uint8_t* ref, int refStride, const uint8_t* tst, int tstStride,
                     int width, int height) {
    if (width <= 0 || height <= 0 || refStride < width || tstStride < width) return 0.0;
    double sse = 0.0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* r = ref + static_cast<std::size_t>(y) * refStride;
        const uint8_t* t = tst + static_cast<std::size_t>(y) * tstStride;
        for (int x = 0; x < width; ++x) {
            const double d = static_cast<double>(r[x]) - static_cast<double>(t[x]);
            sse += d * d;
        }
    }
    if (sse == 0.0) return std::numeric_limits<double>::infinity();
    const double mse = sse / (static_cast<double>(width) * static_cast<double>(height));
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

#endif // OLR_TEST_FRAMEPSNR_H

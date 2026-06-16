#include "nativefallbackpolicy.h"

#include <Qt>

bool nativeDecodeErrorRequestsFallback(const QString& error) {
    // Stream-start decode errors can be transient, such as access units arriving
    // before SPS/PPS/VPS parameter sets. Only capability-style failures should
    // suppress native ingest for the next retry.
    static constexpr const char* kFallbackNeedles[] = {
        "decompression session creation failed",
        "unavailable",
        "not implemented",
        "unsupported",
        "decoder is unavailable",
    };

    for (const char* needle : kFallbackNeedles) {
        if (error.contains(QString::fromLatin1(needle), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

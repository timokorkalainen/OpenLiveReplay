#include "playback/gpu/iosmemoryheadroom.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IOS
#include <os/proc.h>
#endif

uint64_t iosAvailableMemoryBytes() {
#if defined(__APPLE__) && TARGET_OS_IOS
    if (__builtin_available(iOS 13.0, *)) {
        return static_cast<uint64_t>(os_proc_available_memory());
    }
#endif
    return 0;
}

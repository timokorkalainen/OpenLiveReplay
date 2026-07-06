#import "ios/iosgpulifecycle.h"

#ifdef __APPLE__
#import <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IOS

#import <UIKit/UIKit.h>

#include "playback/gpu/iosgpulifecyclesink.h"

#include <atomic>

namespace {

std::atomic<bool> g_installed{false};
id g_bgObserver = nil;
id g_fgObserver = nil;

} // namespace

extern "C" void installIosGpuLifecycle(void) {
    // MAIN-THREAD: UIKit notification registration must run on the main thread.
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;

    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    g_bgObserver = [nc addObserverForName:UIApplicationDidEnterBackgroundNotification
                                   object:nil
                                    queue:[NSOperationQueue mainQueue]
                               usingBlock:^(NSNotification*) {
                                 // MAIN-THREAD: forward to the platform-neutral sink.
                                 if (auto* sink = iosGpuLifecycleSink()) sink->onEnterBackground();
                               }];
    g_fgObserver = [nc addObserverForName:UIApplicationWillEnterForegroundNotification
                                   object:nil
                                    queue:[NSOperationQueue mainQueue]
                               usingBlock:^(NSNotification*) {
                                 // MAIN-THREAD: forward to the platform-neutral sink.
                                 if (auto* sink = iosGpuLifecycleSink()) sink->onEnterForeground();
                               }];
}

#else

extern "C" void installIosGpuLifecycle(void) {}

#endif

#include "macos/macos_window_activation.h"

#import <AppKit/AppKit.h>
#include <cstdlib>

void olrPrepareMacWindowActivation() {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (std::getenv("OLR_APP_E2E_VISUAL_DEBUG")) {
        NSLog(@"OLR_APP_WINDOW_DIAG activationPolicy=%ld", long([NSApp activationPolicy]));
    }
}

void olrRequestMacWindowActivation(WId windowId) {
    [NSApp activateIgnoringOtherApps:YES];
    auto* view = reinterpret_cast<NSView*>(windowId);
    NSWindow* window = [view window];
    if (std::getenv("OLR_APP_E2E_VISUAL_DEBUG")) {
        NSLog(@"OLR_APP_WINDOW_DIAG nativeView=%p nativeWindow=%p visible=%d", view, window,
              window ? [window isVisible] : 0);
    }
    if (window) {
        [window makeKeyAndOrderFront:nil];
        [window orderFrontRegardless];
        if (std::getenv("OLR_APP_E2E_VISUAL_DEBUG")) {
            NSLog(@"OLR_APP_WINDOW_DIAG nativeWindowAfter visible=%d key=%d main=%d",
                  [window isVisible], [window isKeyWindow], [window isMainWindow]);
        }
    }
}

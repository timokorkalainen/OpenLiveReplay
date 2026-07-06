#import <UIKit/UIKit.h>

#ifdef OLR_GPU_PIPELINE_BUILD
#import "ios/iosgpulifecycle.h"

#include "playback/gpu/gpupipelineconfig.h"
#endif

extern "C" void installIosGpuLifecycleIfEnabled(void) {
#ifdef OLR_GPU_PIPELINE_BUILD
    if (gpuPipelineEnabled()) {
        // MAIN-THREAD: called from the Qt/UIKit bootstrap after QGuiApplication exists.
        installIosGpuLifecycle();
    }
#endif
}

extern "C" void requestIosNewScene(void)
{
    if (@available(iOS 13.0, *)) {
        UIApplication *app = [UIApplication sharedApplication];
        if (!app) return;

        UISceneActivationRequestOptions *options = [[UISceneActivationRequestOptions alloc] init];
        [app requestSceneSessionActivation:nil
                               userActivity:nil
                                     options:options
                                errorHandler:^(NSError * _Nonnull error) {
                                    NSLog(@"OpenLiveReplay: scene activation failed: %@", error);
                                }];
    }
}

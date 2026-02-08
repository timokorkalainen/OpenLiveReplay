#import <UIKit/UIKit.h>

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

#import <UIKit/UIKit.h>
#include "lease_fixture.h"
#include "entropy.h"

extern "C" __attribute__((used,visibility("default"))) BOOL madmedia_preview_ios_lease() {
    // Foundation supplies the host app's own temporary directory. mkdtemp
    // creates a distinct sandbox-local subtree, never a caller-selected path.
    NSString* pattern = [NSTemporaryDirectory() stringByAppendingPathComponent:@"madmedia-lease-XXXXXX"];
    std::string copy(pattern.fileSystemRepresentation);
    char* directory = mkdtemp(copy.data());
    if (!directory) return NO;
    bool good = apple_preview::exercise(directory);
    NSString* cleanup = [NSFileManager.defaultManager stringWithFileSystemRepresentation:directory
                                                                                 length:copy.size()];
    BOOL removed = [NSFileManager.defaultManager removeItemAtPath:cleanup error:nullptr];
    return good && removed;
}

extern "C" __attribute__((used,visibility("default"))) BOOL madmedia_preview_ios_entropy() {
    std::array<unsigned char,16> first{}, second{}, zero{};
    return madmedia_platform::entropy128(first) && madmedia_platform::entropy128(second) &&
           first != zero && second != zero && first != second;
}

@interface PreviewAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation PreviewAppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)app; (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [UIViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end
int main(int argc, char* argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(PreviewAppDelegate.class)); }
}

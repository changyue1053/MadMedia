#import <UIKit/UIKit.h>
#include <TargetConditionals.h>
#include <array>
#include <cstdlib>
#include "journal_probe.h"
#include "journal_rollback.h"
#include "entropy.h"

#if !TARGET_OS_SIMULATOR || !defined(__arm64__)
#error Preview requires native ARM64 iOS Simulator
#endif
@interface PreviewAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation PreviewAppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)app; (void)options;
    self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController=[UIViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end
int main(int argc,char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(PreviewAppDelegate.class)); }
}
static BOOL expectedRuntime() {
    const auto v=NSProcessInfo.processInfo.operatingSystemVersion;
    return v.majorVersion==26 && v.minorVersion==2;
}
extern "C" __attribute__((used,visibility("default"))) BOOL madmedia_preview_journal() {
    if (!expectedRuntime()) return NO;
    NSString *pattern=[NSTemporaryDirectory() stringByAppendingPathComponent:@"madmedia-journal-XXXXXX"];
    std::string dir(pattern.fileSystemRepresentation);
    if (!mkdtemp(dir.data())) return NO;
    bool ok=exerciseInstalledJournal(dir+"/preview.sqlite");
    NSString *owned= [NSFileManager.defaultManager stringWithFileSystemRepresentation:dir.c_str()
                                                                                length:dir.size()];
    return [NSFileManager.defaultManager removeItemAtPath:owned error:nullptr] && ok;
}
extern "C" __attribute__((used,visibility("default"))) BOOL madmedia_preview_entropy() {
    std::array<unsigned char,16> first{}, second{}, zero{};
    return expectedRuntime() && madmedia_platform::entropy128(first) &&
        madmedia_platform::entropy128(second) && first!=zero && second!=zero && first!=second;
}

extern "C" __attribute__((used,visibility("default"))) BOOL madmedia_preview_journal_rollback() {
    if (!expectedRuntime()) return NO;
    NSString *pattern=[NSTemporaryDirectory() stringByAppendingPathComponent:@"madmedia-rollback-XXXXXX"];
    const char *local=pattern.fileSystemRepresentation;
    if (!local) return NO;
    std::string dir(local);
    if (!mkdtemp(dir.data())) return NO;
    const bool ok=exerciseInstalledJournalRollback(dir+"/preview.sqlite");
    NSString *owned=[NSFileManager.defaultManager stringWithFileSystemRepresentation:dir.c_str()
                                                                               length:dir.size()];
    return owned && [NSFileManager.defaultManager removeItemAtPath:owned error:nullptr] && ok;
}

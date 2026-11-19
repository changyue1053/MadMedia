#import <XCTest/XCTest.h>
#include <TargetConditionals.h>

#if !TARGET_OS_SIMULATOR || !defined(__arm64__)
#error This test bundle requires an ARM64 iOS simulator.
#endif

extern "C" BOOL madmedia_preview_ios_lease();
extern "C" BOOL madmedia_preview_ios_entropy();

@interface MadMediaCapabilityTests : XCTestCase
@end
@implementation MadMediaCapabilityTests
- (void)testAppPrivateAdvisoryLease { XCTAssertTrue(madmedia_preview_ios_lease()); }
- (void)testActualAppleEntropy { XCTAssertTrue(madmedia_preview_ios_entropy()); }
@end

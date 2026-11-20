#import <XCTest/XCTest.h>
extern "C" const char *madmedia_preview_roundtrip();
extern "C" BOOL madmedia_preview_contract();

@interface PreviewTests : XCTestCase
@end
@implementation PreviewTests
- (void)testInstalledImageRoundTrip {
    const char *reason=madmedia_preview_roundtrip();
    if (reason) XCTFail(@"[MADMEDIA-ROUNDTRIP:%s]",reason);
}
- (void)testInstalledImageBoundsAndCapacity { XCTAssertTrue(madmedia_preview_contract()); }
@end

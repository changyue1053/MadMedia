#import <XCTest/XCTest.h>
extern "C" BOOL madmedia_preview_roundtrip();
extern "C" BOOL madmedia_preview_contract();

@interface PreviewTests : XCTestCase
@end
@implementation PreviewTests
- (void)testInstalledImageRoundTrip { XCTAssertTrue(madmedia_preview_roundtrip()); }
- (void)testInstalledImageBoundsAndCapacity { XCTAssertTrue(madmedia_preview_contract()); }
@end

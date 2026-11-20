#import <XCTest/XCTest.h>
extern "C" BOOL madmedia_preview_journal();
extern "C" BOOL madmedia_preview_entropy();
@interface PreviewTests : XCTestCase
@end
@implementation PreviewTests
- (void)testInstalledJournalCommitCloseReopen { XCTAssertTrue(madmedia_preview_journal()); }
- (void)testActualAppleEntropyProvider { XCTAssertTrue(madmedia_preview_entropy()); }
@end

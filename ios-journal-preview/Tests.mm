#import <XCTest/XCTest.h>
extern "C" BOOL madmedia_preview_journal();
extern "C" BOOL madmedia_preview_entropy();
extern "C" BOOL madmedia_preview_journal_rollback();
@interface PreviewTests : XCTestCase
@end
@implementation PreviewTests
- (void)testInstalledJournalRollbackAndSameHandleRecovery { XCTAssertTrue(madmedia_preview_journal_rollback()); }
- (void)testInstalledJournalCommitCloseReopen { XCTAssertTrue(madmedia_preview_journal()); }
- (void)testActualAppleEntropyProvider { XCTAssertTrue(madmedia_preview_entropy()); }
@end

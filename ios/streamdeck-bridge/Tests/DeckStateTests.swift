import XCTest
@testable import StreamDeckBridge

final class DeckStateTests: XCTestCase {

    func testBridgeSingletonExists() {
        XCTAssertNotNil(OLRStreamDeckBridge.shared)
    }
}

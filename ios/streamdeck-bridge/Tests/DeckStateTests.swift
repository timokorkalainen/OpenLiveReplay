import Combine
import XCTest
@testable import StreamDeckBridge

final class DeckStateTests: XCTestCase {

    private var cancellables = Set<AnyCancellable>()

    override func tearDown() {
        cancellables.removeAll()
        super.tearDown()
    }

    private func changeCount(of state: DeckState, during body: () -> Void) -> Int {
        var count = 0
        state.objectWillChange.sink { _ in count += 1 }.store(in: &cancellables)
        body()
        return count
    }

    // MARK: DeckState change guarding

    func testUnchangedRecordingStateDoesNotPublish() {
        let state = DeckState()
        state.setRecording(true, elapsedText: "00:00:01")
        let count = changeCount(of: state) {
            state.setRecording(true, elapsedText: "00:00:01")
        }
        XCTAssertEqual(count, 0)
    }

    func testChangedElapsedPublishesExactlyOnce() {
        let state = DeckState()
        state.setRecording(true, elapsedText: "00:00:01")
        let count = changeCount(of: state) {
            state.setRecording(true, elapsedText: "00:00:02")
        }
        XCTAssertEqual(count, 1)
    }

    func testUnchangedTransportDoesNotPublish() {
        let state = DeckState()
        state.setTransport(playing: true, speedText: "1.0×", followLive: false)
        let count = changeCount(of: state) {
            state.setTransport(playing: true, speedText: "1.0×", followLive: false)
        }
        XCTAssertEqual(count, 0)
    }

    func testTinyPositionFractionChangeDoesNotPublish() {
        let state = DeckState()
        state.setPosition(timecodeText: "00:00:00:01", positionFraction: 0.5)
        let count = changeCount(of: state) {
            state.setPosition(timecodeText: "00:00:00:01", positionFraction: 0.5001)
        }
        XCTAssertEqual(count, 0)
    }

    // MARK: Key mapping lookup

    func testActionForKeyUsesModelMapping() {
        let state = DeckState()
        state.setKeyMapping([9, 0, 4, -1], forModel: "mini")
        XCTAssertEqual(state.action(forKey: 0, model: "mini"), .record)
        XCTAssertEqual(state.action(forKey: 1, model: "mini"), .playPause)
        XCTAssertNil(state.action(forKey: 3, model: "mini"))
        XCTAssertNil(state.action(forKey: 99, model: "mini"))
        XCTAssertNil(state.action(forKey: 0, model: "xl"))
    }

    // MARK: Default profiles

    func testDefaultMappingMini() {
        XCTAssertEqual(
            DeckAction.defaultMapping(modelIdentifier: "mini", keyCount: 6),
            [9, 0, 4, 5, 7, 3]
        )
    }

    func testDefaultMappingPedal() {
        XCTAssertEqual(
            DeckAction.defaultMapping(modelIdentifier: "pedal", keyCount: 3),
            [0, 7, 3]
        )
    }

    func testDefaultMappingRegularPadsWithEmpty() {
        let mapping = DeckAction.defaultMapping(modelIdentifier: "regular", keyCount: 15)
        XCTAssertEqual(mapping.count, 15)
        XCTAssertEqual(Array(mapping.prefix(11)), [9, 0, 4, 5, 7, 3, 1, 2, 6, 20, 21])
        XCTAssertEqual(Array(mapping.suffix(4)), [-1, -1, -1, -1])
    }

    func testDefaultMappingPlusTopEight() {
        XCTAssertEqual(
            DeckAction.defaultMapping(modelIdentifier: "plus", keyCount: 8),
            [9, 0, 4, 5, 7, 3, 1, 2]
        )
    }
}

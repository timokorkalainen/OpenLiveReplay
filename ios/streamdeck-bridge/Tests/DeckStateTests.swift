import Combine
import XCTest
@testable import StreamDeckBridge

@MainActor
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

    func testUnchangedKeyMappingDoesNotPublish() {
        let state = DeckState()
        state.setKeyMapping([9, 0], forModel: "mini")
        let count = changeCount(of: state) {
            state.setKeyMapping([9, 0], forModel: "mini")
        }
        XCTAssertEqual(count, 0)
    }

    func testChangedTimecodeAlonePublishesExactlyOnce() {
        let state = DeckState()
        state.setPosition(timecodeText: "00:00:00:01", positionFraction: 0.5)
        let count = changeCount(of: state) {
            state.setPosition(timecodeText: "00:00:00:02", positionFraction: 0.5)
        }
        XCTAssertEqual(count, 1)
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

    func testDefaultMappingPedalClampsToKeyCount() {
        XCTAssertEqual(
            DeckAction.defaultMapping(modelIdentifier: "pedal", keyCount: 2),
            [0, 7]
        )
    }

    func testDefaultMappingPlusTopEight() {
        XCTAssertEqual(
            DeckAction.defaultMapping(modelIdentifier: "plus", keyCount: 8),
            [9, 0, 4, 5, 7, 3, 1, 2]
        )
    }

    // MARK: KeyContent derivation

    func testKeyContentForUnmappedKeyIsEmpty() {
        let state = DeckState()
        XCTAssertEqual(KeyContent(state: state, keyIndex: 0, model: "mini"), .empty)
    }

    func testKeyContentRecordIdleShowsLabel() {
        let state = DeckState()
        state.setKeyMapping([9], forModel: "mini")
        let content = KeyContent(state: state, keyIndex: 0, model: "mini")
        XCTAssertEqual(content.action, .record)
        XCTAssertFalse(content.isActive)
        XCTAssertEqual(content.title, "REC")
        XCTAssertEqual(content.symbolName, "record.circle")
    }

    func testKeyContentRecordActiveShowsElapsed() {
        let state = DeckState()
        state.setKeyMapping([9], forModel: "mini")
        state.setRecording(true, elapsedText: "00:05:23")
        let content = KeyContent(state: state, keyIndex: 0, model: "mini")
        XCTAssertTrue(content.isActive)
        XCTAssertEqual(content.title, "00:05:23")
    }

    func testKeyContentTimecodeDisplayCarriesTimecode() {
        let state = DeckState()
        state.setKeyMapping([20], forModel: "regular")
        state.setPosition(timecodeText: "01:02:03:04", positionFraction: 0.5)
        let content = KeyContent(state: state, keyIndex: 0, model: "regular")
        XCTAssertEqual(content.action, .timecodeDisplay)
        XCTAssertEqual(content.title, "01:02:03:04")
        XCTAssertNil(content.symbolName)
    }

    func testKeyContentSpeedDisplayCarriesSpeed() {
        let state = DeckState()
        state.setKeyMapping([21], forModel: "regular")
        state.setTransport(playing: true, speedText: "-5.0×", followLive: false)
        let content = KeyContent(state: state, keyIndex: 0, model: "regular")
        XCTAssertEqual(content.action, .speedDisplay)
        XCTAssertEqual(content.title, "-5.0×")
    }

    func testKeyContentPlayPauseAndGoLiveActiveFlags() {
        let state = DeckState()
        state.setKeyMapping([0, 4], forModel: "mini")
        state.setTransport(playing: true, speedText: "1.0×", followLive: true)
        XCTAssertTrue(KeyContent(state: state, keyIndex: 0, model: "mini").isActive)
        XCTAssertTrue(KeyContent(state: state, keyIndex: 1, model: "mini").isActive)
    }

    func testDefaultMappingPedalPadsWhenMoreKeys() {
        XCTAssertEqual(
            DeckAction.defaultMapping(modelIdentifier: "pedal", keyCount: 4),
            [0, 7, 3, -1]
        )
    }
}

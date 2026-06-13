import Combine
import Foundation

/// Single source of truth for everything rendered on connected decks.
/// All setters guard against no-op writes, so a no-op state push never
/// publishes (idle deck → zero re-renders). Per-key USB economy is the
/// layout's responsibility: the container view is static and each leaf
/// subscribes with an equality guard on its own content slice (see
/// ReplayDeckLayout).
@MainActor
final class DeckState: ObservableObject {

    /// Sub-pixel on the widest (1200 px) touch strip — can never suppress
    /// a visible scrub-bar change.
    private static let positionEpsilon = 0.0005

    /// model identifier ("mini", "plus", ...) → key-index → action id.
    @Published private(set) var keyMappings: [String: [Int]] = [:]
    @Published private(set) var isRecording = false
    @Published private(set) var recElapsedText = ""
    @Published private(set) var isPlaying = false
    @Published private(set) var speedText = ""
    @Published private(set) var followLive = false
    @Published private(set) var timecodeText = "00:00:00:00"
    @Published private(set) var positionFraction: Double = 0

    func setKeyMapping(_ mapping: [Int], forModel model: String) {
        if keyMappings[model] != mapping { keyMappings[model] = mapping }
    }

    func setRecording(_ recording: Bool, elapsedText: String) {
        if isRecording != recording { isRecording = recording }
        if recElapsedText != elapsedText { recElapsedText = elapsedText }
    }

    func setTransport(playing: Bool, speedText: String, followLive: Bool) {
        if isPlaying != playing { isPlaying = playing }
        if self.speedText != speedText { self.speedText = speedText }
        if self.followLive != followLive { self.followLive = followLive }
    }

    func setPosition(timecodeText: String, positionFraction: Double) {
        if self.timecodeText != timecodeText { self.timecodeText = timecodeText }
        if abs(self.positionFraction - positionFraction) > Self.positionEpsilon {
            self.positionFraction = positionFraction
        }
    }

    func action(forKey index: Int, model: String) -> DeckAction? {
        guard let mapping = keyMappings[model],
              index >= 0, index < mapping.count else { return nil }
        return DeckAction(rawValue: mapping[index])
    }
}

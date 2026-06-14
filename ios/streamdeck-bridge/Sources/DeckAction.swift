import Foundation

/// Action ids shared with the Qt side.
/// Keep in sync with streamdeck/streamdeckmanager.h and
/// UIManager::dispatchControlAction in uimanager.cpp.
enum DeckAction: Int {
    case playPause = 0
    case rewind5x = 1
    case forward5x = 2
    case stepForward = 3
    case goLive = 4
    case capture = 5
    case multiview = 6
    case stepBackward = 7
    case jog = 8            // dial rotation only, never on a key
    case record = 9
    case shuttle = 10       // dial rotation only, variable-speed
    case timecodeDisplay = 20
    case speedDisplay = 21
    // Feed/camera select (parity with the MIDI mapping). Ids 100..107 ->
    // UIManager dispatches feedSelectRequested(id - 100).
    case feed1 = 100
    case feed2 = 101
    case feed3 = 102
    case feed4 = 103
    case feed5 = 104
    case feed6 = 105
    case feed7 = 106
    case feed8 = 107
}

extension DeckAction {

    var symbolName: String {
        switch self {
        case .playPause: return "playpause.fill"
        case .rewind5x: return "backward.fill"
        case .forward5x: return "forward.fill"
        case .stepForward: return "forward.frame.fill"
        case .goLive: return "dot.radiowaves.left.and.right"
        case .capture: return "camera.fill"
        case .multiview: return "square.grid.2x2.fill"
        case .stepBackward: return "backward.frame.fill"
        case .jog: return "dial.medium.fill"
        case .record: return "record.circle"
        case .shuttle: return "dial.high.fill"
        case .timecodeDisplay: return "timer"
        case .speedDisplay: return "gauge.with.needle"
        case .feed1, .feed2, .feed3, .feed4, .feed5, .feed6, .feed7, .feed8:
            return "\(rawValue - 99).square.fill"
        }
    }

    var label: String {
        switch self {
        case .playPause: return "Play"
        case .rewind5x: return "-5×"
        case .forward5x: return "+5×"
        case .stepForward: return "+1f"
        case .goLive: return "LIVE"
        case .capture: return "Capture"
        case .multiview: return "Multi"
        case .stepBackward: return "-1f"
        case .jog: return "Jog"
        case .record: return "REC"
        case .shuttle: return "Shuttle"
        case .timecodeDisplay: return ""
        case .speedDisplay: return ""
        case .feed1, .feed2, .feed3, .feed4, .feed5, .feed6, .feed7, .feed8:
            return "Feed \(rawValue - 99)"
        }
    }

    /// Model identifier for the key-less Stream Deck Pedal (must match
    /// deckModelIdentifier(for:) in OLRStreamDeckBridge.swift).
    static let pedalModelIdentifier = "pedal"

    /// Keys are filled from this list, top priority first.
    static let keyPriority: [DeckAction] = [
        .record, .playPause, .goLive, .capture,
        .stepBackward, .stepForward, .rewind5x, .forward5x,
        .multiview, .timecodeDisplay, .speedDisplay,
    ]

    /// Default key-index → action-id table for a device shape.
    /// The Qt side can override per model via
    /// OLRStreamDeckBridge.setKeyMapping(_:forModel:) (future remapping).
    static func defaultMapping(modelIdentifier: String, keyCount: Int) -> [Int] {
        guard keyCount > 0 else { return [] }
        if modelIdentifier == pedalModelIdentifier {
            // Foot switches, no displays: play/pause, step back, step forward.
            let pedalActions = [DeckAction.playPause.rawValue,
                                DeckAction.stepBackward.rawValue,
                                DeckAction.stepForward.rawValue]
            if pedalActions.count >= keyCount {
                return Array(pedalActions.prefix(keyCount))
            }
            return pedalActions + Array(repeating: -1, count: keyCount - pedalActions.count)
        }
        var mapping = keyPriority.prefix(keyCount).map(\.rawValue)
        if mapping.count < keyCount {
            mapping.append(contentsOf: Array(repeating: -1, count: keyCount - mapping.count))
        }
        return mapping
    }
}

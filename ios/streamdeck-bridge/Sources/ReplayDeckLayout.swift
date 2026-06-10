import StreamDeckKit
import SwiftUI

/// Adaptive layout rendered on every connected Stream Deck.
///
/// Observation granularity: ONLY this container observes DeckState. Each key
/// receives a value-type KeyContent and leaf views are Equatable, so SwiftUI
/// skips re-evaluating keys whose content is unchanged and the SDK
/// re-transmits only those keys over USB. A ticking timecode re-renders one
/// key, not the deck.
struct ReplayDeckLayout: View {

    @Environment(\.streamDeckViewContext.device) var streamDeck
    @EnvironmentObject var state: DeckState

    var body: some View {
        StreamDeckLayout {
            StreamDeckKeyAreaLayout { keyIndex in
                ReplayKeyView(content: keyContent(forKey: keyIndex))
                    .equatable()
            }
            .background(.black)
        } windowArea: {
            switch streamDeck.info.product {
            case .plus, .plusXL:
                ReplayDialStrip(
                    fraction: state.positionFraction,
                    timecodeText: state.timecodeText,
                    dialCount: max(streamDeck.capabilities.dialCount, 1)
                )
            case .neo:
                ReplayNeoPanel(
                    isRecording: state.isRecording,
                    timecodeText: state.timecodeText
                )
            default:
                Color.black
            }
        }
    }

    private func keyContent(forKey index: Int) -> KeyContent {
        let model = deckModelIdentifier(for: streamDeck)
        guard let action = state.action(forKey: index, model: model) else {
            return KeyContent(action: nil, isActive: false, title: "", symbolName: nil)
        }
        switch action {
        case .timecodeDisplay:
            return KeyContent(action: action, isActive: false,
                              title: state.timecodeText, symbolName: nil)
        case .speedDisplay:
            return KeyContent(action: action, isActive: false,
                              title: state.speedText, symbolName: nil)
        case .record:
            return KeyContent(action: action, isActive: state.isRecording,
                              title: state.isRecording ? state.recElapsedText : action.label,
                              symbolName: action.symbolName)
        case .playPause:
            return KeyContent(action: action, isActive: state.isPlaying,
                              title: action.label, symbolName: action.symbolName)
        case .goLive:
            return KeyContent(action: action, isActive: state.followLive,
                              title: action.label, symbolName: action.symbolName)
        default:
            return KeyContent(action: action, isActive: false,
                              title: action.label, symbolName: action.symbolName)
        }
    }
}

/// Everything a key renders, as a value: equal content means identical
/// pixels, so Equatable pruning keeps unchanged keys off the USB bus.
struct KeyContent: Equatable {
    let action: DeckAction?
    let isActive: Bool
    let title: String
    let symbolName: String?
}

/// One key. Equatable on content only (@State isPressed excluded — SwiftUI
/// re-renders on its own state changes regardless of ==).
struct ReplayKeyView: View, Equatable {

    let content: KeyContent
    @State private var isPressed = false

    static func == (lhs: ReplayKeyView, rhs: ReplayKeyView) -> Bool {
        lhs.content == rhs.content
    }

    var body: some View {
        if let action = content.action {
            switch action {
            case .timecodeDisplay:
                DisplayKey(title: "TC", value: content.title)
            case .speedDisplay:
                DisplayKey(title: "SPEED", value: content.title)
            default:
                actionKey(for: action)
            }
        } else {
            Color.black
        }
    }

    private var activeColor: Color {
        content.action == .record ? .red : .green
    }

    private func actionKey(for action: DeckAction) -> some View {
        StreamDeckKeyView { pressed in
            isPressed = pressed
            OLRStreamDeckBridge.shared.emitAction(action.rawValue, pressed: pressed)
        } content: {
            VStack(spacing: 2) {
                if let symbolName = content.symbolName {
                    Image(systemName: symbolName)
                        .font(.system(size: 22, weight: .bold))
                }
                Text(content.title)
                    .font(.system(size: 11, weight: .semibold).monospacedDigit())
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .foregroundColor(content.isActive ? .white : Color(white: 0.75))
            .background(content.isActive
                        ? activeColor.opacity(isPressed ? 0.9 : 0.6)
                        : Color(white: isPressed ? 0.35 : 0.12))
        }
    }
}

/// A non-interactive value display key (timecode, speed).
struct DisplayKey: View {

    let title: String
    let value: String

    var body: some View {
        VStack(spacing: 2) {
            Text(title)
                .font(.system(size: 9, weight: .bold))
                .foregroundColor(Color(white: 0.5))
            Text(value)
                .font(.system(size: 12, weight: .bold).monospacedDigit())
                .foregroundColor(.white)
                .minimumScaleFactor(0.5)
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(white: 0.05))
    }
}

/// Stream Deck + / + XL touch strip: dial 0 jogs (press = play/pause),
/// the whole strip is a continuous scrub bar; tapping it seeks.
/// Receives plain values from the observing container.
struct ReplayDialStrip: View {

    let fraction: Double
    let timecodeText: String
    let dialCount: Int

    @Environment(\.streamDeckViewContext.size) var stripSize

    var body: some View {
        StreamDeckDialAreaLayout(
            rotate: { dialIndex, rotation in
                if dialIndex == 0 {
                    OLRStreamDeckBridge.shared.emitJog(rotation)
                }
            },
            press: { dialIndex, pressed in
                if dialIndex == 0 {
                    OLRStreamDeckBridge.shared.emitAction(
                        DeckAction.playPause.rawValue, pressed: pressed)
                }
            },
            touch: { location in
                guard stripSize.width > 0 else { return }
                let f = min(max(location.x / stripSize.width, 0), 1)
                OLRStreamDeckBridge.shared.emitScrub(Double(f))
            }
        ) { dialIndex in
            ScrubBarSegment(
                segmentIndex: dialIndex,
                segmentCount: dialCount,
                fraction: fraction,
                timecodeText: dialIndex == 0 ? timecodeText : "")
        }
    }
}

/// One dial-width segment of the continuous scrub bar. Segment i fills for
/// the global fraction range [i/n, (i+1)/n]; segment 0 overlays the timecode.
struct ScrubBarSegment: View {

    let segmentIndex: Int
    let segmentCount: Int
    let fraction: Double
    let timecodeText: String

    var body: some View {
        GeometryReader { geo in
            let globalProgress = fraction * Double(segmentCount)
            let localFill = min(max(globalProgress - Double(segmentIndex), 0), 1)
            ZStack(alignment: .leading) {
                Rectangle().fill(Color(white: 0.15))
                Rectangle()
                    .fill(Color.blue)
                    .frame(width: geo.size.width * localFill)
                if segmentIndex == 0 {
                    VStack(alignment: .leading, spacing: 0) {
                        Text("JOG / SCRUB")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundColor(Color(white: 0.6))
                        Text(timecodeText)
                            .font(.system(size: 14, weight: .bold).monospacedDigit())
                            .foregroundColor(.white)
                    }
                    .padding(.leading, 8)
                }
            }
        }
    }
}

/// Stream Deck Neo: the two touch keys step a frame back/forward, the info
/// panel shows REC state and timecode. Receives plain values.
struct ReplayNeoPanel: View {

    let isRecording: Bool
    let timecodeText: String

    var body: some View {
        StreamDeckNeoPanelLayout { touched in
            OLRStreamDeckBridge.shared.emitAction(
                DeckAction.stepBackward.rawValue, pressed: touched)
        } rightTouch: { touched in
            OLRStreamDeckBridge.shared.emitAction(
                DeckAction.stepForward.rawValue, pressed: touched)
        } panel: {
            HStack(spacing: 6) {
                if isRecording {
                    Circle().fill(.red).frame(width: 8, height: 8)
                }
                Text(timecodeText)
                    .font(.system(size: 14, weight: .bold).monospacedDigit())
                    .foregroundColor(.white)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(white: 0.05))
        }
    }
}

#if DEBUG
import StreamDeckSimulator

#Preview("Stream Deck +") {
    StreamDeckSimulator.PreviewView(streamDeck: .plus) { device in
        let state = DeckState()
        state.setKeyMapping(
            DeckAction.defaultMapping(modelIdentifier: "plus", keyCount: 8),
            forModel: "plus")
        state.setRecording(true, elapsedText: "00:05:23")
        state.setTransport(playing: true, speedText: "1.0×", followLive: true)
        state.setPosition(timecodeText: "00:04:58:12", positionFraction: 0.85)
        device.render(ReplayDeckLayout().environmentObject(state))
    }
}

#Preview("Stream Deck MK.2") {
    StreamDeckSimulator.PreviewView(streamDeck: .regular) { device in
        let state = DeckState()
        state.setKeyMapping(
            DeckAction.defaultMapping(modelIdentifier: "regular", keyCount: 15),
            forModel: "regular")
        state.setTransport(playing: false, speedText: "Paused", followLive: false)
        device.render(ReplayDeckLayout().environmentObject(state))
    }
}
#endif

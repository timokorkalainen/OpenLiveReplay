import Combine
import StreamDeckKit
import SwiftUI

/// Adaptive layout rendered on every connected Stream Deck.
///
/// Observation granularity (matches the SDK's leaf-state pattern): the
/// container is static — it never re-evaluates after install, because a
/// container body re-evaluation marks the whole screen dirty and forces a
/// full USB re-transmit. Instead each leaf owns its rendered values as
/// @State, updated through an equality-guarded objectWillChange
/// subscription. A leaf re-evaluates only when its own content changed, so
/// on key-only devices a ticking timecode re-renders one key, not the deck.
/// (Window devices — +, + XL, Neo — always receive full frames by SDK
/// design; the leaf guards still bound how often that happens.)
struct ReplayDeckLayout: View {

    @Environment(\.streamDeckViewContext.device) var streamDeck
    /// Plain reference — deliberately NOT observed (see above).
    let state: DeckState

    var body: some View {
        let model = deckModelIdentifier(for: streamDeck)
        StreamDeckLayout {
            StreamDeckKeyAreaLayout { keyIndex in
                ReplayKeyView(keyIndex: keyIndex, model: model, state: state)
            }
            .background(.black)
        } windowArea: {
            switch streamDeck.info.product {
            case .plus, .plusXL:
                ReplayDialStrip(
                    state: state,
                    dialCount: max(streamDeck.capabilities.dialCount, 1),
                    model: model)
            case .neo:
                ReplayNeoPanel(state: state)
            default:
                Color.black
            }
        }
    }
}

/// Everything a key renders, as an equatable value: the leaf's @State guard
/// compares old and new content, so a key re-renders only on real change.
/// Every property a key draws MUST be represented here — a rendered value
/// missing from KeyContent would freeze on the deck.
struct KeyContent: Equatable {
    let action: DeckAction?
    let isActive: Bool
    let title: String
    let symbolName: String?

    static let empty = KeyContent(action: nil, isActive: false, title: "", symbolName: nil)
}

extension KeyContent {

    /// Derives one key's content from app state. Pure data transformation —
    /// covered by table tests in DeckStateTests.
    @MainActor
    init(state: DeckState, keyIndex: Int, model: String) {
        guard let action = state.action(forKey: keyIndex, model: model) else {
            self = .empty
            return
        }
        switch action {
        case .timecodeDisplay:
            self.init(action: action, isActive: false,
                      title: state.timecodeText, symbolName: nil)
        case .speedDisplay:
            self.init(action: action, isActive: false,
                      title: state.speedText, symbolName: nil)
        case .record:
            self.init(action: action, isActive: state.isRecording,
                      title: state.isRecording ? state.recElapsedText : action.label,
                      symbolName: action.symbolName)
        case .playPause:
            self.init(action: action, isActive: state.isPlaying,
                      title: action.label, symbolName: action.symbolName)
        case .goLive:
            self.init(action: action, isActive: state.followLive,
                      title: action.label, symbolName: action.symbolName)
        default:
            self.init(action: action, isActive: false,
                      title: action.label, symbolName: action.symbolName)
        }
    }
}

/// One key. Owns its content as @State so its body — and therefore the
/// SDK's dirty marking for this key — only runs on real content changes.
struct ReplayKeyView: View {

    let keyIndex: Int
    let model: String
    let state: DeckState

    @State private var content: KeyContent
    @State private var isPressed = false

    init(keyIndex: Int, model: String, state: DeckState) {
        self.keyIndex = keyIndex
        self.model = model
        self.state = state
        _content = State(initialValue: KeyContent(state: state, keyIndex: keyIndex, model: model))
    }

    var body: some View {
        // Single StreamDeckKeyView wrapper for ALL content: its body is the
        // SDK's only source of per-key dirty marking, so every content
        // change — including display keys and unmapped keys — must flow
        // through it to reach the USB bus.
        StreamDeckKeyView { pressed in
            isPressed = pressed
            OLRStreamDeckBridge.shared.emitKey(keyIndex, pressed: pressed)
        } content: {
            keyBody
        }
        .onReceive(state.objectWillChange.receive(on: RunLoop.main)) { _ in
            let new = KeyContent(state: state, keyIndex: keyIndex, model: model)
            if new != content { content = new }
        }
    }

    @ViewBuilder
    private var keyBody: some View {
        if let action = content.action {
            switch action {
            case .timecodeDisplay:
                DisplayKey(title: "TC", value: content.title)
            case .speedDisplay:
                DisplayKey(title: "SPEED", value: content.title)
            default:
                actionKeyContent(for: action)
            }
        } else {
            Color.black
        }
    }

    private var activeColor: Color {
        content.action == .record ? .red : .green
    }

    private func actionKeyContent(for action: DeckAction) -> some View {
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

/// Stream Deck + / + XL touch strip: each dial fires per-dial bound actions,
/// the whole strip is a continuous scrub bar; tapping it seeks. Owns its values
/// as @State (same granularity pattern as keys; these devices receive full
/// frames anyway, so the guards bound frequency, not size).
struct ReplayDialStrip: View {

    let state: DeckState
    let dialCount: Int
    let model: String

    @Environment(\.streamDeckViewContext.size) var stripSize
    @State private var fraction: Double
    @State private var timecodeText: String
    @State private var speedText: String
    @State private var pressLabels: [String]
    @State private var rotateLabels: [String]

    init(state: DeckState, dialCount: Int, model: String) {
        self.state = state
        self.dialCount = dialCount
        self.model = model
        _fraction = State(initialValue: state.positionFraction)
        _timecodeText = State(initialValue: state.timecodeText)
        _speedText = State(initialValue: state.speedText)
        _pressLabels = State(initialValue: Self.labels(state.dialPressMappings[model], count: dialCount))
        _rotateLabels = State(initialValue: Self.labels(state.dialRotateMappings[model], count: dialCount))
    }

    private func refreshLabels() {
        let p = Self.labels(state.dialPressMappings[model], count: dialCount)
        let r = Self.labels(state.dialRotateMappings[model], count: dialCount)
        if p != pressLabels { pressLabels = p }
        if r != rotateLabels { rotateLabels = r }
    }

    private static func labels(_ row: [Int]?, count: Int) -> [String] {
        (0..<count).map { i in
            guard let row, i < row.count, let a = DeckAction(rawValue: row[i]) else { return "" }
            return a.label
        }
    }

    var body: some View {
        StreamDeckDialAreaLayout(
            rotate: { dialIndex, rotation in
                OLRStreamDeckBridge.shared.emitDialRotate(dialIndex, delta: rotation)
            },
            press: { dialIndex, pressed in
                OLRStreamDeckBridge.shared.emitDialPress(dialIndex, pressed: pressed)
            },
            touch: { location in
                guard stripSize.width > 0 else { return }
                let f = min(max(location.x / stripSize.width, 0), 1)
                OLRStreamDeckBridge.shared.emitScrub(Double(f))
            }
        ) { dialIndex in
            DialSegment(
                segmentIndex: dialIndex,
                segmentCount: dialCount,
                fraction: fraction,
                timecodeText: dialIndex == 0 ? timecodeText : "",
                pressLabel: pressLabels.indices.contains(dialIndex) ? pressLabels[dialIndex] : "",
                rotateLabel: rotateLabels.indices.contains(dialIndex) ? rotateLabels[dialIndex] : "",
                speedText: speedText)
        }
        .onReceive(state.objectWillChange.receive(on: RunLoop.main)) { _ in
            if state.positionFraction != fraction { fraction = state.positionFraction }
            if state.timecodeText != timecodeText { timecodeText = state.timecodeText }
            if state.speedText != speedText { speedText = state.speedText }
            refreshLabels()
        }
    }
}

/// One dial section: scrub-bar background (shared across the strip) + this
/// dial's bound action labels. Segment 0 also shows the timecode. A dial whose
/// rotate label is "Shuttle" shows the live speed instead of the timecode slot.
struct DialSegment: View {

    let segmentIndex: Int
    let segmentCount: Int
    let fraction: Double
    let timecodeText: String
    let pressLabel: String
    let rotateLabel: String
    let speedText: String

    var body: some View {
        GeometryReader { geo in
            let globalProgress = fraction * Double(segmentCount)
            let localFill = min(max(globalProgress - Double(segmentIndex), 0), 1)
            ZStack(alignment: .topLeading) {
                Rectangle().fill(Color(white: 0.15))
                Rectangle().fill(Color.blue).frame(width: geo.size.width * localFill)
                VStack(alignment: .leading, spacing: 0) {
                    if !rotateLabel.isEmpty {
                        Text(rotateLabel == "Shuttle" ? "Shuttle \(speedText)" : rotateLabel)
                            .font(.system(size: 9, weight: .bold))
                            .foregroundColor(Color(white: 0.85))
                    }
                    if !pressLabel.isEmpty {
                        Text("• \(pressLabel)")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundColor(Color(white: 0.7))
                    }
                    if segmentIndex == 0 {
                        Text(timecodeText)
                            .font(.system(size: 13, weight: .bold).monospacedDigit())
                            .foregroundColor(.white)
                    }
                }
                .padding(.leading, 6)
                .padding(.top, 4)
            }
        }
    }
}

/// Stream Deck Neo: the two touch keys step a frame back/forward, the info
/// panel shows REC state and timecode. Owns its values as @State.
struct ReplayNeoPanel: View {

    let state: DeckState

    @State private var isRecording: Bool
    @State private var timecodeText: String

    init(state: DeckState) {
        self.state = state
        _isRecording = State(initialValue: state.isRecording)
        _timecodeText = State(initialValue: state.timecodeText)
    }

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
        .onReceive(state.objectWillChange.receive(on: RunLoop.main)) { _ in
            if state.isRecording != isRecording { isRecording = state.isRecording }
            if state.timecodeText != timecodeText { timecodeText = state.timecodeText }
        }
    }
}

#if DEBUG && canImport(StreamDeckSimulator)
import StreamDeckSimulator

#Preview("Stream Deck +") {
    StreamDeckSimulator.PreviewView(streamDeck: .plus) { device in
        MainActor.assumeIsolated {
            let state = DeckState()
            state.setKeyMapping(
                DeckAction.defaultMapping(modelIdentifier: "plus", keyCount: 8),
                forModel: "plus")
            state.setDialMapping(rotate: [8, 10, -1, -1], press: [0, 5, -1, -1], forModel: "plus")
            state.setRecording(true, elapsedText: "00:05:23")
            state.setTransport(playing: true, speedText: "1.0×", followLive: true)
            state.setPosition(timecodeText: "00:04:58:12", positionFraction: 0.85)
            device.render(ReplayDeckLayout(state: state))
        }
    }
}

#Preview("Stream Deck MK.2") {
    StreamDeckSimulator.PreviewView(streamDeck: .regular) { device in
        MainActor.assumeIsolated {
            let state = DeckState()
            state.setKeyMapping(
                DeckAction.defaultMapping(modelIdentifier: "regular", keyCount: 15),
                forModel: "regular")
            state.setTransport(playing: false, speedText: "Paused", followLive: false)
            device.render(ReplayDeckLayout(state: state))
        }
    }
}
#endif

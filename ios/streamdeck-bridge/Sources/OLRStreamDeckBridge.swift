import Combine
import Foundation
import StreamDeckKit
import StreamDeckSimulator
import UIKit

/// Objective-C facade consumed by the Qt app (streamdeck/streamdeckmanager.mm).
/// No Swift types cross this boundary.
@MainActor
@objc(OLRStreamDeckBridge)
public final class OLRStreamDeckBridge: NSObject {

    @objc public static let shared = OLRStreamDeckBridge()

    // MARK: Event callbacks — set by the Qt side before start().
    // All fire on the main thread (the SDK delivers on RunLoop.main).

    /// (actionId, pressed). See DeckAction for the id vocabulary.
    @objc public var onAction: ((Int, Bool) -> Void)?
    /// Signed dial-rotation delta (positive = clockwise).
    @objc public var onJog: ((Int) -> Void)?
    /// Touch-strip scrub position, 0.0 ... 1.0.
    @objc public var onScrub: ((Double) -> Void)?
    /// (productName, modelIdentifier, keyCount, dialCount).
    @objc public var onDeviceConnected: ((String, String, Int, Int) -> Void)?
    /// Fires when the last device disconnects.
    @objc public var onDeviceDisconnected: (() -> Void)?

    let state = DeckState()

    private var started = false
    private var cancellables = Set<AnyCancellable>()

    private override init() {
        super.init()
    }

    // MARK: Lifecycle.

    /// Call once at app startup. Devices plugged in at any time afterwards
    /// (including after backgrounding — the SDK re-fires the handler on
    /// foreground) get the layout attached automatically.
    @objc public func start() {
        guard !started else { return }
        started = true

        StreamDeckSession.setUp(newDeviceHandler: { [weak self] device in
            self?.attach(device)
        })

        StreamDeckSession.instance.$devices
            .receive(on: RunLoop.main)
            .sink { [weak self] devices in
                if devices.isEmpty {
                    self?.onDeviceDisconnected?()
                }
            }
            .store(in: &cancellables)
    }

    private func attach(_ device: StreamDeck) {
        let model = deckModelIdentifier(for: device)
        if state.keyMappings[model] == nil {
            state.setKeyMapping(
                DeckAction.defaultMapping(
                    modelIdentifier: model,
                    keyCount: device.capabilities.keyCount),
                forModel: model)
        }
        device.render(ReplayDeckLayout(state: state))
        onDeviceConnected?(
            device.info.productName,
            model,
            device.capabilities.keyCount,
            device.capabilities.dialCount)
    }

    // MARK: State pushed from the Qt side (forwards into DeckState, which
    // no-ops unchanged values so only changed keys re-render).

    /// Override a model's key mapping (future user remapping; unused by the
    /// default flow, which computes mappings in attach()).
    @objc public func setKeyMapping(_ mapping: [Int], forModel model: String) {
        state.setKeyMapping(mapping, forModel: model)
    }

    @objc public func setRecording(_ recording: Bool, elapsedText: String) {
        state.setRecording(recording, elapsedText: elapsedText)
    }

    @objc public func setTransport(playing: Bool, speedText: String, followLive: Bool) {
        state.setTransport(playing: playing, speedText: speedText, followLive: followLive)
    }

    @objc public func setPosition(timecodeText: String, positionFraction: Double) {
        state.setPosition(timecodeText: timecodeText, positionFraction: positionFraction)
    }

    // MARK: Development tooling. The simulator is a dev tool per Elgato's
    // docs; the Qt side only exposes its UI entry point in debug builds.

    @objc public func showSimulator() {
        StreamDeckSimulator.show(defaultStreamDeck: .plus)
    }

    @objc public func closeSimulator() {
        StreamDeckSimulator.close()
    }

    /// True when the Elgato Stream Deck Connect app (which hosts the device
    /// driver) is installed. Requires `elgato-device-driver` in the host
    /// app's LSApplicationQueriesSchemes.
    @objc public var driverAppInstalled: Bool {
        guard let url = URL(string: "elgato-device-driver://") else { return false }
        return UIApplication.shared.canOpenURL(url)
    }

    // MARK: Internal event funnel (called from layout views).

    func emitAction(_ actionId: Int, pressed: Bool) { onAction?(actionId, pressed) }
    func emitJog(_ delta: Int) { onJog?(delta) }
    func emitScrub(_ fraction: Double) { onScrub?(fraction) }
}

/// Stable model identifier shared with the Qt side and used as the
/// key-mapping dictionary key.
func deckModelIdentifier(for device: StreamDeck) -> String {
    switch device.info.product {
    case .mini: return "mini"
    case .regular: return "regular"
    case .plus: return "plus"
    case .xl: return "xl"
    case .pedal: return DeckAction.pedalModelIdentifier
    case .neo: return "neo"
    case .plusXL: return "plusXL"
    case .none: return "unknown"
    @unknown default: return "unknown"
    }
}

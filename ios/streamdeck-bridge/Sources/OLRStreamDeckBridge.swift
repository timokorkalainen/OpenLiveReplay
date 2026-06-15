import Combine
import Foundation
import StreamDeckKit
#if canImport(StreamDeckSimulator)
import StreamDeckSimulator
#endif
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
    /// (rotateActionId, signed delta). Fired only outside learn mode.
    @objc public var onRotate: ((Int, Int) -> Void)?
    /// (elementType, index) during learn mode: 0 = key, 1 = dial-press, 2 = dial-turn.
    @objc public var onLearnInput: ((Int, Int) -> Void)?
    /// Touch-strip scrub position, 0.0 ... 1.0.
    @objc public var onScrub: ((Double) -> Void)?
    /// (productName, modelIdentifier, keyCount, dialCount).
    @objc public var onDeviceConnected: ((String, String, Int, Int) -> Void)?
    /// Fires when the last device disconnects.
    @objc public var onDeviceDisconnected: (() -> Void)?

    let state = DeckState()

    private var started = false
    private var cancellables = Set<AnyCancellable>()
    private var learning = false
    private var currentModel = "unknown"

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
        currentModel = model
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

    @objc public func setLearnMode(_ active: Bool) { learning = active }

    @objc public func setDialMapping(rotate: [Int], press: [Int], forModel model: String) {
        state.setDialMapping(rotate: rotate, press: press, forModel: model)
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
        #if canImport(StreamDeckSimulator)
        StreamDeckSimulator.show(defaultStreamDeck: .plus)
        #endif
    }

    @objc public func closeSimulator() {
        #if canImport(StreamDeckSimulator)
        StreamDeckSimulator.close()
        #endif
    }

    /// Whether the in-app Stream Deck simulator is compiled in. False while the
    /// StreamDeckSimulator SPM product is dropped (Xcode 26.5 asset-catalog
    /// incompatibility) — lets the Qt side hide its now-inert button.
    @objc public var simulatorSupported: Bool {
        #if canImport(StreamDeckSimulator)
        return true
        #else
        return false
        #endif
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

    func emitKey(_ keyIndex: Int, pressed: Bool) {
        if learning {
            if pressed { onLearnInput?(0, keyIndex) }
            return
        }
        guard let action = state.action(forKey: keyIndex, model: currentModel),
              action != .timecodeDisplay, action != .speedDisplay else { return }
        onAction?(action.rawValue, pressed)
    }

    func emitDialPress(_ dial: Int, pressed: Bool) {
        if learning {
            if pressed { onLearnInput?(1, dial) }
            return
        }
        guard let action = state.pressAction(forDial: dial, model: currentModel) else { return }
        onAction?(action.rawValue, pressed)
    }

    func emitDialRotate(_ dial: Int, delta: Int) {
        if learning {
            onLearnInput?(2, dial)
            return
        }
        guard let action = state.rotateAction(forDial: dial, model: currentModel) else { return }
        onRotate?(action.rawValue, delta)
    }

    func emitScrub(_ fraction: Double) { onScrub?(fraction) }
}

#if DEBUG
extension OLRStreamDeckBridge {
    @objc func _setCurrentModelForTesting(_ model: String) { currentModel = model }
}
#endif

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

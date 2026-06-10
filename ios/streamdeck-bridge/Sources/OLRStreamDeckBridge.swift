import Foundation
import StreamDeckKit
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

    private override init() {
        super.init()
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

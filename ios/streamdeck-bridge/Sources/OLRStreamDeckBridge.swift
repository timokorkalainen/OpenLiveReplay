import Foundation
import UIKit

/// Objective-C facade consumed by the Qt app (streamdeck/streamdeckmanager.mm).
/// No Swift types cross this boundary.
@objc(OLRStreamDeckBridge)
public final class OLRStreamDeckBridge: NSObject {

    @objc public static let shared = OLRStreamDeckBridge()

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
}

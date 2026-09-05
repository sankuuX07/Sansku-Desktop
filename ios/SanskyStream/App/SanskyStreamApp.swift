import SwiftUI

/// Application entry point.
/// Sets ConnectView as the root of the window hierarchy.
/// M16: Holds ServiceAdvertiser for the app lifetime.
@main
struct SanskyStreamApp: App {

    /// M16: Advertises this iPhone on the local network as a SanskyStream
    /// sender device.  Lifetime-bound to the app process.
    private let serviceAdvertiser = ServiceAdvertiser()

    init() {
        // M16: Start advertising immediately on launch.
        // Non-fatal: if local-network permission is denied or mDNS is
        // unavailable, the app continues to work normally with manual IP entry.
        serviceAdvertiser.start()
    }

    var body: some Scene {
        WindowGroup {
            ConnectView()
        }
    }
}

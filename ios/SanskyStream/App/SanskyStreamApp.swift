import SwiftUI

/// Application entry point.
/// Sets ConnectView as the root of the window hierarchy.
@main
struct SanskyStreamApp: App {
    var body: some Scene {
        WindowGroup {
            ConnectView()
        }
    }
}

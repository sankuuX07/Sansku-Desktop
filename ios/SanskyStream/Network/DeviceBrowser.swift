// ---------------------------------------------------------------------------
// DeviceBrowser.swift — M16: iPhone Browser for Windows SanskyStream Receivers
//
// Browses for "_sanskystream._tcp" services on the local network.
// Filters for role=receiver (Windows PCs) — ignores other senders.
//
// Each DiscoveredReceiver exposes an NWEndpoint that can be passed
// directly to ConnectionManager.connect(to:) without manual IP entry.
//
// Usage:
//   @StateObject private var browser = DeviceBrowser()
//   browser.start()
//   browser.stop()
//   browser.receivers  // [DiscoveredReceiver]
//
// Required Info.plist keys (same as ServiceAdvertiser):
//   NSLocalNetworkUsageDescription
//   NSBonjourServices: _sanskystream._tcp
//
// Threading: All @Published mutations happen on the main thread.
// NWBrowser callbacks are dispatched to the main queue here.
// ---------------------------------------------------------------------------

import Foundation
import Network

// ---------------------------------------------------------------------------
// DiscoveredReceiver — a Windows SanskyStream PC found on the LAN.
// ---------------------------------------------------------------------------
struct DiscoveredReceiver: Identifiable, Equatable {
    /// The Bonjour service instance name (e.g. "SanskyStream DESKTOP-ABC").
    let id:              String
    /// Human-readable display name stripped of the service-type suffix.
    let displayName:     String
    /// The fully-resolved NWEndpoint — pass directly to NWConnection.
    let endpoint:        NWEndpoint
    /// Protocol version from the "ver" TXT key.  0 = unknown.
    let protocolVersion: UInt32

    static func == (lhs: DiscoveredReceiver, rhs: DiscoveredReceiver) -> Bool {
        lhs.id == rhs.id
    }
}

// ---------------------------------------------------------------------------
// DeviceBrowser
// ---------------------------------------------------------------------------
final class DeviceBrowser: ObservableObject {

    // MARK: - Published

    /// List of currently visible Windows receiver PCs.
    @Published private(set) var receivers: [DiscoveredReceiver] = []

    // MARK: - Private

    private var browser: NWBrowser?
    private let queue = DispatchQueue(label: "com.sanskystream.DeviceBrowser",
                                      qos: .utility)

    // MARK: - Public API

    func start() {
        guard browser == nil else { return }

        let params = NWParameters()
        params.includePeerToPeer = true

        let newBrowser = NWBrowser(
            for: .bonjourWithTXTRecord(type: ServiceAdvertiser.serviceType,
                                       domain: "local."),
            using: params
        )

        newBrowser.stateUpdateHandler = { state in
            DispatchQueue.main.async {
                switch state {
                case .ready:
                    print("[SanskyStream] DeviceBrowser: browsing for Windows receivers.")
                case .failed(let error):
                    print("[SanskyStream] DeviceBrowser: failed — \(error.localizedDescription)")
                default:
                    break
                }
            }
        }

        newBrowser.browseResultsChangedHandler = { [weak self] _, changes in
            self?.applyChanges(changes)
        }

        newBrowser.start(queue: queue)
        browser = newBrowser
    }

    func stop() {
        browser?.cancel()
        browser = nil
        DispatchQueue.main.async { self.receivers = [] }
        print("[SanskyStream] DeviceBrowser: stopped.")
    }

    deinit { stop() }

    // MARK: - Private

    private func applyChanges(_ changes: Set<NWBrowser.Result.Change>) {
        var added:   [DiscoveredReceiver] = []
        var removed: [String] = []

        for change in changes {
            switch change {
            case .added(let result):
                if let receiver = parseResult(result) {
                    added.append(receiver)
                }
            case .removed(let result):
                if case .service(let name, _, _, _) = result.endpoint {
                    removed.append(name)
                }
            case .changed(old: _, new: let newResult, flags: _):
                if let receiver = parseResult(newResult) {
                    added.append(receiver) // Upsert
                }
            @unknown default:
                break
            }
        }

        DispatchQueue.main.async {
            // Remove departed devices.
            self.receivers.removeAll { removed.contains($0.id) }
            // Upsert new/updated devices.
            for r in added {
                if let idx = self.receivers.firstIndex(where: { $0.id == r.id }) {
                    self.receivers[idx] = r
                } else {
                    self.receivers.append(r)
                }
            }
        }
    }

    private func parseResult(_ result: NWBrowser.Result) -> DiscoveredReceiver? {
        // Only .service endpoints carry Bonjour names.
        guard case .service(let name, _, _, _) = result.endpoint else { return nil }

        // Read TXT record — must be role=receiver to be a Windows PC.
        var role = ""
        var ver: UInt32 = 0

        if case .bonjour(let txt) = result.metadata {
            role = txt["role"] ?? ""
            ver  = UInt32(txt["ver"] ?? "0") ?? 0
        }

        guard role == "receiver" else { return nil }

        // Strip the service-type suffix from the display name.
        let suffix = "._sanskystream._tcp.local"
        var displayName = name
        if displayName.hasSuffix(suffix) {
            displayName = String(displayName.dropLast(suffix.count))
        }
        // Also strip a leading "SanskyStream " prefix for a cleaner label.
        if displayName.hasPrefix("SanskyStream ") {
            displayName = String(displayName.dropFirst("SanskyStream ".count))
        }

        return DiscoveredReceiver(
            id:              name,
            displayName:     displayName,
            endpoint:        result.endpoint,
            protocolVersion: ver
        )
    }
}

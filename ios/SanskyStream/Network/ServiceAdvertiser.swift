// ---------------------------------------------------------------------------
// ServiceAdvertiser.swift — M16: iPhone mDNS/DNS-SD Service Advertisement
//
// Advertises "SanskyStream <device-name>._sanskystream._tcp" on the local
// network using Network.framework NWListener (iOS 13+).
//
// The advertisement is purely informational — it tells Windows (and other
// peers) that this iPhone is running SanskyStream and is willing to connect
// as a sender.  No media data is sent through this listener.
//
// TXT record keys:
//   ver=1      Protocol version (maps to Protocol.PROTOCOL_VERSION on Windows)
//   role=sender  Identifies this as a source device (not a receiver)
//
// Required Info.plist keys (see ios/SanskyStream/Info.plist):
//   NSLocalNetworkUsageDescription — triggers the local network permission
//   NSBonjourServices — declares _sanskystream._tcp so the system allows it
//
// Thread safety: start() / stop() must be called on the main thread.
// NWListener callbacks are dispatched back to the main queue internally.
// ---------------------------------------------------------------------------

import Foundation
import Network
import UIKit

final class ServiceAdvertiser {

    // MARK: - Constants

    static let serviceType      = "_sanskystream._tcp"
    static let protocolVersion  = "1"

    // MARK: - Private

    private var listener: NWListener?
    private(set) var isAdvertising = false

    // MARK: - Public API

    /// Start advertising on the local network.  Non-fatal if it fails.
    func start() {
        guard !isAdvertising else { return }

        let params = NWParameters.tcp
        params.includePeerToPeer = true

        guard let listener = try? NWListener(using: params) else {
            print("[SanskyStream] ServiceAdvertiser: Failed to create NWListener.")
            return
        }

        // Build the TXT record: ver=1, role=sender
        var txtRecord = NWTXTRecord()
        txtRecord[ServiceAdvertiser.serviceType + "_ver"]  = nil // clear any leftover
        _ = txtRecord   // NWTXTRecord uses subscript
        var txt = NWTXTRecord()
        txt["ver"]  = ServiceAdvertiser.protocolVersion
        txt["role"] = "sender"

        // Use the user-assigned device name (e.g. "John's iPhone").
        let deviceName = UIDevice.current.name

        listener.service = NWListener.Service(
            name:      deviceName,
            type:      ServiceAdvertiser.serviceType,
            domain:    "local.",
            txtRecord: txt
        )

        listener.serviceRegistrationUpdateHandler = { change in
            switch change {
            case .add(let endpoint):
                print("[SanskyStream] ServiceAdvertiser: registered — \(endpoint)")
            case .remove(let endpoint):
                print("[SanskyStream] ServiceAdvertiser: removed — \(endpoint)")
            @unknown default:
                break
            }
        }

        listener.stateUpdateHandler = { [weak self] state in
            DispatchQueue.main.async {
                switch state {
                case .ready:
                    print("[SanskyStream] ServiceAdvertiser: advertising '\(deviceName)' on local network.")
                case .failed(let error):
                    print("[SanskyStream] ServiceAdvertiser: failed — \(error.localizedDescription)")
                    self?.isAdvertising = false
                case .cancelled:
                    self?.isAdvertising = false
                default:
                    break
                }
            }
        }

        // We advertise only; reject all incoming connection attempts.
        listener.newConnectionHandler = { connection in
            connection.cancel()
        }

        listener.start(queue: .main)
        self.listener     = listener
        self.isAdvertising = true
    }

    /// Stop advertising.
    func stop() {
        listener?.cancel()
        listener      = nil
        isAdvertising = false
        print("[SanskyStream] ServiceAdvertiser: stopped.")
    }

    deinit {
        stop()
    }
}

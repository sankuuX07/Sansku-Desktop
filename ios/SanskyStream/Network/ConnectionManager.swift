import Foundation
import Network

/// Manages a single TCP connection from the iPhone to the Windows SanskyStream receiver.
///
/// - All public methods must be called from the main thread.
/// - All @Published mutations happen on the main thread via DispatchQueue.main.async.
/// - NWConnection callbacks run on the background queue supplied to start().
final class ConnectionManager: ObservableObject {

    // MARK: - Connection status

    enum Status: Equatable {
        case disconnected
        case connecting
        case connected
        case failed(String)

        /// Human-readable description shown in the UI.
        var displayText: String {
            switch self {
            case .disconnected:        return "Disconnected"
            case .connecting:          return "Connecting..."
            case .connected:           return "Connected"
            case .failed(let message): return "Error: \(message)"
            }
        }
    }

    // MARK: - Published state

    @Published private(set) var status: Status = .disconnected

    // MARK: - Private

    private var connection: NWConnection?

    // MARK: - Public API

    /// Open a TCP connection to the Windows receiver.
    ///
    /// - Parameters:
    ///   - host:     Windows PC local IPv4 address (e.g. "192.168.1.100")
    ///   - portText: Port number as a string (default "5000")
    func connect(host: String, portText: String) {
        let trimmedHost = host.trimmingCharacters(in: .whitespaces)
        guard !trimmedHost.isEmpty else {
            setStatus(.failed("Enter the Windows PC IP address"))
            return
        }

        let trimmedPort = portText.trimmingCharacters(in: .whitespaces)
        guard let portNumber = UInt16(trimmedPort), portNumber > 0 else {
            setStatus(.failed("Enter a valid port number"))
            return
        }

        guard let nwPort = NWEndpoint.Port(rawValue: portNumber) else {
            setStatus(.failed("Invalid port"))
            return
        }

        // Cancel any existing connection before opening a new one
        connection?.cancel()
        connection = nil

        setStatus(.connecting)

        let nwConnection = NWConnection(
            host: NWEndpoint.Host(trimmedHost),
            port: nwPort,
            using: .tcp
        )

        // stateUpdateHandler fires on .global(qos: .userInitiated) — dispatch back to main
        nwConnection.stateUpdateHandler = { [weak self] state in
            DispatchQueue.main.async {
                self?.handleConnectionState(state)
            }
        }

        connection = nwConnection
        nwConnection.start(queue: .global(qos: .userInitiated))
    }

    /// Cancel the active connection and return to the disconnected state.
    func disconnect() {
        connection?.cancel()
        connection = nil
        setStatus(.disconnected)
    }

    // MARK: - Private — state machine

    /// Process an NWConnection state transition.
    /// Always called on the main thread.
    private func handleConnectionState(_ state: NWConnection.State) {
        switch state {

        case .setup:
            // Initial state before start() — nothing to do
            break

        case .preparing:
            setStatus(.connecting)

        case .ready:
            // TCP handshake complete — update UI then send the test message
            setStatus(.connected)
            sendTestMessage()

        case .waiting(let error):
            // Temporary condition (e.g. Wi-Fi not yet available); update UI
            setStatus(.failed(error.localizedDescription))

        case .failed(let error):
            setStatus(.failed(error.localizedDescription))
            connection = nil

        case .cancelled:
            // Reach here via disconnect() or system cancellation.
            // Preserve a failure message if one is already displayed.
            if case .failed = status { break }
            setStatus(.disconnected)

        @unknown default:
            break
        }
    }

    // MARK: - Private — test message

    /// Send the UTF-8 string "HELLO" to the Windows receiver.
    /// The receiver logs this and the connection is then left open until the user disconnects.
    private func sendTestMessage() {
        guard let nwConnection = connection,
              let data = "HELLO".data(using: .utf8) else { return }

        nwConnection.send(content: data, completion: .contentProcessed { error in
            if let error = error {
                print("[SanskyStream] Send error: \(error.localizedDescription)")
            } else {
                print("[SanskyStream] Sent: HELLO")
            }
        })
    }

    // MARK: - Private — helpers

    /// Update the published status. Must be called on the main thread.
    private func setStatus(_ newStatus: Status) {
        status = newStatus
    }
}

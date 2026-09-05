import SwiftUI

/// Main screen of the SanskyStream iPhone app.
///
/// Presents:
///   - M16 Discovered Devices section (auto-filled from DeviceBrowser)
///   - Manual IP/port inputs (unchanged from pre-M16)
///   - Connect/Disconnect button
///   - Live status indicator
///   - Screen Capture picker
///
/// Requires iOS 16 (NavigationStack, LabeledContent).
struct ConnectView: View {

    @StateObject private var connectionManager = ConnectionManager()
    @StateObject private var deviceBrowser     = DeviceBrowser()   // M16

    @State private var ipAddress: String = ""
    @State private var portText:  String = "5000"

    var body: some View {
        NavigationStack {
            Form {

                // ── M16: Discovered Devices ───────────────────────────────
                // Shows Windows PCs found automatically on the local network.
                // Tapping a device connects immediately using the resolved
                // NWEndpoint — no manual IP entry required.
                if !deviceBrowser.receivers.isEmpty {
                    Section {
                        ForEach(deviceBrowser.receivers) { receiver in
                            Button {
                                connectionManager.connect(to: receiver.endpoint)
                            } label: {
                                HStack(spacing: 12) {
                                    Image(systemName: "display")
                                        .foregroundStyle(.blue)
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(receiver.displayName)
                                            .font(.headline)
                                        Text("SanskyStream  ·  ver \(receiver.protocolVersion)")
                                            .font(.caption)
                                            .foregroundStyle(.secondary)
                                    }
                                    Spacer()
                                    Image(systemName: "chevron.right")
                                        .foregroundStyle(.secondary)
                                }
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                        }
                    } header: {
                        Label("Discovered on Wi-Fi", systemImage: "network")
                    } footer: {
                        Text("Tap a device to connect automatically.")
                            .font(.caption)
                    }
                }

                // ── Windows PC configuration ──────────────────────────────
                Section {
                    LabeledContent("IP Address") {
                        TextField("192.168.x.x", text: $ipAddress)
                            .keyboardType(.numbersAndPunctuation)
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                            .multilineTextAlignment(.trailing)
                    }

                    LabeledContent("Port") {
                        TextField("5000", text: $portText)
                            .keyboardType(.numberPad)
                            .multilineTextAlignment(.trailing)
                    }
                } header: {
                    Text("Windows PC (Manual)")
                } footer: {
                    Text("Enter the local IPv4 address of your Windows PC, or tap a discovered device above.")
                        .font(.caption)
                }

                // ── Action button ─────────────────────────────────────────
                Section {
                    switch connectionManager.status {
                    case .connected:
                        Button(role: .destructive) {
                            connectionManager.disconnect()
                        } label: {
                            centeredLabel("Disconnect")
                        }

                    case .connecting:
                        HStack {
                            Spacer()
                            ProgressView()
                                .padding(.trailing, 8)
                            Text("Connecting...")
                                .foregroundStyle(.secondary)
                            Spacer()
                        }

                    default:
                        Button {
                            connectionManager.connect(host: ipAddress, portText: portText)
                        } label: {
                            centeredLabel("Connect")
                        }
                    }
                }

                // ── Status ────────────────────────────────────────────────
                Section {
                    HStack(spacing: 10) {
                        Circle()
                            .fill(statusColor)
                            .frame(width: 10, height: 10)
                        Text(connectionManager.status.displayText)
                            .foregroundStyle(statusColor)
                    }
                } header: {
                    Text("Connection")
                }

                // ── Screen Capture ────────────────────────────────────────
                Section {
                    HStack {
                        Spacer()
                        BroadcastPickerView(preferredExtension: "com.sanskystream.ios.BroadcastExtension")
                            .frame(width: 44, height: 44)
                        Spacer()
                    }
                } header: {
                    Text("Screen Capture")
                } footer: {
                    Text("Tap to start capturing the iPhone system screen.")
                        .font(.caption)
                }
            }
            .navigationTitle("SanskyStream")
            .onAppear  { deviceBrowser.start() }   // M16: start browsing
            .onDisappear { /* keep browsing while app is in foreground */ }
        }
    }

    // MARK: - Helpers

    private var statusColor: Color {
        switch connectionManager.status {
        case .connected:    return .green
        case .connecting:   return .orange
        case .failed:       return .red
        case .disconnected: return .secondary
        }
    }

    /// Centred label used in the action button rows.
    private func centeredLabel(_ title: String) -> some View {
        HStack {
            Spacer()
            Text(title)
            Spacer()
        }
    }
}

// MARK: - Preview

#Preview {
    ConnectView()
}

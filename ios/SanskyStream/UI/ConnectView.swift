import SwiftUI

/// Main screen of the SanskyStream iPhone app.
///
/// Presents IP/port inputs, a Connect/Disconnect button, and a live status indicator.
/// Requires iOS 16 (NavigationStack, LabeledContent).
struct ConnectView: View {

    @StateObject private var connectionManager = ConnectionManager()

    @State private var ipAddress: String = ""
    @State private var portText:  String = "5000"

    var body: some View {
        NavigationStack {
            Form {

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
                    Text("Windows PC")
                } footer: {
                    Text("Enter the local IPv4 address of your Windows PC.")
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

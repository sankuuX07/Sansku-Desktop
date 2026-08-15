import SwiftUI
import ReplayKit

/// A SwiftUI wrapper for RPSystemBroadcastPickerView.
/// This provides the standard iOS control to start a system-wide screen broadcast.
struct BroadcastPickerView: UIViewRepresentable {
    
    /// The bundle identifier of the Broadcast Upload Extension.
    let preferredExtension: String?

    func makeUIView(context: Context) -> RPSystemBroadcastPickerView {
        let picker = RPSystemBroadcastPickerView(frame: CGRect(x: 0, y: 0, width: 44, height: 44))
        picker.preferredExtension = preferredExtension
        
        // Optional: show microphone button in the picker
        // Since audio is out of scope for Milestone 4, we keep this false.
        picker.showsMicrophoneButton = false
        
        return picker
    }

    func updateUIView(_ uiView: RPSystemBroadcastPickerView, context: Context) {
        // No dynamic updates needed
    }
}

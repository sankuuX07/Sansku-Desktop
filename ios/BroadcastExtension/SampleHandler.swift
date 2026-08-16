import ReplayKit
import CoreMedia

/// The Broadcast Upload Extension sample handler.
/// This class receives sample buffers from the system screen capture (ReplayKit).
///
/// M6 additions:
///   - VideoTransport instance sends encoded frames over UDP to Windows.
///   - VideoEncoder.onEncodedFrame is wired to VideoTransport.sendFrame().
///
/// Windows IP configuration:
///   WINDOWS_HOST below must be set to the LAN IP of the Windows PC
///   running SanskyStream (e.g. "192.168.1.100").
///   This will be replaced by a dynamic discovery mechanism in a future milestone.
class SampleHandler: RPBroadcastSampleHandler {

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    /// IP address of the Windows SanskyStream receiver on the local network.
    /// Update this to match your Windows PC's LAN IP before compiling.
    private static let WINDOWS_HOST = "192.168.1.100"

    // -----------------------------------------------------------------------
    // Components
    // -----------------------------------------------------------------------

    private let videoEncoder                  = VideoEncoder()
    private var videoTransport: VideoTransport?

    // -----------------------------------------------------------------------
    // Broadcast lifecycle
    // -----------------------------------------------------------------------

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        print("SanskyStream Broadcast Started")

        // Create the UDP transport to the Windows receiver.
        videoTransport = VideoTransport(windowsHost: SampleHandler.WINDOWS_HOST)
        if videoTransport == nil {
            print("SanskyStream SampleHandler: VideoTransport failed to initialize. Video will not be sent.")
        }

        // Wire the encoder output to the transport input.
        videoEncoder.onEncodedFrame = { [weak self] (naluData, presentationUs, isKeyframe) in
            self?.videoTransport?.sendFrame(naluData: naluData,
                                            presentationUs: presentationUs,
                                            isKeyframe: isKeyframe)
        }
    }

    override func broadcastPaused() {
        // User has requested to pause the broadcast.
        print("SanskyStream Broadcast Paused")
    }

    override func broadcastResumed() {
        // User has requested to resume the broadcast.
        print("SanskyStream Broadcast Resumed")
    }

    override func broadcastFinished() {
        // User has requested to finish the broadcast.
        print("SanskyStream Broadcast Finished")
        videoEncoder.invalidate()
        // VideoTransport socket is closed by its deinit.
        videoTransport = nil
    }

    // -----------------------------------------------------------------------
    // Sample buffer processing
    // -----------------------------------------------------------------------

    override func processSampleBuffer(_ sampleBuffer: CMSampleBuffer,
                                      with sampleBufferType: RPSampleBufferType) {
        switch sampleBufferType {
        case .video:
            handleVideoSampleBuffer(sampleBuffer)

        case .audioApp:
            // Audio is out of scope for Milestone 6.
            break

        case .audioMic:
            // Audio is out of scope for Milestone 6.
            break

        @unknown default:
            break
        }
    }

    // -----------------------------------------------------------------------
    // Video handling
    // -----------------------------------------------------------------------

    private func handleVideoSampleBuffer(_ sampleBuffer: CMSampleBuffer) {
        // Encode the video sample buffer using our H.264 VideoEncoder.
        // On completion, VideoEncoder calls onEncodedFrame, which calls
        // VideoTransport.sendFrame() to fragment and transmit over UDP.
        videoEncoder.encode(sampleBuffer)
    }
}

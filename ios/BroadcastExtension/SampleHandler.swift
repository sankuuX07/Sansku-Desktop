import ReplayKit
import CoreMedia

/// The Broadcast Upload Extension sample handler.
/// This class receives sample buffers from the system screen capture (ReplayKit).
///
/// M6 additions:
///   - VideoTransport instance sends encoded frames over UDP to Windows.
///   - VideoEncoder.onEncodedFrame is wired to VideoTransport.sendFrame().
///
/// M9 additions:
///   - AudioCapture instance processes .audioApp (game/system audio) buffers.
///   - .audioMic buffers remain intentionally ignored (microphone not required).
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

    // M9: Audio capture — app/system audio only (game audio, UI sounds).
    // Microphone capture is NOT included per M9 scope.
    private let audioCapture                   = AudioCapture()

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

        // M9: Wire AudioCapture callback.
        // onAudioFrame is a no-op placeholder until M10 (audio encoding).
        // The closure is intentionally empty: AudioCapture validates, buffers,
        // and logs internally. M10 will replace this with encoder invocation.
        audioCapture.onAudioFrame = { frame in
            // M10 will encode frame.pcmData here.
            // M9: audio data lives in memory only — no encoding, no transport.
            _ = frame
        }

        print("SanskyStream SampleHandler: AudioCapture ready (app audio only, M9).")
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
        // M9: log final audio statistics.
        audioCapture.logDiagnostics()
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
            // M9: Route application/system (game) audio to AudioCapture.
            handleAppAudioSampleBuffer(sampleBuffer)

        case .audioMic:
            // Microphone audio is not required for SanskyStream (game/app audio focus).
            // Intentionally ignored. Do not request microphone permission.
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

    // -----------------------------------------------------------------------
    // M9: App audio handling
    // -----------------------------------------------------------------------

    /// Route a ReplayKit .audioApp sample buffer to AudioCapture.
    ///
    /// AudioCapture validates, extracts format metadata, preserves the PTS,
    /// copies PCM bytes into an AudioFrame, and fires onAudioFrame.
    /// No encoding or network transmission occurs in M9.
    private func handleAppAudioSampleBuffer(_ sampleBuffer: CMSampleBuffer) {
        audioCapture.processAppAudioBuffer(sampleBuffer)
    }
}

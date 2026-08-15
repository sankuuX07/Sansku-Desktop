import ReplayKit
import CoreMedia

/// The Broadcast Upload Extension sample handler.
/// This class receives sample buffers from the system screen capture (ReplayKit).
class SampleHandler: RPBroadcastSampleHandler {
    
    private let videoEncoder = VideoEncoder()

    override func broadcastStarted(withSetupInfo setupInfo: [String : NSObject]?) {
        // User has requested to start the broadcast. Setup resources here.
        print("SanskyStream Broadcast Started")
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
    }
    
    override func processSampleBuffer(_ sampleBuffer: CMSampleBuffer, with sampleBufferType: RPSampleBufferType) {
        switch sampleBufferType {
        case .video:
            // Handle video sample buffer
            handleVideoSampleBuffer(sampleBuffer)
            
        case .audioApp:
            // Handle audio sample buffer for app audio
            // Audio is out of scope for Milestone 4.
            break
            
        case .audioMic:
            // Handle audio sample buffer for mic audio
            // Audio is out of scope for Milestone 4.
            break
            
        @unknown default:
            // Handle other sample buffer types
            break
        }
    }
    
    // MARK: - Video Handling
    
    private func handleVideoSampleBuffer(_ sampleBuffer: CMSampleBuffer) {
        // Encode the video sample buffer using our H.264 VideoEncoder
        videoEncoder.encode(sampleBuffer)
    }
}

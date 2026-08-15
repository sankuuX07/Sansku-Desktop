import ReplayKit
import CoreMedia

/// The Broadcast Upload Extension sample handler.
/// This class receives sample buffers from the system screen capture (ReplayKit).
class SampleHandler: RPBroadcastSampleHandler {

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
        // We only extract and log basic frame information as required by Milestone 4.
        // We DO NOT encode or send data over the network here.
        
        guard let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            print("SanskyStream: Video sample buffer does not contain an image buffer.")
            return
        }
        
        let width = CVPixelBufferGetWidth(imageBuffer)
        let height = CVPixelBufferGetHeight(imageBuffer)
        let timestamp = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let pixelFormat = CVPixelBufferGetPixelFormatType(imageBuffer)
        
        // Convert fourCC pixel format to a string if possible, or print the OSType integer
        let pixelFormatString = String(format: "%08X", pixelFormat)
        
        print("SanskyStream Video Frame Captured:")
        print(" - Dimensions: \(width)x\(height)")
        print(" - Timestamp : \(timestamp.value) / \(timestamp.timescale)")
        print(" - Format    : \(pixelFormatString)")
        
        // IMPORTANT:
        // No copies are made.
        // No conversions to UIImage are performed.
        // No disk writes occur.
        // This preserves performance and leaves the buffer intact for future encoding steps.
    }
}

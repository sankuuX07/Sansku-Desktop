import Foundation
import CoreMedia
import VideoToolbox

/// Hardware-accelerated H.264 Video Encoder for real-time streaming.
/// Takes raw ReplayKit CMSampleBuffers and outputs H.264 encoded CMSampleBuffers.
class VideoEncoder {
    
    private var compressionSession: VTCompressionSession?
    
    /// Initializes the compression session if it hasn't been created yet or if dimensions change.
    /// - Parameters:
    ///   - width: Video frame width.
    ///   - height: Video frame height.
    private func setupEncoder(width: Int32, height: Int32) {
        guard compressionSession == nil else { return }
        
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: width,
            height: height,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,
            refcon: nil,
            compressionSessionOut: &compressionSession
        )
        
        guard status == errSecSuccess, let session = compressionSession else {
            print("SanskyStream VideoEncoder: Failed to create VTCompressionSession. Status: \(status)")
            return
        }
        
        // --- Low-Latency & Real-Time Configuration ---
        
        // Real-time encoding priority
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        
        // H.264 Profile: Baseline is typical for lowest latency real-time streaming
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_Baseline_AutoLevel)
        
        // Frame reordering: False to disable B-frames for low latency
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        
        // Target framerate
        let fps: NSNumber = 60
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: fps)
        
        // Keyframe interval (IDR frames) - e.g., every 2 seconds
        let keyframeInterval: NSNumber = 120 // 60fps * 2s
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: keyframeInterval)
        
        // Average Bitrate (e.g., 4 Mbps)
        let averageBitrate: NSNumber = 4_000_000
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: averageBitrate)
        
        // Data rate limits (Bytes per second, seconds)
        // e.g., max 500 KB per second
        let dataRateLimits = [500_000, 1] as CFArray
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_DataRateLimits, value: dataRateLimits)
        
        // Prepare the session
        VTCompressionSessionPrepareToEncodeFrames(session)
        print("SanskyStream VideoEncoder: VTCompressionSession created and configured for \(width)x\(height).")
    }
    
    /// Encodes a raw video sample buffer.
    /// - Parameter sampleBuffer: The raw CMSampleBuffer from ReplayKit.
    func encode(_ sampleBuffer: CMSampleBuffer) {
        guard let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            print("SanskyStream VideoEncoder: Invalid image buffer.")
            return
        }
        
        let width = Int32(CVPixelBufferGetWidth(imageBuffer))
        let height = Int32(CVPixelBufferGetHeight(imageBuffer))
        
        // Setup session dynamically on the first frame
        if compressionSession == nil {
            setupEncoder(width: width, height: height)
        }
        
        guard let session = compressionSession else { return }
        
        let presentationTimeStamp = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let duration = CMSampleBufferGetDuration(sampleBuffer)
        
        // We pass 'self' as a context to the callback block, though in the block-based API
        // we can also capture it in the closure. Using the modern VTCompressionSessionEncodeFrameWithOutputHandler.
        
        var flags: VTEncodeInfoFlags = []
        
        // Encode the frame
        let status = VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: imageBuffer,
            presentationTimeStamp: presentationTimeStamp,
            duration: duration,
            frameProperties: nil,
            infoFlagsOut: &flags
        ) { [weak self] (status: OSStatus, infoFlags: VTEncodeInfoFlags, sampleBuffer: CMSampleBuffer?) in
            self?.handleEncodedFrame(status: status, infoFlags: infoFlags, sampleBuffer: sampleBuffer)
        }
        
        if status != errSecSuccess {
            print("SanskyStream VideoEncoder: VTCompressionSessionEncodeFrame failed. Status: \(status)")
        }
    }
    
    /// The callback for when VideoToolbox finishes encoding a frame.
    private func handleEncodedFrame(status: OSStatus, infoFlags: VTEncodeInfoFlags, sampleBuffer: CMSampleBuffer?) {
        guard status == errSecSuccess, let sampleBuffer = sampleBuffer else {
            // It might fail if the frame was dropped by the encoder
            if status != errSecSuccess {
                print("SanskyStream VideoEncoder: Encoding failed or frame dropped. Status: \(status)")
            }
            return
        }
        
        // 1. Identify Keyframe (IDR) status
        var isKeyframe = false
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[CFString: Any]],
           let attachment = attachments.first {
            // If kCMSampleAttachmentKey_NotSync is NOT present or is false, it's a keyframe (sync frame)
            let notSync = attachment[kCMSampleAttachmentKey_NotSync] as? Bool ?? false
            isKeyframe = !notSync
        } else {
            // If there's no attachment array at all, we assume it's a keyframe
            isKeyframe = true
        }
        
        // 2. Timing information
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        
        // 3. Encoded data length
        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
        let length = CMBlockBufferGetDataLength(dataBuffer)
        
        // 4. Log the availability of the encoded H.264 sample
        // In this milestone, we stop here. We have H.264 data in memory, ready for network transport.
        let frameType = isKeyframe ? "KEYFRAME (IDR)" : "P-FRAME"
        print("SanskyStream Encoded H.264 -> [\(frameType)] | Size: \(length) bytes | PTS: \(pts.value)/\(pts.timescale)")
        
        // For the next milestone, this is where we will extract the elementary stream
        // (SPS/PPS NALUs from the format description, and the frame NALUs from the dataBuffer)
        // and package them for TCP transmission.
    }
    
    /// Invalidates the compression session to free up hardware resources.
    func invalidate() {
        if let session = compressionSession {
            VTCompressionSessionInvalidate(session)
            compressionSession = nil
            print("SanskyStream VideoEncoder: Session invalidated.")
        }
    }
}

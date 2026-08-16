import Foundation
import CoreMedia
import VideoToolbox

/// Hardware-accelerated H.264 Video Encoder for real-time streaming.
/// Takes raw ReplayKit CMSampleBuffers and outputs H.264 encoded data
/// via the onEncodedFrame callback for the M6 transport layer.
class VideoEncoder {

    private var compressionSession: VTCompressionSession?

    // -----------------------------------------------------------------------
    // M6 transport callback.
    // Invoked from the VideoToolbox callback queue whenever an encoded frame
    // is ready.  The caller (SampleHandler) wires this to VideoTransport.
    //
    // Parameters:
    //   naluData:       Raw H.264 bytes in Annex B format.
    //                   Keyframes have SPS + PPS + IDR NALUs prepended.
    //   presentationUs: PTS in microseconds.
    //   isKeyframe:     True if this is an IDR / keyframe.
    // -----------------------------------------------------------------------
    var onEncodedFrame: ((_ naluData: Data, _ presentationUs: UInt64, _ isKeyframe: Bool) -> Void)?

    // -----------------------------------------------------------------------
    // Encoder setup
    // -----------------------------------------------------------------------

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

    // -----------------------------------------------------------------------
    // Encode
    // -----------------------------------------------------------------------

    /// Encodes a raw video sample buffer.
    /// - Parameter sampleBuffer: The raw CMSampleBuffer from ReplayKit.
    func encode(_ sampleBuffer: CMSampleBuffer) {
        guard let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            print("SanskyStream VideoEncoder: Invalid image buffer.")
            return
        }

        let width  = Int32(CVPixelBufferGetWidth(imageBuffer))
        let height = Int32(CVPixelBufferGetHeight(imageBuffer))

        // Setup session dynamically on the first frame
        if compressionSession == nil {
            setupEncoder(width: width, height: height)
        }

        guard let session = compressionSession else { return }

        let presentationTimeStamp = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let duration              = CMSampleBufferGetDuration(sampleBuffer)

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

    // -----------------------------------------------------------------------
    // Encoded-frame callback — M6 extraction and dispatch
    // -----------------------------------------------------------------------

    /// The callback for when VideoToolbox finishes encoding a frame.
    private func handleEncodedFrame(status: OSStatus, infoFlags: VTEncodeInfoFlags, sampleBuffer: CMSampleBuffer?) {
        guard status == errSecSuccess, let sampleBuffer = sampleBuffer else {
            if status != errSecSuccess {
                print("SanskyStream VideoEncoder: Encoding failed or frame dropped. Status: \(status)")
            }
            return
        }

        // 1. Identify Keyframe (IDR) status
        var isKeyframe = false
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer,
                                                                     createIfNecessary: false) as? [[CFString: Any]],
           let attachment = attachments.first {
            // If kCMSampleAttachmentKey_NotSync is NOT present or is false, it is a keyframe.
            let notSync = attachment[kCMSampleAttachmentKey_NotSync] as? Bool ?? false
            isKeyframe = !notSync
        } else {
            // No attachment array — assume keyframe (first frame from encoder).
            isKeyframe = true
        }

        // 2. Presentation timestamp → microseconds
        let pts           = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let presentationUs = UInt64(pts.value) * 1_000_000 / UInt64(max(pts.timescale, 1))

        // 3. Extract H.264 NALU bytes in Annex B format
        guard let naluData = extractAnnexBNALUs(from: sampleBuffer, isKeyframe: isKeyframe) else {
            print("SanskyStream VideoEncoder: Failed to extract NALU data.")
            return
        }

        // 4. Log
        let frameType = isKeyframe ? "KEYFRAME (IDR)" : "P-FRAME"
        print("SanskyStream Encoded H.264 -> [\(frameType)] | Size: \(naluData.count) bytes | PTS: \(presentationUs) µs")

        // 5. Dispatch to the transport layer (M6)
        onEncodedFrame?(naluData, presentationUs, isKeyframe)
    }

    // -----------------------------------------------------------------------
    // NALU extraction: AVCC → Annex B
    // -----------------------------------------------------------------------

    /// Extracts H.264 NALUs from a VideoToolbox-produced CMSampleBuffer and
    /// returns them serialized in Annex B format (0x00 0x00 0x00 0x01 start codes).
    ///
    /// VideoToolbox outputs H.264 in AVCC format: each NALU is preceded by a
    /// 4-byte big-endian length field.  We convert every NALU to Annex B by
    /// replacing the length prefix with the start code, which is the standard
    /// elementary stream format required by M7's decoder and widely expected by
    /// network receivers.
    ///
    /// For keyframes, the SPS and PPS NALUs are prepended first (they are stored
    /// in the CMFormatDescription, not in the CMBlockBuffer).
    private func extractAnnexBNALUs(from sampleBuffer: CMSampleBuffer, isKeyframe: Bool) -> Data? {
        var naluData = Data()

        // For keyframes: extract SPS and PPS from the format description.
        if isKeyframe, let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer) {
            var paramSetCount = 0
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                formatDescription,
                parameterSetIndex: 0,
                parameterSetPointerOut: nil,
                parameterSetSizeOut: nil,
                parameterSetCountOut: &paramSetCount,
                nalUnitHeaderLengthOut: nil
            )

            let annexBStartCode: [UInt8] = [0x00, 0x00, 0x00, 0x01]

            for i in 0 ..< paramSetCount {
                var paramSetPtr: UnsafePointer<UInt8>?
                var paramSetSize = 0
                let err = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    formatDescription,
                    parameterSetIndex: i,
                    parameterSetPointerOut: &paramSetPtr,
                    parameterSetSizeOut: &paramSetSize,
                    parameterSetCountOut: nil,
                    nalUnitHeaderLengthOut: nil
                )
                if err == noErr, let ptr = paramSetPtr, paramSetSize > 0 {
                    naluData.append(contentsOf: annexBStartCode)
                    naluData.append(ptr, count: paramSetSize)
                }
            }
        }

        // Extract frame NALUs from the CMBlockBuffer (AVCC → Annex B).
        guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return nil }
        let totalLength = CMBlockBufferGetDataLength(blockBuffer)
        guard totalLength > 0 else { return nil }

        var rawData = Data(count: totalLength)
        let copyStatus = rawData.withUnsafeMutableBytes { rawBuf -> OSStatus in
            guard let ptr = rawBuf.baseAddress else { return kCMBlockBufferBadCustomBlockSourceErr }
            return CMBlockBufferCopyDataBytes(blockBuffer, atOffset: 0, dataLength: totalLength, destination: ptr)
        }
        guard copyStatus == noErr else { return nil }

        // Walk the AVCC byte stream.
        var offset = 0
        let annexBStartCode: [UInt8] = [0x00, 0x00, 0x00, 0x01]

        while offset + 4 <= totalLength {
            // AVCC 4-byte big-endian NALU length.
            let naluLength = Int(rawData[offset])     << 24 |
                             Int(rawData[offset + 1]) << 16 |
                             Int(rawData[offset + 2]) <<  8 |
                             Int(rawData[offset + 3])

            offset += 4

            guard naluLength > 0, offset + naluLength <= totalLength else { break }

            naluData.append(contentsOf: annexBStartCode)
            naluData.append(rawData[offset ..< offset + naluLength])

            offset += naluLength
        }

        return naluData.isEmpty ? nil : naluData
    }

    // -----------------------------------------------------------------------
    // Teardown
    // -----------------------------------------------------------------------

    /// Invalidates the compression session to free up hardware resources.
    func invalidate() {
        if let session = compressionSession {
            VTCompressionSessionInvalidate(session)
            compressionSession = nil
            print("SanskyStream VideoEncoder: Session invalidated.")
        }
    }
}

import Foundation
import CoreMedia
import CoreAudio

// ============================================================================
// AudioCapture — M9: iPhone Audio Capture Foundation
//
// Responsibilities (M9 scope only):
//   - Receive an audio CMSampleBuffer from ReplayKit (.audioApp)
//   - Validate the sample buffer
//   - Extract and log the audio format description
//   - Read actual sample rate, channel count, sample count, PCM sub-format
//   - Preserve the original ReplayKit presentation timestamp
//   - Copy the raw PCM bytes into a bounded in-memory AudioFrame
//   - Expose frames via the onAudioFrame callback
//
// Explicitly out-of-scope for M9:
//   - Audio encoding   (→ M10)
//   - Network transport (→ M10)
//   - Windows playback  (→ M11)
//   - A/V synchronization (→ M12)
//   - Microphone capture (not required for SanskyStream game/app audio)
//
// Thread-safety:
//   ReplayKit delivers buffers on its own internal queue.
//   AudioCapture is designed to be called from that single queue only.
//   The onAudioFrame callback is invoked synchronously on the same queue.
//   The bounded drop buffer (audioBuffer) is protected by a simple lock.
// ============================================================================

// ----------------------------------------------------------------------------
// AudioFormatInfo — detected audio stream parameters
// ----------------------------------------------------------------------------

/// Describes the audio format read directly from the CMSampleBuffer.
/// No assumed defaults — every field is validated from the actual descriptor.
struct AudioFormatInfo {
    /// Linear PCM sample rate in Hz (e.g. 44100.0, 48000.0).
    let sampleRate: Double

    /// Number of interleaved audio channels (e.g. 1 = mono, 2 = stereo).
    let channelCount: UInt32

    /// Number of valid bits per channel (e.g. 16, 32).
    let bitsPerChannel: UInt32

    /// Whether the samples are floating-point (true) or integer (false).
    let isFloat: Bool

    /// Whether the byte order is big-endian (true) or little-endian (false).
    let isBigEndian: Bool

    /// Whether integer samples are signed (true).
    let isSignedInteger: Bool

    /// Raw AudioStreamBasicDescription format flags for diagnostics.
    let rawFormatFlags: AudioFormatFlags

    var description: String {
        let typeStr  = isFloat ? "Float\(bitsPerChannel)" : (isSignedInteger ? "SInt\(bitsPerChannel)" : "UInt\(bitsPerChannel)")
        let endian   = isBigEndian ? "BE" : "LE"
        return "\(typeStr)\(endian) \(Int(sampleRate))Hz \(channelCount)ch"
    }
}

// ----------------------------------------------------------------------------
// AudioFrame — one processed audio buffer delivered by ReplayKit
// ----------------------------------------------------------------------------

/// A single validated, in-memory audio frame extracted from a CMSampleBuffer.
struct AudioFrame {
    /// Validated format description for this frame.
    let format: AudioFormatInfo

    /// Number of audio samples in this frame (per channel).
    let sampleCount: Int

    /// Original ReplayKit presentation timestamp (CMTime).
    /// Must be preserved exactly for M12 A/V synchronisation.
    let presentationTime: CMTime

    /// Raw interleaved PCM bytes, copied once from the CMBlockBuffer.
    /// Layout matches `format`: channel count, bit depth, byte order above.
    /// For M10 encoding pass this directly to the audio encoder.
    let pcmData: Data
}

// ----------------------------------------------------------------------------
// AudioCapture
// ----------------------------------------------------------------------------

final class AudioCapture {

    // -------------------------------------------------------------------------
    // Bounded drop buffer
    //
    // Keeps at most MAX_BUFFERED_FRAMES frames in memory.
    // In real-time operation the consumer (M10 encoder) should drain this
    // faster than ReplayKit produces frames.  If it falls behind the oldest
    // frame is silently dropped — losing audio is always preferable to
    // accumulating unbounded memory or blocking the ReplayKit callback.
    // -------------------------------------------------------------------------

    private static let MAX_BUFFERED_FRAMES = 8

    // Lock protecting audioBuffer; lightweight since contention is minimal.
    private let bufferLock = NSLock()
    private var audioBuffer: [AudioFrame] = []

    // -------------------------------------------------------------------------
    // Format-change detection
    //
    // ReplayKit may change the audio format mid-session (e.g. on device
    // rotation or app reconfiguration).  We cache the last seen ASBD so we
    // can log a warning when the format changes without crashing.
    // -------------------------------------------------------------------------

    private var lastKnownASBD: AudioStreamBasicDescription?

    // -------------------------------------------------------------------------
    // Statistics (diagnostic only, not transmitted)
    // -------------------------------------------------------------------------

    private var framesReceived:  UInt64 = 0
    private var framesDropped:   UInt64 = 0
    private var framesInvalid:   UInt64 = 0

    // -------------------------------------------------------------------------
    // Callback
    //
    // Invoked synchronously on the ReplayKit sample-handler queue for every
    // successfully validated frame.  M10 will replace or extend this wire-up.
    // -------------------------------------------------------------------------

    /// Called with each validated AudioFrame in presentation-time order.
    /// The callback receives ownership of the frame; no further copies needed.
    var onAudioFrame: ((_ frame: AudioFrame) -> Void)?

    // =========================================================================
    // MARK: - Public entry point
    // =========================================================================

    /// Process one application/system audio CMSampleBuffer from ReplayKit.
    ///
    /// - Parameter sampleBuffer: The `.audioApp` buffer from processSampleBuffer.
    ///
    /// Call this ONLY for `.audioApp` buffers.
    /// Microphone (`.audioMic`) buffers must NOT be passed here.
    func processAppAudioBuffer(_ sampleBuffer: CMSampleBuffer) {
        framesReceived += 1

        // ------------------------------------------------------------------
        // Step 1 — validate the sample buffer itself
        // ------------------------------------------------------------------

        guard CMSampleBufferIsValid(sampleBuffer) else {
            framesInvalid += 1
            print("SanskyStream AudioCapture: Received invalid CMSampleBuffer — skipping.")
            return
        }

        guard CMSampleBufferDataIsReady(sampleBuffer) else {
            framesInvalid += 1
            print("SanskyStream AudioCapture: CMSampleBuffer data is not ready — skipping.")
            return
        }

        // ------------------------------------------------------------------
        // Step 2 — obtain and validate the format description
        // ------------------------------------------------------------------

        guard let formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer) else {
            framesInvalid += 1
            print("SanskyStream AudioCapture: Missing format description — skipping.")
            return
        }

        // ------------------------------------------------------------------
        // Step 3 — extract AudioStreamBasicDescription
        // ------------------------------------------------------------------

        guard let asbd = extractASBD(from: formatDesc) else {
            framesInvalid += 1
            return
        }

        // ------------------------------------------------------------------
        // Step 4 — validate format fields
        //          Do not assume sample rate, channel count, or PCM sub-type.
        // ------------------------------------------------------------------

        guard asbd.mSampleRate > 0 else {
            framesInvalid += 1
            print("SanskyStream AudioCapture: Zero sample rate — skipping.")
            return
        }

        guard asbd.mChannelsPerFrame > 0 else {
            framesInvalid += 1
            print("SanskyStream AudioCapture: Zero channel count — skipping.")
            return
        }

        guard asbd.mBitsPerChannel > 0 else {
            framesInvalid += 1
            print("SanskyStream AudioCapture: Zero bits-per-channel — skipping.")
            return
        }

        // ------------------------------------------------------------------
        // Step 5 — detect format changes mid-session
        // ------------------------------------------------------------------

        if let prev = lastKnownASBD, !asbdEquals(prev, asbd) {
            print("SanskyStream AudioCapture: Warning: Audio format changed mid-session.")
            print("  Old: \(formatDescription(prev))")
            print("  New: \(formatDescription(asbd))")
        }
        lastKnownASBD = asbd

        // ------------------------------------------------------------------
        // Step 6 — build AudioFormatInfo from the actual ASBD
        // ------------------------------------------------------------------

        let flags           = asbd.mFormatFlags
        let isFloat         = (flags & kAudioFormatFlagIsFloat)         != 0
        let isBigEndian     = (flags & kAudioFormatFlagIsBigEndian)     != 0
        let isSignedInteger = (flags & kAudioFormatFlagIsSignedInteger) != 0

        let formatInfo = AudioFormatInfo(
            sampleRate:       asbd.mSampleRate,
            channelCount:     asbd.mChannelsPerFrame,
            bitsPerChannel:   asbd.mBitsPerChannel,
            isFloat:          isFloat,
            isBigEndian:      isBigEndian,
            isSignedInteger:  isSignedInteger,
            rawFormatFlags:   flags
        )

        // ------------------------------------------------------------------
        // Step 7 — sample count (per channel)
        // ------------------------------------------------------------------

        let sampleCount = CMSampleBufferGetNumSamples(sampleBuffer)
        guard sampleCount > 0 else {
            framesInvalid += 1
            print("SanskyStream AudioCapture: Zero sample count — skipping.")
            return
        }

        // ------------------------------------------------------------------
        // Step 8 — preserve original presentation timestamp
        //          Do NOT replace with current time or callback arrival time.
        // ------------------------------------------------------------------

        let presentationTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        guard presentationTime != .invalid else {
            framesInvalid += 1
            print("SanskyStream AudioCapture: Invalid presentation timestamp — skipping.")
            return
        }

        // ------------------------------------------------------------------
        // Step 9 — copy raw PCM bytes from the CMBlockBuffer
        //          One copy is unavoidable to hand ownership to AudioFrame.
        // ------------------------------------------------------------------

        guard let pcmData = extractPCMBytes(from: sampleBuffer) else {
            framesInvalid += 1
            return
        }

        // ------------------------------------------------------------------
        // Step 10 — assemble AudioFrame
        // ------------------------------------------------------------------

        let frame = AudioFrame(
            format:           formatInfo,
            sampleCount:      sampleCount,
            presentationTime: presentationTime,
            pcmData:          pcmData
        )

        // ------------------------------------------------------------------
        // Step 11 — deliver via callback (fast path) and enqueue in buffer
        // ------------------------------------------------------------------

        // Deliver immediately on the ReplayKit queue — M10 will consume here.
        onAudioFrame?(frame)

        // Also push into the bounded buffer for any pull-based consumer.
        enqueue(frame)

        // Periodic diagnostic log (every 300 frames ~= ~6 s at 48 kHz/1024 samples)
        if framesReceived % 300 == 1 {
            print("SanskyStream AudioCapture: \(framesReceived) frames | \(framesDropped) dropped | \(framesInvalid) invalid | \(formatInfo.description) | \(sampleCount) samples/frame | PTS \(presentationTime.seconds)s")
        }
    }

    // =========================================================================
    // MARK: - Pull-based consumer API (optional)
    // =========================================================================

    /// Dequeue the oldest AudioFrame, or nil if the buffer is empty.
    /// Safe to call from any thread.
    func dequeue() -> AudioFrame? {
        bufferLock.lock()
        defer { bufferLock.unlock() }
        guard !audioBuffer.isEmpty else { return nil }
        return audioBuffer.removeFirst()
    }

    /// Current number of frames waiting in the bounded buffer.
    var bufferedFrameCount: Int {
        bufferLock.lock()
        defer { bufferLock.unlock() }
        return audioBuffer.count
    }

    // =========================================================================
    // MARK: - Diagnostics
    // =========================================================================

    /// Log a summary of audio format and statistics to the console.
    func logDiagnostics() {
        let fmt = lastKnownASBD.map { formatDescription($0) } ?? "none"
        print("SanskyStream AudioCapture Diagnostics:")
        print("  Frames received : \(framesReceived)")
        print("  Frames invalid  : \(framesInvalid)")
        print("  Frames dropped  : \(framesDropped)")
        print("  Last format     : \(fmt)")
        print("  Buffer depth    : \(bufferedFrameCount)/\(AudioCapture.MAX_BUFFERED_FRAMES)")
    }

    // =========================================================================
    // MARK: - Private helpers
    // =========================================================================

    /// Enqueue a frame into the bounded drop buffer.
    /// If the buffer is full, the oldest (stale) frame is discarded.
    private func enqueue(_ frame: AudioFrame) {
        bufferLock.lock()
        defer { bufferLock.unlock() }

        if audioBuffer.count >= AudioCapture.MAX_BUFFERED_FRAMES {
            audioBuffer.removeFirst()   // drop oldest stale frame
            framesDropped += 1
        }
        audioBuffer.append(frame)
    }

    // -------------------------------------------------------------------------
    // Extract AudioStreamBasicDescription from a CMFormatDescription.
    // -------------------------------------------------------------------------

    private func extractASBD(from formatDesc: CMFormatDescription) -> AudioStreamBasicDescription? {
        // CMAudioFormatDescriptionGetStreamBasicDescription returns a pointer
        // to the ASBD owned by the format description object.
        guard let asbdPtr = CMAudioFormatDescriptionGetStreamBasicDescription(formatDesc) else {
            print("SanskyStream AudioCapture: Could not obtain AudioStreamBasicDescription.")
            return nil
        }

        let asbd = asbdPtr.pointee

        // Sanity check: must be linear PCM.
        // ReplayKit always delivers raw PCM for app audio.
        guard asbd.mFormatID == kAudioFormatLinearPCM else {
            print("SanskyStream AudioCapture: Unexpected audio format ID: \(fourCC(asbd.mFormatID)) — expected LPCM.")
            return nil
        }

        return asbd
    }

    // -------------------------------------------------------------------------
    // Copy raw PCM bytes from CMBlockBuffer into a Data value.
    // -------------------------------------------------------------------------

    private func extractPCMBytes(from sampleBuffer: CMSampleBuffer) -> Data? {
        guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else {
            print("SanskyStream AudioCapture: No CMBlockBuffer in sample buffer.")
            return nil
        }

        let totalLength = CMBlockBufferGetDataLength(blockBuffer)
        guard totalLength > 0 else {
            print("SanskyStream AudioCapture: Empty CMBlockBuffer.")
            return nil
        }

        var pcmData = Data(count: totalLength)
        let status = pcmData.withUnsafeMutableBytes { rawBuf -> OSStatus in
            guard let ptr = rawBuf.baseAddress else {
                return kCMBlockBufferBadCustomBlockSourceErr
            }
            return CMBlockBufferCopyDataBytes(
                blockBuffer,
                atOffset: 0,
                dataLength: totalLength,
                destination: ptr
            )
        }

        guard status == noErr else {
            print("SanskyStream AudioCapture: CMBlockBufferCopyDataBytes failed. Status: \(status)")
            return nil
        }

        return pcmData
    }

    // -------------------------------------------------------------------------
    // Utility: readable format string from an ASBD.
    // -------------------------------------------------------------------------

    private func formatDescription(_ asbd: AudioStreamBasicDescription) -> String {
        let flags   = asbd.mFormatFlags
        let isFloat = (flags & kAudioFormatFlagIsFloat)         != 0
        let isBE    = (flags & kAudioFormatFlagIsBigEndian)     != 0
        let isSInt  = (flags & kAudioFormatFlagIsSignedInteger) != 0
        let type    = isFloat ? "Float" : (isSInt ? "SInt" : "UInt")
        let endian  = isBE ? "BE" : "LE"
        return "\(type)\(asbd.mBitsPerChannel)\(endian) \(Int(asbd.mSampleRate))Hz \(asbd.mChannelsPerFrame)ch"
    }

    // -------------------------------------------------------------------------
    // Utility: compare two ASBDs for equality on the fields we care about.
    // -------------------------------------------------------------------------

    private func asbdEquals(_ a: AudioStreamBasicDescription,
                             _ b: AudioStreamBasicDescription) -> Bool {
        return a.mSampleRate       == b.mSampleRate       &&
               a.mChannelsPerFrame == b.mChannelsPerFrame &&
               a.mBitsPerChannel   == b.mBitsPerChannel   &&
               a.mFormatID         == b.mFormatID         &&
               a.mFormatFlags      == b.mFormatFlags
    }

    // -------------------------------------------------------------------------
    // Utility: convert a four-byte OSType to a human-readable string.
    // -------------------------------------------------------------------------

    private func fourCC(_ value: UInt32) -> String {
        let bytes: [UInt8] = [
            UInt8((value >> 24) & 0xFF),
            UInt8((value >> 16) & 0xFF),
            UInt8((value >>  8) & 0xFF),
            UInt8( value        & 0xFF)
        ]
        let allPrintable = bytes.allSatisfy { $0 >= 0x20 && $0 < 0x7F }
        if allPrintable {
            return "'" + bytes.map { String(UnicodeScalar($0)) }.joined() + "'"
        }
        return "0x" + String(value, radix: 16, uppercase: true)
    }
}

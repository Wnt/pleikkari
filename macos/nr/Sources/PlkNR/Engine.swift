import CPlkNR
import CoreVideo
import DLSSCore
import DLSSMLX
import Foundation

/// One loaded model and the single frame it may have in flight.
///
/// The engine never queues: `submit` takes a frame only when the previous one has left,
/// so whoever offers every decoded frame gets the newest result the GPU can deliver and
/// is never made to wait for it.
final class Engine: @unchecked Sendable {
  struct Controls {
    var intensity: Float
    var detail: Float
    var colour: Float
    var profile: NeuralRenderingControlProfile
    var temporal: Bool

    init(_ c: plk_nr_controls) {
      func clamped(_ value: Float, _ range: ClosedRange<Float>, otherwise: Float) -> Float {
        value.isFinite ? min(max(value, range.lowerBound), range.upperBound) : otherwise
      }
      intensity = clamped(c.intensity, 0...2, otherwise: 1)
      detail = clamped(c.detail_strength, 0...8, otherwise: 1)
      colour = clamped(c.colour_strength, 0...4, otherwise: 1)
      switch c.profile {
      case Int32(PLK_NR_PROFILE_NATURAL): profile = .natural
      case Int32(PLK_NR_PROFILE_CINEMATIC): profile = .cinematic
      case Int32(PLK_NR_PROFILE_NEUTRAL): profile = .neutral
      default: profile = .standard
      }
      temporal = c.temporal != 0
    }
  }

  /// The callback and its context cross into the frame's task.
  private struct Completion: @unchecked Sendable {
    let callback: plk_nr_callback
    let context: UnsafeMutableRawPointer?

    func call(seq: UInt64, output: MLXPixelBuffer?, milliseconds: Double, error: String?) {
      func deliver(_ message: UnsafePointer<CChar>?) {
        var result = plk_nr_result(
          seq: seq, output: output.map { Unmanaged.passUnretained($0.buffer) },
          total_ms: milliseconds, error: message)
        callback(context, &result)
      }
      if let error { error.withCString { deliver($0) } } else { deliver(nil) }
    }
  }

  private let backend: MLXNeuralRenderingDeviceTemporalBackend
  private let state = NSCondition()  // guards busy, inFlight, closed, controls
  private var busy = false           // a frame is being rendered: no other is taken
  private var inFlight = 0           // frames rendered or being delivered: close() waits for them
  private var closed = false
  private let delivery = NSLock()    // one callback at a time
  private var controls: Controls
  // Touched only by the frame in flight.
  private var writer: (width: Int, height: Int, half: Bool, writer: MLXPixelBufferWriter)?
  private var frameIndex: UInt64 = 0
  let summary: UnsafeMutablePointer<CChar>

  init(modelURL: URL, halfPrecision: Bool, controls: Controls) throws {
    backend = try MLXNeuralRenderingDeviceTemporalBackend(
      packageURL: modelURL, executionMode: .metalFused,
      computePrecision: halfPrecision ? .float16 : .float32,
      controlMaskIntensity: controls.intensity,
      featureControls: controls.profile.featureControls)
    self.controls = controls
    summary = strdup(Self.describe(modelURL: modelURL, halfPrecision: halfPrecision))
  }

  deinit { free(summary) }

  /// The network is named by its block count: both known ones share one package format.
  private static func describe(modelURL: URL, halfPrecision: Bool) -> String {
    var blocks = 0
    var synthetic = false
    if let data = try? Data(contentsOf: modelURL.appendingPathComponent("manifest.json")),
      let manifest = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    {
      synthetic = (manifest["identifier"] as? String)?.contains("synthetic") == true
      let tensors = (manifest["weights"] as? [String: Any])?["tensors"] as? [[String: Any]] ?? []
      for tensor in tensors {
        guard let name = tensor["name"] as? String, name.hasPrefix("block"),
          let dot = name.firstIndex(of: "."),
          let index = Int(name[name.index(name.startIndex, offsetBy: 5)..<dot])
        else { continue }
        blocks = max(blocks, index + 1)
      }
    }
    let network = blocks == 31 ? "small" : blocks == 71 ? "HT" : "unknown"
    return "\(modelURL.lastPathComponent): \(network) network, \(blocks) blocks, "
      + (halfPrecision ? "float16" : "float32")
      + (synthetic ? ", SYNTHETIC weights (the picture is noise)" : "")
  }

  func setControls(_ new: Controls) {
    state.lock()
    controls = new
    state.unlock()
  }

  func submit(
    _ buffer: CVPixelBuffer, seq: UInt64, reset: Bool, callback: plk_nr_callback,
    context: UnsafeMutableRawPointer?
  ) -> Bool {
    let format = CVPixelBufferGetPixelFormatType(buffer)
    guard format == kCVPixelFormatType_32BGRA || format == kCVPixelFormatType_64RGBAHalf,
      CVPixelBufferGetIOSurface(buffer) != nil
    else { return false }
    state.lock()
    if busy || closed {
      state.unlock()
      return false
    }
    busy = true
    inFlight += 1
    let controls = self.controls
    state.unlock()

    let input = MLXPixelBuffer(buffer)
    let completion = Completion(callback: callback, context: context)
    let started = DispatchTime.now().uptimeNanoseconds
    Task.detached(priority: .userInitiated) { [self] in
      var output: MLXPixelBuffer?
      var failure: String?
      do {
        output = try await render(input, controls: controls, reset: reset)
      } catch {
        failure = (error as? LocalizedError)?.errorDescription ?? String(describing: error)
      }
      let milliseconds = Double(DispatchTime.now().uptimeNanoseconds - started) / 1e6
      // Idle before the result is delivered: whoever learns of it may offer the next
      // frame at once, from the callback or from another thread, and is not refused.
      rendered()
      deliver(completion, seq: seq, output: output, milliseconds: milliseconds, error: failure)
    }
    return true
  }

  /// The frame's picture is made: the engine takes the next frame from here on.
  private func rendered() {
    state.lock()
    busy = false
    state.unlock()
  }

  /// Calls the callback, one at a time, and then counts the frame as gone.
  private func deliver(
    _ completion: Completion, seq: UInt64, output: MLXPixelBuffer?, milliseconds: Double,
    error: String?
  ) {
    delivery.lock()
    completion.call(seq: seq, output: output, milliseconds: milliseconds, error: error)
    delivery.unlock()
    state.lock()
    inFlight -= 1
    state.broadcast()
    state.unlock()
  }

  private func render(_ input: MLXPixelBuffer, controls: Controls, reset: Bool) async throws
    -> MLXPixelBuffer
  {
    let width = input.width
    let height = input.height
    let frame = try MLXVideoFrame(pixelBuffer: input)
    let options = try MLXVideoOutputOptions(
      width: width, height: height, detailStrength: controls.detail,
      colourStrength: controls.colour)
    // The frames the engine is given are not consecutive frames of the stream (it takes
    // one only when idle), but they are consecutive for the network: its history is the
    // previous frame it rendered. No motion vectors exist for a video stream, so the
    // history is reprojected with zero motion and the network's own per-pixel blend
    // decides how much of it survives.
    let context = NeuralRenderFrameContext(
      streamID: 1, frameIndex: frameIndex, discontinuity: reset ? .sceneCut : nil)
    frameIndex += 1
    let rendered = try await backend.renderVideoFrame(
      frame, motion: nil, context: context, processingScale: 1, temporal: controls.temporal,
      outputOptions: options, featureControls: controls.profile.featureControls,
      intensity: controls.intensity)
    // The result has the input's pixel format: 8 bits for 8 bits, half float for half float.
    let half = CVPixelBufferGetPixelFormatType(input.buffer) == kCVPixelFormatType_64RGBAHalf
    if writer?.width != width || writer?.height != height || writer?.half != half {
      writer = (
        width, height, half,
        try MLXPixelBufferWriter(width: width, height: height, halfOutput: half)
      )
    }
    return try await writer!.writer.write(rendered)
  }

  /// No frame is taken after this; returns when every frame taken has delivered.
  func close() {
    state.lock()
    closed = true
    while inFlight > 0 { state.wait() }
    state.unlock()
  }
}
